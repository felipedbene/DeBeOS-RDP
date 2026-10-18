#include <string_view>
#include "haiku_remote/text_engine.hpp"

#include "haiku_remote/font_fetch.hpp"

#include "agg_port.hpp"
#include "painter.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include FT_LCD_FILTER_H
#include FT_MODULE_H

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <bit>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace haiku_remote {
namespace {

// headers/os/interface/Font.h:80-89. Only the bits that pick a *style* matter
// here; the decoration bits (underscore, negative, outlined, strikeout) are
// applied at draw time by app_server and change no advance.
constexpr std::uint16_t italic_face = 0x0001;
constexpr std::uint16_t bold_face = 0x0020;
constexpr std::uint16_t condensed_face = 0x0080;
// The two decorations AGGTextRenderer draws (Font.h:81, :84;
// AGGTextRenderer.cpp:171-182).
constexpr std::uint16_t underscore_face = 0x0002;
constexpr std::uint16_t strikeout_face = 0x0010;
// Weight bits that Haiku selects a style with -- GetStyleMatchingFace() masks
// them in alongside bold/italic/condensed (src/servers/app/font/
// FontFamily.cpp:224-226) -- but that this client has no file axis for. They
// are named in the diagnostic below instead of being dropped in silence.
constexpr std::uint16_t light_face = 0x0100;
constexpr std::uint16_t heavy_face = 0x0200;
// B_DISABLE_ANTIALIASING, headers/os/interface/Font.h:39.
constexpr std::uint32_t disable_antialiasing = 0x00000001u;

// RP_SET_FONT carries `shear`, `rotation` and `false_bold_width` -- app_server
// honours all three locally (src/servers/app/drawing/Painter/
// AGGTextRenderer.cpp SetFont) and there is no server-side fallback that
// rasterises transformed text for a remote view, so if the client ignores them
// the attributes are simply lost.
//
// `rotation` is degrees counter-clockwise and `shear` is degrees with 90 as
// neutral. Both leave the *layout* alone: Haiku lays glyphs out along an
// unrotated horizontal baseline and then pushes the whole positioned outline
// through one embedded transform (AGGTextRenderer::StringRenderer passes the
// untransformed x/y to InitAdaptors and transforms the resulting path). Since
// transform(G + v) == transform(G) + transform(v), transforming the glyph
// outline via FT_Set_Transform and transforming the accumulated baseline
// offset is the same composition.
bool font_is_transformed(const Font& font)
{
    return font.rotation != 0.0f || font.shear != neutral_font_shear;
}

constexpr double ft_fixed_one = 65536.0;

// RP_SET_TRANSFORM's six floats reach DrawState::transform unvalidated
// (RemoteMessage.cpp AddTransform writes them verbatim), so fold NaN to zero and
// clamp the magnitude before the cast: converting a double outside FT_Fixed's
// range is undefined behaviour, which the cos/sin callers below could never hit
// but a wire-supplied scale can.
constexpr double ft_matrix_limit = 4096.0;

FT_Fixed to_ft_fixed(double value)
{
    if (std::isnan(value))
        return 0;
    return static_cast<FT_Fixed>(
        std::clamp(value, -ft_matrix_limit, ft_matrix_limit) * ft_fixed_one);
}

// Mirrors ServerFont::GetTransformedFace() (src/servers/app/ServerFont.cpp):
// a rotation matrix times a shear matrix, in FreeType 16.16 fixed point. At
// shear == 90 the shear matrix is exactly the identity, because cos(90) rounds
// to 0 in 16.16 -- so a font with no rotation and neutral shear is untouched.
FT_Matrix font_matrix(const Font& font)
{
    const double degrees_to_radians = std::numbers::pi / 180.0;
    const double rotation = static_cast<double>(font.rotation)
        * degrees_to_radians;
    FT_Matrix rotate;
    rotate.xx = to_ft_fixed(std::cos(rotation));
    rotate.xy = to_ft_fixed(-std::sin(rotation));
    rotate.yx = to_ft_fixed(std::sin(rotation));
    rotate.yy = to_ft_fixed(std::cos(rotation));

    const double shear = static_cast<double>(font.shear) * degrees_to_radians;
    FT_Matrix result;
    result.xx = to_ft_fixed(1.0);
    result.xy = to_ft_fixed(-std::cos(shear));
    result.yx = 0;
    result.yy = to_ft_fixed(1.0);

    // FT_Matrix_Multiply(a, b) computes `b = a * b`.
    FT_Matrix_Multiply(&rotate, &result);
    return result;
}

// Whether the view transform does anything a translation cannot. This is the
// client's half of StringRenderer::NeedsVector(), which is
// `!fTransform.IsTranslationOnly()` over the combined embedded * baseline *
// view transform (AGGTextRenderer.cpp:146): a translation-only transform leaves
// the server on its hinted-bitmap fast path, offset by transform(0,0)
// (AGGTextRenderer.cpp:229-236), and leaves this client's glyphs untouched too.
bool view_is_linear_identity(const Transform& transform)
{
    return transform.sx == 1 && transform.shy == 0
        && transform.shx == 0 && transform.sy == 1;
}

// The resource bounds on a transformed vector glyph (TextEngine::draw): no
// string is legible on a Surface::max_dimension canvas past a 4096x view
// scale, and no glyph past twice the canvas.
constexpr double max_vector_scale = 4096.0;
constexpr double max_vector_extent = 2.0 * Surface::max_dimension;

// The three ways app_server can turn a glyph into pixels, as FontCacheEntry
// names them (glyph_rendering, src/servers/app/font/FontCacheEntry.h). The
// fourth, glyph_ren_native_mono, is never chosen by _RenderTypeFor.
enum class GlyphRendering { subpix, gray8, outline };

// The two rendering inputs that are desktop settings rather than font record
// fields. Without an identity block they take the values DesktopSettings
// starts from (src/servers/app/DesktopSettings.cpp:88-89: subpixel
// antialiasing on, HINTING_MODE_ON); with one, the server says what they are
// for this font (RemoteMessage::AddFontIdentity).
struct RenderSettings {
    bool hinting = true;
    bool subpixel = true;
};

RenderSettings render_settings(const Font& font)
{
    if (font.identity == nullptr)
        return {};
    return {font.identity->hinting, font.identity->subpixel};
}

bool font_antialiased(const Font& font)
{
    return (font.flags & disable_antialiasing) == 0;
}

// FontCacheEntry::_RenderTypeFor (src/servers/app/font/FontCacheEntry.cpp:
// 423-438), condition for condition. `force_vector` is the consumer's
// NeedsVector(): for drawing that is a view transform that is not
// translation-only (AGGTextRenderer.cpp:144-148); a width measurement never
// needs one (StringWidthConsumer).
GlyphRendering render_type_for(const Font& font, bool force_vector)
{
    const auto settings = render_settings(font);
    if (force_vector || font.rotation != 0.0f
        || font.shear != neutral_font_shear || font.false_bold_width != 0.0f
        || !font_antialiased(font) || font.size > 30 || !settings.hinting) {
        return GlyphRendering::outline;
    }
    return settings.subpixel ? GlyphRendering::subpix : GlyphRendering::gray8;
}

// FreeType load flags for a glyph, exactly as FontEngine::PrepareGlyph builds
// them (src/servers/app/font/FontEngine.cpp:486-490), and used for both the
// advances and the raster so the two can never disagree.
//
// Note what that does to B_DISABLE_ANTIALIASING: it forces glyph_ren_outline,
// which loads with FT_LOAD_TARGET_NORMAL -- not FreeType's monochrome hinter.
// The aliasing happens afterwards, in AGG's rasteriser with a 0.5 threshold
// gamma (AGGTextRenderer.cpp:96-107). FT_LOAD_TARGET_MONO instead selects the
// strong, bilevel-oriented hinting, which on a bytecode-hinted face like Arial
// snaps stems to whole pixels. Measured with Windows' Arial and FreeType 2.15 at
// 18px, "Hamburgefonstiv" with antialiasing disabled: 713 non-white pixels under
// TARGET_MONO, 584 under the server's TARGET_NORMAL; view-scaled 2x, 2793
// against 2378.
FT_Int32 load_flags_for(GlyphRendering rendering, bool hinting)
{
    FT_Int32 flags = hinting ? FT_LOAD_DEFAULT : FT_LOAD_NO_HINTING;
    flags |= rendering == GlyphRendering::subpix ? FT_LOAD_TARGET_LCD
                                                 : FT_LOAD_TARGET_NORMAL;
    return flags;
}

// FontEngine::Init sizes the face with `unsigned(size * 64.0) >> 6` for both
// axes (src/servers/app/font/FontEngine.cpp:688-690): the ppem is the size
// *truncated*, so a 12.7 font is laid out and drawn at 12, not 13.
FT_UInt pixel_size_for(float size)
{
    const double clamped = std::clamp(static_cast<double>(size), 0.0, 65535.0);
    return static_cast<FT_UInt>(static_cast<unsigned>(clamped * 64.0) >> 6);
}

// AGG's iround (headers/libs/agg/agg_basics.h:188-191), which is how the
// serialized scanline adaptors place a cached glyph bitmap
// (agg_scanline_storage_aa.h:684-685): half away from zero, not floor.
int agg_iround(double value)
{
    return static_cast<int>(value < 0.0 ? value - 0.5 : value + 0.5);
}

// Always called, never skipped: the FT_Face cache is shared between fonts, and
// FT_Set_Transform is face state, so an earlier rotated font's matrix would
// otherwise stay installed and rotate an unrotated string. Nothing installs one
// any more (see transform_glyph), but an FT_Face is shared state and a reset
// costs nothing.
void install_transform(FT_Face face, const FT_Matrix& matrix, bool transformed)
{
    if (transformed) {
        FT_Matrix copy = matrix;
        FT_Set_Transform(face, &copy, nullptr);
    } else {
        FT_Set_Transform(face, nullptr, nullptr);
    }
}

// FontEngine.cpp:44-56
double int26p6_to_dbl(FT_Pos p)
{
    return double(p) / 64.0;
}

int dbl_to_int26p6(double p)
{
    return agg::saturating_int(p * 64.0 + 0.5);
}

// vertex_integer<int32, 6>'s constructor (agg_path_storage_integer.h:42-45):
// each coordinate shifted left one bit, the command in the two low bits.
// Unsigned shifts, which wrap as the server's int32 shift does.
void add_integer_vertex(std::vector<std::int32_t>& path, int x, int y, unsigned flag)
{
    path.push_back(static_cast<std::int32_t>(
        ((static_cast<std::uint32_t>(x) << 1) & ~std::uint32_t {1}) | (flag & 1u)));
    path.push_back(static_cast<std::int32_t>(
        ((static_cast<std::uint32_t>(y) << 1) & ~std::uint32_t {1}) | (flag >> 1)));
}

constexpr unsigned integer_move_to = 0;
constexpr unsigned integer_line_to = 1;
constexpr unsigned integer_curve3 = 2;
constexpr unsigned integer_curve4 = 3;

// decompose_ft_outline<path_storage_integer<int32, 6>> with flip_y, verbatim
// (FontEngine.cpp:59-290) -- including the round trip of every coordinate
// through int26p6_to_dbl and dbl_to_int26p6, whose int() truncates toward zero
// and so moves every negative 26.6 coordinate 1/64 pixel towards zero.
bool decompose_outline(const FT_Outline& outline, std::vector<std::int32_t>& path)
{
    const bool flip_y = true;
    FT_Vector v_last;
    FT_Vector v_control;
    FT_Vector v_start;
    double x1;
    double y1;
    double x2;
    double y2;
    double x3;
    double y3;

    const auto move_to = [&](double x, double y) {
        add_integer_vertex(path, dbl_to_int26p6(x), dbl_to_int26p6(y), integer_move_to);
    };
    const auto line_to = [&](double x, double y) {
        add_integer_vertex(path, dbl_to_int26p6(x), dbl_to_int26p6(y), integer_line_to);
    };
    const auto curve3 = [&](double cx, double cy, double x, double y) {
        add_integer_vertex(path, dbl_to_int26p6(cx), dbl_to_int26p6(cy), integer_curve3);
        add_integer_vertex(path, dbl_to_int26p6(x), dbl_to_int26p6(y), integer_curve3);
    };
    const auto curve4 = [&](double c1x, double c1y, double c2x, double c2y, double x,
                            double y) {
        add_integer_vertex(path, dbl_to_int26p6(c1x), dbl_to_int26p6(c1y), integer_curve4);
        add_integer_vertex(path, dbl_to_int26p6(c2x), dbl_to_int26p6(c2y), integer_curve4);
        add_integer_vertex(path, dbl_to_int26p6(x), dbl_to_int26p6(y), integer_curve4);
    };

    int first = 0;
    for (int n = 0; n < outline.n_contours; n++) {
        const int last = outline.contours[n];
        const FT_Vector* limit = outline.points + last;

        v_start = outline.points[first];
        v_last = outline.points[last];
        v_control = v_start;

        const FT_Vector* point = outline.points + first;
        const auto* tags = outline.tags + first;
        char tag = FT_CURVE_TAG(tags[0]);

        // A contour cannot start with a cubic control point!
        if (tag == FT_CURVE_TAG_CUBIC)
            return false;

        // check first point to determine origin
        if (tag == FT_CURVE_TAG_CONIC) {
            // first point is conic control.  Yes, this happens.
            if (FT_CURVE_TAG(outline.tags[last]) == FT_CURVE_TAG_ON) {
                // start at last point if it is on the curve
                v_start = v_last;
                limit--;
            } else {
                // if both first and last points are conic, start at their
                // middle and record its position for closure
                v_start.x = (v_start.x + v_last.x) / 2;
                v_start.y = (v_start.y + v_last.y) / 2;
                v_last = v_start;
            }
            point--;
            tags--;
        }

        x1 = int26p6_to_dbl(v_start.x);
        y1 = int26p6_to_dbl(v_start.y);
        if (flip_y)
            y1 = -y1;
        move_to(x1, y1);

        bool closed = false;
        while (point < limit && !closed) {
            point++;
            tags++;

            tag = FT_CURVE_TAG(tags[0]);
            switch (tag) {
            case FT_CURVE_TAG_ON: { // emit a single line_to
                x1 = int26p6_to_dbl(point->x);
                y1 = int26p6_to_dbl(point->y);
                if (flip_y)
                    y1 = -y1;
                line_to(x1, y1);
                continue;
            }

            case FT_CURVE_TAG_CONIC: { // consume conic arcs
                v_control.x = point->x;
                v_control.y = point->y;

                for (;;) { // Do_Conic
                    if (point < limit) {
                        FT_Vector vec;
                        FT_Vector v_middle;

                        point++;
                        tags++;
                        tag = FT_CURVE_TAG(tags[0]);

                        vec.x = point->x;
                        vec.y = point->y;

                        if (tag == FT_CURVE_TAG_ON) {
                            x1 = int26p6_to_dbl(v_control.x);
                            y1 = int26p6_to_dbl(v_control.y);
                            x2 = int26p6_to_dbl(vec.x);
                            y2 = int26p6_to_dbl(vec.y);
                            if (flip_y) {
                                y1 = -y1;
                                y2 = -y2;
                            }
                            curve3(x1, y1, x2, y2);
                            break;
                        }

                        if (tag != FT_CURVE_TAG_CONIC)
                            return false;

                        v_middle.x = (v_control.x + vec.x) / 2;
                        v_middle.y = (v_control.y + vec.y) / 2;

                        x1 = int26p6_to_dbl(v_control.x);
                        y1 = int26p6_to_dbl(v_control.y);
                        x2 = int26p6_to_dbl(v_middle.x);
                        y2 = int26p6_to_dbl(v_middle.y);
                        if (flip_y) {
                            y1 = -y1;
                            y2 = -y2;
                        }
                        curve3(x1, y1, x2, y2);

                        v_control = vec;
                        continue; // goto Do_Conic
                    }

                    x1 = int26p6_to_dbl(v_control.x);
                    y1 = int26p6_to_dbl(v_control.y);
                    x2 = int26p6_to_dbl(v_start.x);
                    y2 = int26p6_to_dbl(v_start.y);
                    if (flip_y) {
                        y1 = -y1;
                        y2 = -y2;
                    }
                    curve3(x1, y1, x2, y2);
                    closed = true; // goto Close
                    break;
                }
                continue;
            }

            default: { // FT_CURVE_TAG_CUBIC
                FT_Vector vec1;
                FT_Vector vec2;

                if (point + 1 > limit || FT_CURVE_TAG(tags[1]) != FT_CURVE_TAG_CUBIC)
                    return false;

                vec1.x = point[0].x;
                vec1.y = point[0].y;
                vec2.x = point[1].x;
                vec2.y = point[1].y;

                point += 2;
                tags += 2;

                if (point <= limit) {
                    FT_Vector vec;

                    vec.x = point->x;
                    vec.y = point->y;

                    x1 = int26p6_to_dbl(vec1.x);
                    y1 = int26p6_to_dbl(vec1.y);
                    x2 = int26p6_to_dbl(vec2.x);
                    y2 = int26p6_to_dbl(vec2.y);
                    x3 = int26p6_to_dbl(vec.x);
                    y3 = int26p6_to_dbl(vec.y);
                    if (flip_y) {
                        y1 = -y1;
                        y2 = -y2;
                        y3 = -y3;
                    }
                    curve4(x1, y1, x2, y2, x3, y3);
                    continue;
                }

                x1 = int26p6_to_dbl(vec1.x);
                y1 = int26p6_to_dbl(vec1.y);
                x2 = int26p6_to_dbl(vec2.x);
                y2 = int26p6_to_dbl(vec2.y);
                x3 = int26p6_to_dbl(v_start.x);
                y3 = int26p6_to_dbl(v_start.y);
                if (flip_y) {
                    y1 = -y1;
                    y2 = -y2;
                    y3 = -y3;
                }
                curve4(x1, y1, x2, y2, x3, y3);
                closed = true; // goto Close
                continue;
            }
            }
        }

        // path.close_polygon() is a no-op on path_storage_integer
        // (agg_path_storage_integer.h:113), so the Close label and the end of
        // the loop body are the same place.
        first = last + 1;
    }
    return true;
}

// Whether a glyph's outline, transformed, stays within max_vector_extent
// pixels of the canvas origin in both axes (its control points bound the
// curve, so checking them is enough).
template<class Glyph, class Transform>
bool transformed_path_fits(const Glyph& glyph, double dx, double dy,
                           const Transform& transform)
{
    for (std::size_t i = 0; i + 1 < glyph.path.size(); i += 2) {
        double x = dx + double(glyph.path[i] >> 1) / 64.0;
        double y = dy + double(glyph.path[i + 1] >> 1) / 64.0;
        transform.transform(&x, &y);
        if (!(std::fabs(x) <= max_vector_extent) || !(std::fabs(y) <= max_vector_extent))
            return false;
    }
    return true;
}

// serialized_integer_path_adaptor<int32, 6>::vertex (agg_path_storage_integer.h:
// 272-298) over a glyph's stored vertices, at the glyph's pen position (dx,
// dy) and scale 1 (FontCacheEntry::InitAdaptors, FontCacheEntry.cpp:370-394):
// an end_poly before a move_to only once a contour has more than two
// vertices, one more at the end, and both with the coordinates (0, 0) the
// adaptor writes for them.
agg::VertexList integer_path_vertices(const std::vector<std::int32_t>& path, double dx,
                                      double dy)
{
    agg::VertexList out;
    unsigned vertices = 0;
    for (std::size_t i = 0; i + 1 < path.size(); i += 2) {
        const std::int32_t vx = path[i];
        const std::int32_t vy = path[i + 1];
        const double x = dx + (double(vx >> 1) / 64.0) * 1.0;
        const double y = dy + (double(vy >> 1) / 64.0) * 1.0;
        unsigned cmd = agg::path_cmd_stop;
        switch (((vy & 1) << 1) | (vx & 1)) {
        case integer_move_to: cmd = agg::path_cmd_move_to; break;
        case integer_line_to: cmd = agg::path_cmd_line_to; break;
        case integer_curve3: cmd = agg::path_cmd_curve3; break;
        case integer_curve4: cmd = agg::path_cmd_curve4; break;
        }
        if (agg::is_move_to(cmd) && vertices > 2) {
            out.push_back({0, 0, agg::path_cmd_end_poly | agg::path_flags_close});
            vertices = 0;
        }
        ++vertices;
        out.push_back({x, y, cmd});
    }
    out.push_back({0, 0, agg::path_cmd_end_poly | agg::path_flags_close});
    return out;
}

struct FaceKey {
    bool mono = false;
    bool bold = false;
    bool italic = false;
    bool condensed = false;

    friend bool operator<(const FaceKey& a, const FaceKey& b)
    {
        return std::tie(a.mono, a.bold, a.italic, a.condensed)
            < std::tie(b.mono, b.bold, b.italic, b.condensed);
    }
};

FaceKey face_key(const Font& font)
{
    return FaceKey {
        font.spacing == fixed_spacing,
        (font.face & bold_face) != 0,
        (font.face & italic_face) != 0,
        (font.face & condensed_face) != 0,
    };
}

// Which of a family's eight files a key wants.
constexpr std::size_t style_slot(const FaceKey& key)
{
    return (key.bold ? 1u : 0u) | (key.italic ? 2u : 0u)
        | (key.condensed ? 4u : 0u);
}

constexpr std::size_t style_slots = 8;

// One font family, as the eight files it may provide, indexed by style_slot():
// regular, bold, italic, bold italic, then the same four condensed. A null entry
// means the family ships no file for that combination -- Cantarell has no
// italic, Noto Sans Mono has no italic, DejaVu Sans Mono has no condensed -- and
// that family is skipped for that style rather than answering with its regular
// face.
//
// Haiku conflates Italic and Oblique into the one B_ITALIC_FACE
// (FontStyle::_TranslateStyleToFace, src/servers/app/font/FontStyle.cpp:250-252)
// and so does this table: a family spelling it either way fills the same slot.
//
// Whole families are listed, in preference order, so a styled face is the styled
// face *of the family the regular face came from* whenever that family has one.
// The previous list mixed them -- proportional regular resolved to Noto Sans
// while proportional bold resolved to Cantarell-Bold -- and measured here at
// 12px over "Hamburgefonstiv" that was 96.875px regular against 96.000px bold:
// bold text laid out *narrower* than the same string in regular. Same family,
// the answer is 105.875px.
struct FamilyFiles {
    bool mono;
    std::array<const char*, style_slots> files;
};

constexpr FamilyFiles font_families[] = {
    // Haiku ships Noto under /boot/system/data/fonts/ttfonts (NotoSans
    // proportional, NotoMono fixed; no condensed cuts). These lead so a Haiku
    // host finds a real face; the relaxation ladder covers the absent condensed
    // slots, and on non-Haiku hosts the paths simply don't open and are skipped.
    {false, {
        "/boot/system/data/fonts/ttfonts/NotoSans-Regular.ttf",
        "/boot/system/data/fonts/ttfonts/NotoSans-Bold.ttf",
        "/boot/system/data/fonts/ttfonts/NotoSans-Italic.ttf",
        "/boot/system/data/fonts/ttfonts/NotoSans-BoldItalic.ttf",
        nullptr, nullptr, nullptr, nullptr,
    }},
    // Haiku's monospace file is named differently across revisions:
    // NotoSansMono-* on fuller images (e.g. hrev59866, which also ships Bold),
    // NotoMono-Regular on minimal ones (e.g. hrev60200). List both; missing
    // paths are skipped, so whichever the running image has is used. Getting
    // this wrong leaves Terminal (the mono consumer) blank while proportional
    // text renders.
    {true, {
        "/boot/system/data/fonts/ttfonts/NotoSansMono-Regular.ttf",
        "/boot/system/data/fonts/ttfonts/NotoSansMono-Bold.ttf",
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    }},
    {true, {
        "/boot/system/data/fonts/ttfonts/NotoMono-Regular.ttf",
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    }},
    // Noto Sans: the only complete proportional family on a stock Linux host,
    // and already the source of the regular face, so it stays first.
    {false, {
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Italic.ttf",
        "/usr/share/fonts/google-noto/NotoSans-BoldItalic.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Condensed.ttf",
        "/usr/share/fonts/google-noto/NotoSans-CondensedBold.ttf",
        "/usr/share/fonts/google-noto/NotoSans-CondensedItalic.ttf",
        "/usr/share/fonts/google-noto/NotoSans-CondensedBoldItalic.ttf",
    }},
    {false, {
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Oblique.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-BoldOblique.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSansCondensed.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSansCondensed-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSansCondensed-Oblique.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/"
            "DejaVuSansCondensed-BoldOblique.ttf",
    }},
    // Cantarell ships Regular and Bold and nothing slanted, which is why it may
    // only ever answer those two slots.
    {false, {
        "/usr/share/fonts/cantarell/Cantarell-Regular.otf",
        "/usr/share/fonts/cantarell/Cantarell-Bold.otf",
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    }},
    // The Windows and macOS rows are the documented stock filenames and have not
    // been opened on those hosts from here. They fail safely if one is wrong:
    // a path that does not exist is skipped, the relaxation ladder takes over,
    // and the substitution is reported rather than passed off as the real face.
    {false, {
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/arialbd.ttf",
        "C:/Windows/Fonts/ariali.ttf",
        "C:/Windows/Fonts/arialbi.ttf",
        "C:/Windows/Fonts/arialn.ttf",
        "C:/Windows/Fonts/arialnb.ttf",
        "C:/Windows/Fonts/arialni.ttf",
        "C:/Windows/Fonts/arialnbi.ttf",
    }},
    // A .ttc collection: one path, four styles at four face *indices*, which
    // open_matching() below searches. Opening index 0 and stopping -- all this
    // code used to do -- gets Helvetica Regular for an italic request no matter
    // how the candidate list is written.
    {false, {
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Helvetica.ttc",
        nullptr, nullptr, nullptr, nullptr,
    }},
    {true, {
        "/usr/share/fonts/google-noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSansMono-Bold.ttf",
        nullptr, nullptr,
        "/usr/share/fonts/google-noto/NotoSansMono-Condensed.ttf",
        "/usr/share/fonts/google-noto/NotoSansMono-CondensedBold.ttf",
        nullptr, nullptr,
    }},
    {true, {
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono-Oblique.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono-BoldOblique.ttf",
        nullptr, nullptr, nullptr, nullptr,
    }},
    {true, {
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/consolab.ttf",
        "C:/Windows/Fonts/consolai.ttf",
        "C:/Windows/Fonts/consolaz.ttf",
        nullptr, nullptr, nullptr, nullptr,
    }},
    {true, {
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Menlo.ttc",
        nullptr, nullptr, nullptr, nullptr,
    }},
};

// The files that could carry exactly `key`, best first. The environment
// override leads, as it always has, but it is now style-matched like any other
// candidate -- pointing HAIKU_REMOTE_FONT at a regular file no longer makes it
// the answer to an italic query.
std::vector<std::string> font_candidates(const FaceKey& key)
{
    std::vector<std::string> result;
    if (const char* configured = std::getenv(
            key.mono ? "HAIKU_REMOTE_MONO_FONT" : "HAIKU_REMOTE_FONT")) {
        result.emplace_back(configured);
    }
    const std::size_t slot = style_slot(key);
    for (const auto& family : font_families) {
        if (family.mono != key.mono || family.files[slot] == nullptr)
            continue;
        result.emplace_back(family.files[slot]);
    }
    return result;
}

// Styles to try when the requested one has no file anywhere, closest first.
// Every rung is still matched exactly against the file's own style, so what we
// end up with is known rather than assumed, and FaceChoice::exact records that
// it was a substitution.
//
// The order drops the cheapest axis first, cheap meaning least damage to the
// *metrics* -- which is the whole point of the reply this feeds. Measured here
// at 12px over "Hamburgefonstiv" in Noto Sans, against 96.875px regular: italic
// is 93.875 (3.0px away), bold 105.875 (9.0px) and condensed 80.922 (16.0px).
std::vector<FaceKey> style_relaxations(const FaceKey& key)
{
    std::vector<FaceKey> result {key};
    if (key.italic) {
        FaceKey next = result.back();
        next.italic = false;
        result.push_back(next);
    }
    if (key.bold) {
        FaceKey next = result.back();
        next.bold = false;
        result.push_back(next);
    }
    if (key.condensed) {
        FaceKey next = result.back();
        next.condensed = false;
        result.push_back(next);
    }
    return result;
}

struct FaceStyle {
    bool bold = false;
    bool italic = false;
    bool condensed = false;
    // A light or heavy cut: what app_server calls B_LIGHT_FACE / B_HEAVY_FACE.
    bool weighted = false;
};

std::string lowered(std::string_view text);

bool contains_word(const char* haystack, std::string_view needle)
{
    if (haystack == nullptr)
        return false;
    std::string lowered = haystack;
    for (char& c : lowered)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lowered.find(needle) != std::string_view::npos;
}

// The style a file really carries. FreeType sets FT_STYLE_FLAG_ITALIC for an
// Oblique file as well as an Italic one, the same conflation Haiku makes turning
// a style name into face bits (FontStyle::_TranslateStyleToFace,
// src/servers/app/font/FontStyle.cpp:250-252). Condensed has no FreeType flag,
// so it comes off the name -- which is where Haiku reads it from too (same
// function, :256-257).
//
// Both names are searched, because families disagree about where the word goes:
// Noto Sans and DejaVu Sans put it in the *style* ("Noto Sans" / "Condensed",
// read off the files here), while Arial Narrow puts it in the *family* and calls
// its style "Regular". Looking only at the style name would reject the narrow
// file as un-condensed and then report a substitution that had actually been
// found.
FaceStyle style_of(FT_Face face)
{
    FaceStyle result;
    result.bold = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
    result.italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    result.condensed = contains_word(face->style_name, "condensed")
        || contains_word(face->style_name, "narrow")
        || contains_word(face->family_name, "condensed")
        || contains_word(face->family_name, "narrow");
    // app_server reads weight off the style name too: "light"/"thin" make a
    // style B_LIGHT_FACE and "heavy"/"black" make it B_HEAVY_FACE (same
    // function, :256-262). FreeType has no flag for either -- a Thin file reports
    // neither bold nor italic -- so without this every Thin, ExtraLight, Light
    // and Black file looked exactly like Regular.
    result.weighted = contains_word(face->style_name, "light")
        || contains_word(face->style_name, "thin")
        || contains_word(face->style_name, "heavy")
        || contains_word(face->style_name, "black");
    return result;
}

// Exact, the way app_server compares: GetStyleMatchingFace() accepts a style
// only when `style->Face() == face` over the style-selecting bits
// (src/servers/app/font/FontFamily.cpp:228-235), and ServerFont::SetFace returns
// B_ERROR rather than approximating when nothing in the family matches
// (src/servers/app/ServerFont.cpp:334-371). Anything looser is how a regular
// face came to answer an italic query.
//
// A FaceKey never asks for a light or heavy cut (those bits have no file axis
// here; report_unhonoured() names them), so a weighted file never matches. In
// app_server it cannot answer such a query either: its face carries the
// B_LIGHT_FACE/B_HEAVY_FACE bit and the comparison is exact. Accepting it is how
// regular text came out in NotoSans-Thin on a stock Haiku image, which ships
// the whole Noto Sans weight range in one directory.
bool style_matches(const FaceStyle& have, const FaceKey& want)
{
    return have.bold == want.bold && have.italic == want.italic
        && have.condensed == want.condensed && !have.weighted;
}

// Opens `path` and returns the face inside it carrying exactly `want`, or
// nullptr. `want == nullptr` accepts the first face whatever its style.
// `family`, when not empty, is a lowercased family name the face must also
// belong to: a collection can hold several families (Noto Sans CJK's .ttc holds
// JP, KR, SC, TC and HK), and trait selection draws every style from the one
// family it chose -- the way ServerFont::SetFace only ever searches the font's
// own family (src/servers/app/ServerFont.cpp:347-365).
//
// The loop over num_faces is what makes the macOS candidates mean anything: a
// .ttc collection holds Regular, Bold, Oblique and Bold Oblique in one file and
// FT_New_Face(..., 0, ...) always returns the first of them.
FT_Face open_matching(FT_Library library, const std::string& path,
                      const FaceKey* want, long& index_out,
                      std::string_view family = {})
{
    const auto accepts = [&](FT_Face face) {
        if (!family.empty()
            && (face->family_name == nullptr
                || lowered(face->family_name) != family)) {
            return false;
        }
        return want == nullptr || style_matches(style_of(face), *want);
    };
    long count = 1;
    for (long index = 0; index < count; ++index) {
        FT_Face face = nullptr;
        if (FT_New_Face(library, path.c_str(), index, &face) != 0) {
            if (index == 0)
                return nullptr;
            continue;
        }
        count = face->num_faces;
        if (accepts(face)) {
            index_out = index;
            return face;
        }
        FT_Done_Face(face);
    }
    return nullptr;
}

const char* style_name(const FaceKey& key)
{
    static const char* const names[style_slots] = {
        "Regular", "Bold", "Italic", "Bold Italic",
        "Condensed", "Condensed Bold", "Condensed Italic",
        "Condensed Bold Italic",
    };
    return names[style_slot(key)];
}
// The exact set of scalars Haiku's layout engine charges `delta.space` to:
// GlyphLayoutEngine::IsWhiteSpace(), src/servers/app/font/GlyphLayoutEngine.h
// lines 226-243. Everything else -- including a NUL or a malformed byte -- is
// charged `delta.nonspace`.
bool is_haiku_white_space(std::uint32_t code)
{
    switch (code) {
    case 0x0009: // tab
    case 0x000a: // line feed
    case 0x000b: // vertical tab
    case 0x000c: // form feed
    case 0x000d: // carriage return
    case 0x0020: // space
    case 0x00a0: // non-breaking space
    case 0x2028: // line separator
    case 0x2029: // paragraph separator
        return true;
    default:
        return false;
    }
}

// UTF8ToCharCode, headers/private/interface/utf8_functions.h:168-228, byte for
// byte: it is what decides how many glyphs app_server lays out, so it decides
// where every glyph after a malformed sequence goes. An orphan continuation
// byte is one U+FFFD; a sequence cut short by another lead byte is one U+FFFD
// that has consumed what it read; one cut short by the terminating NUL is a
// NUL, which ends the string. `bytes` must be NUL-terminated (a std::string's
// data() is).
std::uint32_t utf8_to_char_code(const char** bytes)
{
    constexpr std::uint32_t substitute = 0xfffd;
    const auto byte = [&](std::size_t at) {
        return static_cast<std::uint8_t>((*bytes)[at]);
    };

    if ((byte(0) & 0x80u) == 0) {
        const std::uint32_t result = byte(0);
        if (result != 0)
            ++*bytes;
        return result;
    }

    if ((byte(0) & 0xc0u) == 0x80u) {
        ++*bytes;
        return substitute;
    }

    std::uint8_t mask = 0x80;
    std::uint32_t result = byte(0);
    ++*bytes;

    while ((result & mask) != 0) {
        if (mask == 0x02)
            return substitute;
        result &= ~static_cast<std::uint32_t>(mask);
        mask = static_cast<std::uint8_t>(mask >> 1);
    }

    while ((byte(0) & 0xc0u) == 0x80u) {
        result <<= 6;
        result += byte(0) & 0x3fu;
        ++*bytes;

        mask = static_cast<std::uint8_t>(mask << 1);
        if (mask == 0x40)
            return result;
    }

    if (mask == 0x40)
        return result;

    if (byte(0) == 0)
        return 0;

    return substitute;
}

// render_as_space, src/servers/app/font/FontCacheEntry.cpp:200-224: a code the
// font has no glyph for that is drawn as the font's own space.
bool render_as_space(std::uint32_t code)
{
    return (code >= 0x0009 && code <= 0x000d) || code == 0x0085
        || code == 0x00a0 || code == 0x1680 || (code >= 0x2000 && code <= 0x200a)
        || (code >= 0x2028 && code <= 0x2029) || code == 0x202f
        || code == 0x205f || code == 0x3000;
}

// render_as_zero_width, src/servers/app/font/FontCacheEntry.cpp:227-274: a code
// the font has no glyph for that is laid out with no advance and no ink.
bool render_as_zero_width(std::uint32_t code)
{
    return code == 0x00ad || code == 0x034f || code == 0x061c
        || (code >= 0x115f && code <= 0x1160)
        || (code >= 0x17b4 && code <= 0x17b5)
        || (code >= 0x180b && code <= 0x180f)
        || (code >= 0x200b && code <= 0x200f)
        || (code >= 0x202a && code <= 0x202e)
        || (code >= 0x2060 && code <= 0x206f) || code == 0x3164
        || (code >= 0xfe00 && code <= 0xfe0f) || code == 0xfeff
        || code == 0xffa0 || (code >= 0xfff0 && code <= 0xfff8)
        || (code >= 0x1bca0 && code <= 0x1bca3)
        || (code >= 0x1d173 && code <= 0x1d17a)
        || (code >= 0xe0000 && code <= 0xe01ef)
        || (code >= 0xe01f0 && code <= 0xe0fff) || (code & 0xffffu) >= 0xfffe
        || (code >= 0xfdd0 && code <= 0xfdef && code != 0xfdd1);
}

// The families app_server takes a glyph from when the font itself has none,
// in its order (GlyphLayoutEngine::PopulateFallbacks,
// src/servers/app/font/GlyphLayoutEngine.h:418-426).
constexpr std::array<const char*, 7> fallback_families {
    "Noto Sans", "Noto Sans Thai", "Noto Sans CJK JP", "Noto Sans Cherokee",
    "Noto Sans Symbols", "Noto Sans Symbols 2", "Noto Emoji",
};

std::string lowered(std::string_view text)
{
    std::string result(text);
    for (char& c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

std::string hex32(std::uint32_t value)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "0x";
    for (int shift = 28; shift >= 0; shift -= 4)
        result += digits[(value >> shift) & 0xfu];
    return result;
}

// Files fetched while engines were already running (TextEngine::add_font_files),
// in the order they arrived. Every engine indexes the ones it has not seen
// before it next resolves a face; the count is the cheap "anything new?" test.
std::mutex added_files_mutex;
std::vector<std::string> added_files;
std::atomic<std::size_t> added_files_count {0};

// TextEngine::set_font_search_override: when set, the only directories an
// engine created afterwards searches.
std::mutex font_search_override_mutex;
std::optional<std::vector<std::string>> font_search_override;

std::optional<std::vector<std::string>> current_font_search_override()
{
    std::lock_guard<std::mutex> lock(font_search_override_mutex);
    return font_search_override;
}

// Where the client's own font fetch (font_fetch.hpp) puts a guest's font
// files, and so where this
// engine looks for the exact files a server names before anything on the host:
// each HAIKU_REMOTE_FONT_DIR entry (priority 0), then the per-user cache
// (priority 1, font_cache_directory()).
std::vector<std::pair<std::string, int>> fetched_font_dirs()
{
    std::vector<std::pair<std::string, int>> result;
    if (const char* configured = std::getenv("HAIKU_REMOTE_FONT_DIR")) {
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::string_view rest = configured;
        while (!rest.empty()) {
            const auto end = rest.find(separator);
            const auto item = rest.substr(0, end);
            if (!item.empty())
                result.emplace_back(std::string(item), 0);
            if (end == std::string_view::npos)
                break;
            rest.remove_prefix(end + 1);
        }
    }
    if (const auto cache = font_cache_directory(); !cache.empty())
        result.emplace_back(cache.string(), 1);
    return result;
}

} // namespace

struct TextEngine::Impl {
    FT_Library library = nullptr;
    // Every face this engine opened, freed once at destruction. The maps below
    // hold non-owning pointers, so a trait choice can stand in for a named one
    // (or serve as a fallback) without anyone freeing it twice.
    std::vector<FT_Face> owned_faces;

    // Keyed by the *requested* style, and populated even when nothing resolved:
    // a negative result is as worth remembering as a positive one, and it is
    // what makes the diagnostic below fire exactly once per style rather than
    // once per measured string.
    struct Resolved {
        FT_Face face = nullptr;
        FaceChoice choice;
    };
    std::map<FaceKey, Resolved> faces;
    // Faces resolved from an RP_CAP_FONT_IDENTITY block, keyed on everything
    // the block says about the file. Misses are cached too, holding the trait
    // choice that stood in, so the diagnostic is said once.
    using IdentityKey = std::tuple<std::string, std::string, std::uint32_t,
                                   std::uint32_t, std::uint32_t>;
    std::map<IdentityKey, Resolved> named_faces;
    // Fallback faces by (family, style); "" as the style means "any".
    std::map<std::pair<std::string, std::string>, FT_Face> fallback_faces;
    const Resolved unavailable;
    Log log;
    // Face bits already named in a diagnostic. Deliberately *not* folded into
    // the `faces` key: B_LIGHT_FACE and B_REGULAR_FACE resolve to the same
    // FaceKey, so a cache hit would have swallowed the warning for the light
    // one -- which is the same class of silence this change is about.
    std::uint16_t reported_unhonoured = 0;

    // Every face found by scanning the font directories, as FreeType reports
    // it: the names app_server would register it under (FontManager::_AddFont
    // takes face->family_name and face->style_name verbatim), and the 'head'
    // fields RP_CAP_FONT_IDENTITY identifies a file by. Built once, lazily.
    struct IndexedFace {
        std::string path;
        long index = 0;       // FT face index, named instance in the high 16
        std::string family;   // verbatim
        std::string style;    // verbatim
        std::uint32_t checksum = 0;
        std::uint32_t revision = 0;
        long instances = 0;   // named instances (style_flags >> 16), face 0..
        bool instances_indexed = false;
        // 0 = HAIKU_REMOTE_FONT_DIR, 1 = the fetched-fonts cache, 2 = the host.
        int priority = 2;
        bool mono = false;
        // OS/2 usWeightClass (400 regular, 700 bold); 0 if absent.
        int weight = 0;
        // The style the face really carries (style_of()).
        FaceStyle face_style;
    };
    mutable std::vector<IndexedFace> index_;

    // The scanned faces grouped into families, for trait selection: classified
    // by what FreeType reports (family name + fixed-width flag) rather than by
    // filename, so a renamed file (NotoMono -> NotoSansMono across Haiku
    // revisions, #54) no longer breaks text. Sorted by (pitch, name), so
    // nothing below depends on the order the directory scan returned files in.
    struct TraitFamily {
        std::string name;     // lowercased FreeType family name
        bool mono = false;
        // Bit style_slot(k) set: the family has an unweighted face carrying
        // exactly style k, which is what select() can open for it.
        unsigned slots = 0;
        struct File {
            std::string path;
            int weight = 0;   // of its first face in this family
            int priority = 2;
        };
        std::vector<File> files;
    };
    mutable std::vector<TraitFamily> families_;
    // The one family each pitch draws from (choose_family()), once chosen;
    // index 0 proportional, 1 fixed. Null inside means "none at all".
    mutable std::array<std::optional<const TraitFamily*>, 2> chosen_;
    mutable bool discovery_done_ = false;
    // Discovery ran under TextEngine::set_font_search_override: only its
    // directories (and added files) were scanned, and selection consults
    // nothing else -- no environment override, no built-in table.
    mutable bool hermetic_ = false;
    // How many of added_files this engine has indexed.
    mutable std::size_t added_seen_ = 0;

    // Index the files fetched since this engine last looked (at the fetched
    // cache's priority), and forget every named resolution that did not find
    // the server's own file, so the next draw resolves it again -- now against
    // the file that was missing. Resolutions that did find it stay.
    // Index the files added since this engine last looked; true if any were.
    bool index_added_files() const
    {
        if (!discovery_done_ || library == nullptr
            || added_files_count.load() == added_seen_) {
            return false;
        }
        std::vector<std::string> fresh;
        {
            std::lock_guard<std::mutex> lock(added_files_mutex);
            fresh.assign(added_files.begin()
                             + static_cast<std::ptrdiff_t>(added_seen_),
                         added_files.end());
            added_seen_ = added_files.size();
        }
        for (const std::string& path : fresh) {
            if (std::any_of(index_.begin(), index_.end(),
                            [&](const IndexedFace& e) { return e.path == path; }))
                continue;
            FT_Face face = nullptr;
            if (FT_New_Face(library, path.c_str(), 0, &face) != 0)
                continue;
            const long count = face->num_faces;
            record(face, path, 0, 1);
            FT_Done_Face(face);
            for (long index = 1; index < count; ++index) {
                if (FT_New_Face(library, path.c_str(), index, &face) != 0)
                    continue;
                record(face, path, index, 1);
                FT_Done_Face(face);
            }
        }
        return !fresh.empty();
    }

    void sync_added_files()
    {
        if (!index_added_files())
            return;
        for (auto it = named_faces.begin(); it != named_faces.end();) {
            if (it->second.choice.identical)
                ++it;
            else
                it = named_faces.erase(it);
        }
    }

    // Whether a file carrying `id`'s own bytes -- 'head' checksum and revision
    // at its face index -- is on this host. The question the font fetch asks.
    bool has_identical(const FontIdentity& id)
    {
        if (library == nullptr || id.checksum_adjustment == 0)
            return false;
        ensure_discovery();
        sync_added_files();
        index_instances_of_all_with(id.face_index);
        return std::any_of(index_.begin(), index_.end(), [&](const IndexedFace& e) {
            return e.checksum == id.checksum_adjustment
                && e.revision == id.font_revision
                && static_cast<std::uint32_t>(e.index) == id.face_index;
        });
    }

    // Hinted and unhinted advances, per (face, ppem, load flags, glyph). The
    // layout loads every glyph twice the way FontEngine::PrepareGlyph does, and
    // UI text repeats; faces live as long as this engine, so the key is stable.
    struct Advance {
        bool loaded = false;
        double x = 0;
        double y = 0;
        double precise_x = 0;
        double precise_y = 0;
    };
    std::map<std::tuple<FT_Face, FT_UInt, FT_Int32, FT_UInt>, Advance> advances;

    Impl()
    {
        // No FT_Library_SetLcdFilter: FontEngine never calls it (src/servers/
        // app/font/FontEngine.cpp:452-455 is the whole of its library setup),
        // so the server renders LCD glyphs with whatever its FreeType does by
        // default. On the image's 2.6.3, built without
        // FT_CONFIG_OPTION_SUBPIXEL_RENDERING, that is the greyscale coverage
        // repeated into all three channels (third_party/freetype/README.md).
        if (FT_Init_FreeType(&library) != 0)
            library = nullptr;
    }

    ~Impl()
    {
        for (FT_Face face : owned_faces)
            FT_Done_Face(face);
        if (library != nullptr)
            FT_Done_FreeType(library);
    }

    FT_Face adopt(FT_Face face, const std::string& path, long index)
    {
        if (face != nullptr) {
            face_files[face] = {path, index};
            // FontEngine::Init selects the Unicode charmap
            // (src/servers/app/font/FontEngine.cpp:692-697).
            (void)FT_Select_Charmap(face, FT_ENCODING_UNICODE);
            owned_faces.push_back(face);
        }
        return face;
    }

    static void read_head(FT_Face face, std::uint32_t& checksum,
                          std::uint32_t& revision)
    {
        checksum = 0;
        revision = 0;
        if (const auto* head = static_cast<const TT_Header*>(
                FT_Get_Sfnt_Table(face, FT_SFNT_HEAD))) {
            checksum = static_cast<std::uint32_t>(head->CheckSum_Adjust);
            revision = static_cast<std::uint32_t>(head->Font_Revision);
        }
    }

    void record(FT_Face face, const std::string& path, long index,
                int priority) const
    {
        if (face->family_name == nullptr)
            return;
        IndexedFace entry;
        entry.path = path;
        entry.index = index;
        entry.family = face->family_name;
        entry.style = face->style_name != nullptr ? face->style_name : "";
        read_head(face, entry.checksum, entry.revision);
        entry.instances = (index >> 16) == 0 ? (face->style_flags >> 16) : 0;
        entry.instances_indexed = entry.instances == 0;
        entry.priority = priority;
        const std::string family = lowered(entry.family);
        // FIXED_WIDTH is the real signal; the name is a backstop for mono
        // fonts that forget to set it.
        entry.mono = (face->face_flags & FT_FACE_FLAG_FIXED_WIDTH) != 0
            || family.find("mono") != std::string::npos;
        if (const auto* os2 = static_cast<const TT_OS2*>(
                FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
            os2 != nullptr && os2->version != 0xffff) {
            entry.weight = os2->usWeightClass;
        }
        entry.face_style = style_of(face);
        index_.push_back(std::move(entry));
    }

    // Scan the font directories once. The fetched-fonts directories lead, so a
    // guest's own files are found -- for a name *and* for a trait -- before a
    // host's same-named but different-version ones.
    void ensure_discovery() const
    {
        if (discovery_done_)
            return;
        discovery_done_ = true;
        // Files fetched before this engine existed are indexed by the first
        // sync_added_files() after this scan (it skips any the scan found).
        added_seen_ = 0;
        if (library == nullptr)
            return;
        std::vector<std::pair<std::string, int>> dirs;
        if (const auto only = current_font_search_override()) {
            hermetic_ = true;
            for (const auto& dir : *only)
                dirs.emplace_back(dir, 2);
        } else {
            dirs = host_font_dirs();
        }
        scan(dirs);
    }

    // Every directory the normal search covers, best first.
    static std::vector<std::pair<std::string, int>> host_font_dirs()
    {
        std::vector<std::pair<std::string, int>> dirs = fetched_font_dirs();
        for (const char* dir : {
                 "/boot/system/data/fonts",
                 "/boot/system/non-packaged/data/fonts",
                 "/usr/share/fonts", "/usr/local/share/fonts",
                 "/System/Library/Fonts", "/Library/Fonts", "C:/Windows/Fonts",
             }) {
            dirs.emplace_back(dir, 2);
        }
        if (const char* home = std::getenv("HOME")) {
            const std::string h = home;
            for (const char* sub : {"/config/non-packaged/data/fonts",
                                    "/config/settings/fonts", "/.fonts",
                                    "/.local/share/fonts", "/Library/Fonts"}) {
                dirs.emplace_back(h + sub, 2);
            }
        }
        return dirs;
    }

    void scan(const std::vector<std::pair<std::string, int>>& dirs) const
    {
        std::error_code ec;
        for (const auto& [dir, priority] : dirs) {
            if (!std::filesystem::is_directory(dir, ec))
                continue;
            for (std::filesystem::recursive_directory_iterator
                     it(dir, std::filesystem::directory_options::skip_permission_denied, ec),
                     end;
                 it != end; it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (!it->is_regular_file(ec))
                    continue;
                const std::string ext = lowered(it->path().extension().string());
                // Haiku also registers Type 1 fonts (its psfonts directory);
                // FreeType opens them the same way.
                if (ext != ".ttf" && ext != ".ttc" && ext != ".otf"
                    && ext != ".otc" && ext != ".pfb" && ext != ".pfa") {
                    continue;
                }
                const std::string path = it->path().string();
                FT_Face face = nullptr;
                if (FT_New_Face(library, path.c_str(), 0, &face) != 0)
                    continue;
                const long count = face->num_faces;
                record(face, path, 0, priority);
                FT_Done_Face(face);
                for (long index = 1; index < count; ++index) {
                    if (FT_New_Face(library, path.c_str(), index, &face) != 0)
                        continue;
                    record(face, path, index, priority);
                    FT_Done_Face(face);
                }
            }
        }
        (void)index_added_files();
        std::map<std::pair<bool, std::string>, TraitFamily> grouped;
        for (const auto& entry : index_) {
            if ((entry.index >> 16) != 0)
                continue; // a named instance; select() opens faces, not those
            const std::string name = lowered(entry.family);
            TraitFamily& family = grouped[{entry.mono, name}];
            family.name = name;
            family.mono = entry.mono;
            if (!entry.face_style.weighted) {
                family.slots |= 1u << style_slot(FaceKey {
                    entry.mono, entry.face_style.bold, entry.face_style.italic,
                    entry.face_style.condensed});
            }
            if (std::none_of(family.files.begin(), family.files.end(),
                             [&](const TraitFamily::File& f) {
                                 return f.path == entry.path;
                             })) {
                family.files.push_back({entry.path, entry.weight, entry.priority});
            }
        }
        for (auto& [key, family] : grouped)
            families_.push_back(std::move(family));
    }

    // Named instances of a variable font are only opened when their family is
    // asked for: there can be dozens per file, each a full FT_New_Face.
    void index_instances_of(std::string_view family) const
    {
        const std::string wanted = lowered(family);
        const std::size_t count = index_.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (index_[i].instances_indexed || lowered(index_[i].family) != wanted)
                continue;
            index_[i].instances_indexed = true;
            const std::string path = index_[i].path;
            const long base = index_[i].index;
            const long instances = index_[i].instances;
            const int priority = index_[i].priority;
            for (long instance = 1; instance <= instances; ++instance) {
                FT_Face face = nullptr;
                const long index = (instance << 16) | base;
                if (FT_New_Face(library, path.c_str(), index, &face) != 0)
                    continue;
                record(face, path, index, priority);
                FT_Done_Face(face);
            }
        }
    }

    // The file a server names. Its bytes, if they are here: a matching 'head'
    // checksum and revision at the same face index is the server's own file,
    // whatever this host's FreeType calls it. Otherwise its name, exactly and
    // then ignoring case -- the same face, perhaps another release of it.
    // Lowest priority number wins, then scan order.
    const IndexedFace* find_named(std::string_view family, std::string_view style,
                                  const FontIdentity* identity) const
    {
        ensure_discovery();
        index_instances_of(family);
        if (identity != nullptr && identity->checksum_adjustment != 0)
            index_instances_of_all_with(identity->face_index);

        const IndexedFace* best = nullptr;
        const auto consider = [&](auto&& matches) {
            for (const auto& entry : index_) {
                if (!matches(entry))
                    continue;
                if (best == nullptr || entry.priority < best->priority)
                    best = &entry;
            }
            return best != nullptr;
        };
        if (identity != nullptr && identity->checksum_adjustment != 0
            && consider([&](const IndexedFace& e) {
                   return e.checksum == identity->checksum_adjustment
                       && e.revision == identity->font_revision
                       && static_cast<std::uint32_t>(e.index)
                           == identity->face_index;
               })) {
            return best;
        }
        if (consider([&](const IndexedFace& e) {
                return e.family == family && (style.empty() || e.style == style);
            })) {
            return best;
        }
        const std::string family_lower = lowered(family);
        const std::string style_lower = lowered(style);
        consider([&](const IndexedFace& e) {
            return lowered(e.family) == family_lower
                && (style.empty() || lowered(e.style) == style_lower);
        });
        return best;
    }

    // A server face index naming a named instance can only be matched by
    // checksum once that instance has been opened.
    void index_instances_of_all_with(std::uint32_t face_index) const
    {
        if ((face_index >> 16) == 0)
            return;
        std::vector<std::string> families;
        for (const auto& entry : index_) {
            if (!entry.instances_indexed
                && static_cast<std::uint32_t>(entry.index)
                    == (face_index & 0xffffu)) {
                families.push_back(entry.family);
            }
        }
        for (const auto& family : families)
            index_instances_of(family);
    }

    FT_Face open_indexed(const IndexedFace& entry)
    {
        FT_Face face = nullptr;
        if (FT_New_Face(library, entry.path.c_str(), entry.index, &face) != 0)
            return nullptr;
        return adopt(face, entry.path, entry.index);
    }

    // The one family trait selection draws a pitch from, or null when this
    // host has none. Chosen once per pitch and independently of the style
    // asked for, so regular, bold and italic are always cuts of the same
    // family: app_server's SetFace only ever looks inside the font's own family
    // (src/servers/app/ServerFont.cpp:347-365) and returns B_ERROR rather than
    // leave it when the style is missing there, so neither may this client --
    // a style the family lacks relaxes inside it (style_relaxations), and is
    // reported, never synthesised: app_server emboldens or slants only on an
    // explicit false_bold_width or shear, never for a missing face.
    //
    // The order, every step a property of the files and none the order a
    // directory scan returned them in (which is what used to decide, and on a
    // host without the preferred families made bold a different family from
    // regular):
    //   1. the preferred families, exactly named, first present wins;
    //   2. families whose name contains a preferred one, the first preference
    //      with any match, best of them by coverage;
    //   3. any family of the pitch, best by coverage.
    // Coverage: has a plain regular face, then how many of regular, bold,
    // italic and bold italic it has, then how many of all eight styles, then
    // the name. Symbol and emoji families never answer a trait outside step 1.
    const TraitFamily* choose_family(bool mono) const
    {
        ensure_discovery();
        auto& chosen = chosen_[mono ? 1 : 0];
        if (chosen)
            return *chosen;
        static constexpr std::array<std::string_view, 6> mono_prefs {
            "noto sans mono", "noto mono", "dejavu sans mono", "liberation mono",
            "consolas", "menlo"};
        static constexpr std::array<std::string_view, 6> prop_prefs {
            "noto sans", "dejavu sans", "cantarell", "liberation sans",
            "helvetica", "arial"};
        const auto& prefs = mono ? mono_prefs : prop_prefs;
        const auto coverage = [](const TraitFamily& f) {
            return std::make_tuple((f.slots & 1u) != 0,
                                   std::popcount(f.slots & 0x0fu),
                                   std::popcount(f.slots));
        };
        // Strictly better: more coverage, or the same and an earlier name.
        const auto better = [&](const TraitFamily& a, const TraitFamily& b) {
            const auto ca = coverage(a);
            const auto cb = coverage(b);
            if (ca != cb)
                return ca > cb;
            return a.name < b.name;
        };
        const auto pick = [&](auto&& admits) -> const TraitFamily* {
            const TraitFamily* best = nullptr;
            for (const auto& f : families_) {
                if (f.mono != mono || f.files.empty() || !admits(f))
                    continue;
                if (best == nullptr || better(f, *best))
                    best = &f;
            }
            return best;
        };
        const auto decorative = [](const TraitFamily& f) {
            return f.name.find("emoji") != std::string::npos
                || f.name.find("symbol") != std::string::npos;
        };
        const TraitFamily* found = nullptr;
        for (const auto& fam : prefs) {
            found = pick([&](const TraitFamily& f) { return f.name == fam; });
            if (found != nullptr)
                break;
        }
        for (const auto& fam : prefs) {
            if (found != nullptr)
                break;
            found = pick([&](const TraitFamily& f) {
                return !decorative(f) && f.name.find(fam) != std::string::npos;
            });
        }
        if (found == nullptr)
            found = pick([&](const TraitFamily& f) { return !decorative(f); });
        chosen = found;
        return found;
    }

    // A file selection may open, and the family the face inside it must belong
    // to (lowercased; empty: any -- the environment override and the built-in
    // table, which name files rather than families).
    struct Candidate {
        std::string path;
        std::string family;
    };

    // The chosen family's files for `key`'s pitch; open_matching() then picks
    // the exact style within. Nearest weight first: a family is often a whole
    // weight range -- Medium, SemiBold and ExtraBold carry no light/heavy word
    // and, for FreeType, may carry no bold flag either -- and a file without an
    // OS/2 table sorts as regular. Ties go to the fetched-fonts directories
    // (which lead, so a guest's own file wins), then to the path: never to the
    // scan order.
    std::vector<Candidate> discovered_candidates(const FaceKey& key) const
    {
        const TraitFamily* family = choose_family(key.mono);
        if (family == nullptr)
            return {};
        const int target = key.bold ? 700 : 400;
        std::vector<const TraitFamily::File*> files;
        for (const auto& f : family->files)
            files.push_back(&f);
        const auto distance = [&](const TraitFamily::File* f) {
            return std::abs((f->weight != 0 ? f->weight : 400) - target);
        };
        std::sort(files.begin(), files.end(),
                  [&](const TraitFamily::File* a, const TraitFamily::File* b) {
                      return std::make_tuple(distance(a), a->priority,
                                             std::string_view(a->path))
                          < std::make_tuple(distance(b), b->priority,
                                            std::string_view(b->path));
                  });
        std::vector<Candidate> result;
        for (const auto* f : files)
            result.push_back({f->path, family->name});
        return result;
    }

    // Full candidate list: the environment override first (unchanged), then the
    // chosen family. The built-in table of host paths only when discovery found
    // no family of the pitch at all: its rows are other families, and walked
    // after the chosen one they answered a style that family lacks with another
    // family's cut.
    std::vector<Candidate> candidates(const FaceKey& key) const
    {
        ensure_discovery();
        if (hermetic_)
            return discovered_candidates(key);
        std::vector<std::string> base = font_candidates(key);  // [env?, table...]
        const bool has_env = std::getenv(
            key.mono ? "HAIKU_REMOTE_MONO_FONT" : "HAIKU_REMOTE_FONT") != nullptr;
        std::vector<Candidate> result;
        std::size_t i = 0;
        if (has_env && !base.empty()) {
            result.push_back({base[0], {}});
            i = 1;
        }
        auto family = discovered_candidates(key);
        if (!family.empty()) {
            for (auto& c : family)
                result.push_back(std::move(c));
            return result;
        }
        for (; i < base.size(); ++i)
            result.push_back({base[i], {}});
        return result;
    }

    // Walks the relaxation ladder, then -- only if every rung failed -- accepts
    // any file that opens at all, so an environment override pointing at an
    // unusual style still works as the last resort it has always been.
    Resolved select(const FaceKey& key)
    {
        Resolved result;
        for (const auto& rung : style_relaxations(key)) {
            for (const auto& [path, family] : candidates(rung)) {
                if (!std::filesystem::exists(path))
                    continue;
                long index = 0;
                FT_Face face = open_matching(library, path, &rung, index, family);
                if (face == nullptr)
                    continue;
                result.face = adopt(face, path, index);
                result.choice.path = path;
                result.choice.index = index;
                result.choice.bold = rung.bold;
                result.choice.italic = rung.italic;
                result.choice.condensed = rung.condensed;
                result.choice.exact = style_matches(
                    FaceStyle {rung.bold, rung.italic, rung.condensed}, key);
                return result;
            }
        }

        const FaceKey plain {key.mono, false, false, false};
        for (const auto& [path, family] : candidates(plain)) {
            if (!std::filesystem::exists(path))
                continue;
            long index = 0;
            FT_Face face = open_matching(library, path, nullptr, index, family);
            if (face == nullptr)
                continue;
            const FaceStyle have = style_of(face);
            result.face = adopt(face, path, index);
            result.choice.path = path;
            result.choice.index = index;
            result.choice.bold = have.bold;
            result.choice.italic = have.italic;
            result.choice.condensed = have.condensed;
            result.choice.exact = style_matches(have, key);
            return result;
        }
        return result;
    }

    // Loud, and once per distinct style because `faces` caches the miss. A
    // substituted face means text is drawn -- and widths measured -- in a style
    // the server did not ask for. app_server itself refuses rather than
    // approximates (ServerFont::SetFace returns B_ERROR,
    // src/servers/app/ServerFont.cpp:334-371), so there is nothing here to
    // match by guessing quietly.
    void report_style(const FaceKey& key, const FaceChoice& choice) const
    {
        if (!log)
            return;
        const char* pitch = key.mono ? "fixed-pitch" : "proportional";
        if (choice.path.empty()) {
            log(std::string("font: no usable font file for the ") + pitch + ' '
                + style_name(key)
                + " style; string widths will be estimated, not measured, and"
                  " will not match the server's layout");
        } else if (!choice.exact) {
            const FaceKey got {key.mono, choice.bold, choice.italic,
                               choice.condensed};
            log(std::string("font: no ") + pitch + ' ' + style_name(key)
                + " face on this host; measuring with " + style_name(got)
                + " instead (" + choice.path
                + ") -- widths replied for this font are another style's");
        }
    }

    // B_LIGHT_FACE and B_HEAVY_FACE pick a style in app_server and have no file
    // axis here, so they are dropped -- but said out loud once each, rather than
    // dropped quietly the way italic used to be. A font that arrived with its
    // identity is resolved by name, which selects the weight by itself.
    void report_unhonoured(const Font& font)
    {
        if (font.identity != nullptr)
            return;
        const auto unhonoured = static_cast<std::uint16_t>(
            font.face & (light_face | heavy_face) & ~reported_unhonoured);
        if (unhonoured == 0)
            return;
        reported_unhonoured |= unhonoured;
        if (!log)
            return;
        log(std::string("font: face bits ")
            + (unhonoured == light_face ? "B_LIGHT_FACE"
               : unhonoured == heavy_face ? "B_HEAVY_FACE"
                                          : "B_LIGHT_FACE|B_HEAVY_FACE")
            + " pick a style in app_server but have no file axis here; widths"
              " replied for this font are another weight's");
    }

    const Resolved& resolve_trait(const Font& font)
    {
        report_unhonoured(font);
        const FaceKey key = face_key(font);
        if (const auto found = faces.find(key); found != faces.end())
            return found->second;
        const Resolved resolved = select(key);
        report_style(key, resolved.choice);
        return faces.emplace(key, resolved).first->second;
    }

    // The face an RP_CAP_FONT_IDENTITY block names, or -- said once, out loud --
    // the trait choice standing in for it.
    const Resolved& resolve_named(const Font& font)
    {
        const FontIdentity& id = *font.identity;
        IdentityKey key {id.family, id.style, id.checksum_adjustment,
                         id.font_revision, id.face_index};
        if (const auto found = named_faces.find(key); found != named_faces.end())
            return found->second;

        Resolved resolved;
        if (const auto* entry = find_named(id.family, id.style, &id)) {
            const IndexedFace chosen = *entry;
            if (FT_Face face = open_indexed(chosen)) {
                const FaceStyle have = style_of(face);
                resolved.face = face;
                resolved.choice.path = chosen.path;
                resolved.choice.index = chosen.index;
                resolved.choice.bold = have.bold;
                resolved.choice.italic = have.italic;
                resolved.choice.condensed = have.condensed;
                resolved.choice.exact = true;
                resolved.choice.named = true;
                resolved.choice.identical = id.checksum_adjustment != 0
                    && chosen.checksum == id.checksum_adjustment
                    && chosen.revision == id.font_revision
                    && static_cast<std::uint32_t>(chosen.index) == id.face_index;
                if (log && !resolved.choice.identical && id.checksum_adjustment != 0) {
                    log("font: the server's '" + id.family + "' '" + id.style
                        + "' (" + (id.path.empty() ? "a memory font" : id.path)
                        + ", head " + hex32(id.checksum_adjustment) + " rev "
                        + hex32(id.font_revision) + ") is drawn from "
                        + chosen.path + ", the same face by name but not the same"
                          " file (head " + hex32(chosen.checksum) + " rev "
                        + hex32(chosen.revision) + "); its advances may differ"
                          " -- the client fetches the guest's file itself on an"
                          " SSH route; on a direct one, copy it into "
                        + font_cache_directory().string());
                }
            }
        }
        if (resolved.face == nullptr) {
            resolved = resolve_trait(font);
            resolved.choice.named = false;
            resolved.choice.identical = false;
            if (log) {
                log("font: the server lays this text out in '" + id.family
                    + "' '" + id.style + "' ("
                    + (id.path.empty() ? "a memory font" : id.path)
                    + "), which is not installed here; drawing it with "
                    + (resolved.choice.path.empty() ? std::string("no font at all")
                                                    : resolved.choice.path)
                    + " instead, so its glyphs will not land where the server"
                      " put them -- the client fetches the guest's file itself"
                      " on an SSH route; on a direct one, copy it into "
                    + font_cache_directory().string());
            }
        }
        return named_faces.emplace(std::move(key), resolved).first->second;
    }

    const Resolved& resolve(const Font& font)
    {
        if (library == nullptr)
            return unavailable;
        sync_added_files();
        if (font.identity != nullptr && !font.identity->family.empty())
            return resolve_named(font);
        return resolve_trait(font);
    }

    FT_Face face_for(const Font& font) { return resolve(font).face; }

    // The style app_server's fallback search starts from: the font's own style
    // name (GlyphLayoutEngine.h:434-437). Without an identity, the name the
    // trait describes.
    static std::string fallback_style(const Font& font)
    {
        if (font.identity != nullptr)
            return font.identity->style;
        const bool bold = (font.face & bold_face) != 0;
        const bool italic = (font.face & italic_face) != 0;
        return bold ? (italic ? "Bold Italic" : "Bold")
                    : (italic ? "Italic" : "Regular");
    }

    FT_Face fallback_face(const std::string& family, const std::string& style)
    {
        const auto key = std::make_pair(family, style);
        if (const auto found = fallback_faces.find(key);
            found != fallback_faces.end()) {
            return found->second;
        }
        FT_Face face = nullptr;
        if (const auto* entry = find_named(family, style, nullptr)) {
            const IndexedFace chosen = *entry;
            face = open_indexed(chosen);
        }
        fallback_faces.emplace(key, face);
        return face;
    }

    // GlyphLayoutEngine::PopulateFallbacks's list, in its order: every family at
    // the font's style, then every family at "Regular", then every family's
    // first style (GlyphLayoutEngine.h:434-469).
    std::vector<FT_Face> fallbacks_for(const Font& font)
    {
        std::vector<FT_Face> result;
        const std::string styles[] = {fallback_style(font), "Regular", ""};
        for (const auto& style : styles) {
            for (const char* family : fallback_families) {
                if (FT_Face face = fallback_face(family, style))
                    result.push_back(face);
            }
        }
        return result;
    }

    // ---- FontCacheEntry and its FontEngine, for drawing ---------------------
    //
    // app_server rasterises a glyph once per font cache entry and keeps the
    // result: FontCacheEntry::CreateGlyph runs FontEngine::PrepareGlyph the
    // first time a character code is drawn in that entry and caches what it
    // made by the code (FontCacheEntry.cpp:325-368). Every entry has its own
    // FontEngine and every FontEngine its own FT_Library (FontEngine.cpp:
    // 452-455). That matters beyond speed: FreeType 2.6.3's PostScript hinter
    // keeps state in its module between glyphs, so a Type 1 glyph's hinted
    // outline depends on which glyphs that library hinted before it (Charter's
    // 'r' after 'a', '2' after a few others: tests/text_scenes.hpp). So the
    // client keeps the same structure -- one library per entry, glyphs made in
    // the order the session first draws them and never again -- and its
    // glyphs come out of the same hinter state as the server's whenever the
    // server's entry saw the same strings first.
    enum class GlyphData { gray8, subpix, outline };

    struct CachedGlyph {
        // False when PrepareGlyph failed: CreateGlyph then caches nothing and
        // returns NULL, so the server tries again next time.
        bool exists = false;
        GlyphData data = GlyphData::gray8;
        // FontEngine::Bounds(): the scanline storage's extent, or the
        // outline's bounding box rounded out (FontEngine.cpp:519-575).
        int x1 = 1;
        int y1 = 1;
        int x2 = 0;
        int y2 = 0;
        // The rendered FT_Bitmap, rows top to bottom.
        int left = 0;
        int top = 0;
        unsigned width = 0;
        unsigned rows = 0;
        unsigned pitch = 0;
        unsigned char pixel_mode = 0;
        std::vector<std::uint8_t> bitmap;
        // path_storage_integer<int32, 6>'s vertex_integer pairs as stored:
        // each coordinate shifted left by one, the command in the low bits.
        std::vector<std::int32_t> path;

        [[nodiscard]] bool bounds_valid() const { return x1 <= x2 && y1 <= y2; }
    };

    struct CacheEntry {
        FT_Library library = nullptr;
        FT_Face face = nullptr;
        GlyphRendering rendering = GlyphRendering::subpix;
        FT_Int32 load_flags = 0;
        std::map<std::uint32_t, CachedGlyph> glyphs;
        std::uint64_t use_count = 0;
        std::chrono::steady_clock::time_point last_used;

        CacheEntry() = default;
        CacheEntry(const CacheEntry&) = delete;
        CacheEntry& operator=(const CacheEntry&) = delete;
        ~CacheEntry()
        {
            if (face != nullptr)
                FT_Done_Face(face);
            if (library != nullptr)
                FT_Done_FreeType(library);
        }
    };

    // Which file each adopted face was opened from, so a cache entry can open
    // its own copy of it.
    std::map<FT_Face, std::pair<std::string, long>> face_files;
    std::map<std::string, std::unique_ptr<CacheEntry>> cache_entries;

    // FontCache::_ConstrainEntryCount (FontCache.cpp:105-155): at 30 entries
    // the one with the lowest 100 * uses / age goes.
    void constrain_cache_entries()
    {
        constexpr std::size_t max_entries = 30;
        if (cache_entries.size() < max_entries)
            return;
        const auto now = std::chrono::steady_clock::now();
        auto least = cache_entries.end();
        double least_index = 0;
        for (auto it = cache_entries.begin(); it != cache_entries.end(); ++it) {
            const double age = std::max(
                1.0, static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                         now - it->second->last_used).count()));
            const double usage = 100.0 * static_cast<double>(it->second->use_count) / age;
            if (least == cache_entries.end() || usage < least_index) {
                least = it;
                least_index = usage;
            }
        }
        if (least != cache_entries.end())
            cache_entries.erase(least);
    }

    // FontCache::FontCacheEntryFor (FontCache.cpp:48-97), keyed as
    // FontCacheEntry::GenerateSignature keys it (FontCacheEntry.cpp:396-405):
    // the face, charmap, ServerFont::Face(), rendering type, the size to one
    // decimal, hinting and the subpixel average weight -- the face standing
    // in for the family-and-style id. An entry made for one size serves every
    // size that prints the same, at the ppem of the font that made it
    // (FontEngine::Init, FontEngine.cpp:688-690).
    CacheEntry* cache_entry_for(FT_Face source, std::uint16_t face_bits, float size,
                                GlyphRendering rendering, bool hinting)
    {
        const auto file = face_files.find(source);
        if (file == face_files.end())
            return nullptr;
        char printed[64];
        std::snprintf(printed, sizeof printed, "%.1f", static_cast<double>(size));
        std::string key = file->second.first;
        key += '\n';
        key += std::to_string(file->second.second) + '|' + std::to_string(face_bits) + '|'
            + std::to_string(static_cast<int>(rendering)) + '|' + printed + '|'
            + (hinting ? "1" : "0") + "|120";
        if (const auto found = cache_entries.find(key); found != cache_entries.end())
            return found->second.get();

        constrain_cache_entries();
        auto entry = std::make_unique<CacheEntry>();
        if (FT_Init_FreeType(&entry->library) != 0) {
            entry->library = nullptr;
            return nullptr;
        }
        if (FT_New_Face(entry->library, file->second.first.c_str(), file->second.second,
                        &entry->face) != 0) {
            entry->face = nullptr;
            return nullptr;
        }
        // FontEngine::Init, FontEngine.cpp:688-697: the pixel size, then the
        // Unicode charmap.
        const FT_UInt ppem = pixel_size_for(size);
        FT_Set_Pixel_Sizes(entry->face, ppem, ppem);
        if (FT_Select_Charmap(entry->face, FT_ENCODING_UNICODE) != 0)
            (void)FT_Select_Charmap(entry->face, FT_ENCODING_NONE);
        entry->rendering = rendering;
        entry->load_flags = load_flags_for(rendering, hinting);
        CacheEntry* raw = entry.get();
        cache_entries.emplace(std::move(key), std::move(entry));
        return raw;
    }

    // FontEngine::PrepareGlyph, FontEngine.cpp:486-578, into a CachedGlyph.
    static CachedGlyph prepare_glyph(CacheEntry& entry, FT_UInt index)
    {
        CachedGlyph glyph;
        FT_Face face = entry.face;
        // Unscaled and unhinted first, for the B_CHAR_SPACING advance; its
        // result is overwritten by the second load, but the load itself is
        // part of what the hinter has seen.
        (void)FT_Load_Glyph(face, index,
                            entry.load_flags | FT_LOAD_NO_HINTING | FT_LOAD_NO_SCALE);
        if (FT_Load_Glyph(face, index, entry.load_flags) != 0)
            return glyph;
        FT_GlyphSlot slot = face->glyph;
        if (entry.rendering == GlyphRendering::outline) {
            if (!decompose_outline(slot->outline, glyph.path))
                return glyph;
            glyph.data = GlyphData::outline;
            outline_bounds(glyph);
            glyph.exists = true;
            return glyph;
        }
        const bool subpix = entry.rendering == GlyphRendering::subpix;
        if (FT_Render_Glyph(slot, subpix ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL) != 0)
            return glyph;
        glyph.data = subpix ? GlyphData::subpix : GlyphData::gray8;
        const FT_Bitmap& bitmap = slot->bitmap;
        glyph.left = slot->bitmap_left;
        glyph.top = slot->bitmap_top;
        glyph.width = bitmap.width;
        glyph.rows = bitmap.rows;
        glyph.pixel_mode = bitmap.pixel_mode;
        glyph.pitch = static_cast<unsigned>(std::abs(bitmap.pitch));
        glyph.bitmap.resize(static_cast<std::size_t>(glyph.pitch) * glyph.rows);
        for (unsigned row = 0; row < glyph.rows; ++row) {
            const auto source_row = bitmap.pitch >= 0 ? row : glyph.rows - 1 - row;
            if (glyph.pitch != 0)
                std::memcpy(&glyph.bitmap[static_cast<std::size_t>(row) * glyph.pitch],
                            bitmap.buffer + static_cast<std::size_t>(source_row) * glyph.pitch,
                            glyph.pitch);
        }
        bitmap_bounds(glyph);
        glyph.exists = true;
        return glyph;
    }

    // The cells decompose_ft_bitmap_gray8/_subpix add (FontEngine.cpp:
    // 327-428): one per pixel with any cover, rows top to bottom here. `cell`
    // gets the device-independent x/y (relative to the glyph origin, y down)
    // and the three covers (all three equal for a greyscale glyph).
    template<class Cell>
    static void for_each_cell(const CachedGlyph& glyph, Cell&& cell)
    {
        const bool mono = glyph.pixel_mode == FT_PIXEL_MODE_MONO;
        const bool triples = glyph.data == GlyphData::subpix && !mono;
        const unsigned columns = triples ? glyph.width / 3 : glyph.width;
        for (unsigned row = 0; row < glyph.rows; ++row) {
            const std::uint8_t* p = glyph.bitmap.data() + static_cast<std::size_t>(row) * glyph.pitch;
            const int y = -glyph.top + static_cast<int>(row);
            for (unsigned column = 0; column < columns; ++column) {
                const int x = glyph.left + static_cast<int>(column);
                if (mono) {
                    if ((p[column / 8] & (0x80u >> (column & 7))) != 0)
                        cell(x, y, 255, 255, 255);
                } else if (triples) {
                    const std::uint8_t* c = p + 3 * column;
                    if (c[0] != 0 || c[1] != 0 || c[2] != 0)
                        cell(x, y, c[0], c[1], c[2]);
                } else if (p[column] != 0) {
                    cell(x, y, p[column], p[column], p[column]);
                }
            }
        }
    }

    // scanline_storage_aa/_subpix min_x .. max_y over the cells stored
    // (agg_scanline_storage_aa.h:295-328): an empty glyph keeps the storage's
    // reset values, 0x7FFFFFFF and -0x7FFFFFFF, and so invalid bounds.
    static void bitmap_bounds(CachedGlyph& glyph)
    {
        glyph.x1 = glyph.y1 = 0x7FFFFFFF;
        glyph.x2 = glyph.y2 = -0x7FFFFFFF;
        for_each_cell(glyph, [&](int x, int y, std::uint8_t, std::uint8_t, std::uint8_t) {
            glyph.x1 = std::min(glyph.x1, x);
            glyph.x2 = std::max(glyph.x2, x);
            glyph.y1 = std::min(glyph.y1, y);
            glyph.y2 = std::max(glyph.y2, y);
        });
    }

    // path_storage_integer::bounding_rect (agg_path_storage_integer.h:195-219)
    // through floor/ceil (FontEngine.cpp:567-571). No vertex at all gives a
    // rect of zeros -- a valid one, so even a space widens the string's bounds
    // on the outline path.
    static void outline_bounds(CachedGlyph& glyph)
    {
        double bx1 = 1e100;
        double by1 = 1e100;
        double bx2 = -1e100;
        double by2 = -1e100;
        if (glyph.path.empty()) {
            bx1 = by1 = bx2 = by2 = 0.0;
        }
        for (std::size_t i = 0; i + 1 < glyph.path.size(); i += 2) {
            const double x = double(glyph.path[i] >> 1) / 64.0;
            const double y = double(glyph.path[i + 1] >> 1) / 64.0;
            bx1 = std::min(bx1, x);
            by1 = std::min(by1, y);
            bx2 = std::max(bx2, x);
            by2 = std::max(by2, y);
        }
        glyph.x1 = static_cast<int>(std::floor(bx1));
        glyph.y1 = static_cast<int>(std::floor(by1));
        glyph.x2 = static_cast<int>(std::ceil(bx2));
        glyph.y2 = static_cast<int>(std::ceil(by2));
    }

    // FontCacheEntry::CreateGlyph: the cached glyph for `code`, or the one
    // `engine`'s FontEngine makes for `index` the first time.
    static const CachedGlyph* cached_glyph(CacheEntry& owner, CacheEntry& engine,
                                           std::uint32_t code, FT_UInt index)
    {
        if (const auto found = owner.glyphs.find(code); found != owner.glyphs.end())
            return &found->second;
        CachedGlyph glyph = prepare_glyph(engine, index);
        if (!glyph.exists)
            return nullptr;
        return &owner.glyphs.emplace(code, std::move(glyph)).first->second;
    }

    // FontEngine::PrepareGlyph's two loads: unscaled and unhinted for the
    // B_CHAR_SPACING advance, then with the real flags for everything else
    // (src/servers/app/font/FontEngine.cpp:486-512).
    const Advance& advance_of(FT_Face face, FT_UInt ppem, FT_Int32 load_flags,
                              FT_UInt glyph)
    {
        auto& slot = advances[{face, ppem, load_flags, glyph}];
        if (slot.loaded)
            return slot;
        FT_Set_Pixel_Sizes(face, ppem, ppem);
        FT_UShort units = face->units_per_EM;
        if (!FT_IS_SCALABLE(face))
            units = 1;
        if (FT_Load_Glyph(face, glyph,
                          load_flags | FT_LOAD_NO_HINTING | FT_LOAD_NO_SCALE)
            == 0) {
            // GlyphCache keeps them as floats (FontCacheEntry.h:44-76).
            slot.precise_x = static_cast<float>(
                static_cast<double>(face->glyph->advance.x) / units);
            slot.precise_y = static_cast<float>(
                static_cast<double>(face->glyph->advance.y) / units);
        }
        if (FT_Load_Glyph(face, glyph, load_flags) == 0) {
            slot.loaded = true;
            slot.x = static_cast<double>(face->glyph->advance.x) / 64.0;
            slot.y = static_cast<double>(face->glyph->advance.y) / 64.0;
        } else {
            // PrepareGlyph failed: the layout gets no glyph at all.
            slot = Advance {};
        }
        return slot;
    }

    struct Laid {
        FT_Face face = nullptr;
        FT_UInt index = 0;
        // The character code: what FontCacheEntry caches the glyph under.
        std::uint32_t code = 0;
        // Pen position, relative to the string's origin, in app_server's
        // layout space (GlyphLayoutEngine's x/y: y grows downwards).
        double x = 0;
        double y = 0;
        // False for a glyph app_server lays out but never inks: a zero-width
        // code, or one whose glyph failed to load.
        bool ink = false;
    };

    struct Layout {
        std::vector<Laid> glyphs;
        FT_Face face = nullptr;
        // The final pen position: StringWidthConsumer::Finish's x, which is
        // what ServerFont::StringWidth returns -- untransformed, so rotation
        // and shear never change it.
        double advance_x = 0;
        double advance_y = 0;
        GlyphRendering rendering = GlyphRendering::subpix;
        FT_Int32 load_flags = 0;
        FT_UInt ppem = 0;
        // Embedded rotate/shear transform for the font.
        FT_Matrix matrix {};
        bool transformed = false;
        // How many bytes of the text were laid out, and whether a Boundary
        // callback ended the layout early (advance_x is then meaningless).
        std::size_t consumed = 0;
        bool stopped = false;
    };

    // Called before each glyph with its byte offset, the caret position there
    // (the width of everything before it: exactly what width() of that prefix
    // returns) and its pen position (the caret plus the kerning with the glyph
    // before it). Returning false ends the layout there.
    using Boundary = std::function<bool(std::size_t at, double caret, double pen)>;

    // What every layout costs (TextEngine::Stats).
    Stats stats;

    // GlyphLayoutEngine::LayoutGlyphs (src/servers/app/font/
    // GlyphLayoutEngine.h:256-372) without offsets: one glyph per UTF8ToCharCode
    // code, no shaping, no ligatures. B_STRING_SPACING kerns through
    // FT_Get_Kerning -- with the *character codes*, where FreeType expects glyph
    // indices (FontEngine::GetKerning, FontEngine.cpp:621-637); that is what
    // app_server does, so it is what this does. B_CHAR_SPACING advances by the
    // unhinted advance times the font's *unrounded* size; everything else by the
    // hinted advance.
    Layout layout(std::string_view text, const Font& font,
                  const EscapementDelta* delta, bool force_vector,
                  const Boundary* boundary = nullptr)
    {
        Layout result;
        result.face = face_for(font);
        if (result.face == nullptr)
            return result;
        ++stats.layouts;
        result.rendering = render_type_for(font, force_vector);
        result.load_flags =
            load_flags_for(result.rendering, render_settings(font).hinting);
        result.ppem = pixel_size_for(font.size);
        result.transformed = font_is_transformed(font);
        result.matrix = font_matrix(font);
        // Layout runs on an untransformed face, as FontEngine::Init leaves it;
        // the transform is face state shared between fonts, so reset it.
        install_transform(result.face, result.matrix, false);

        std::vector<FT_Face> fallbacks;
        bool fallbacks_populated = false;
        const std::string terminated(text);
        const char* cursor = terminated.c_str();
        const char* const start = cursor;
        const auto length = static_cast<std::ptrdiff_t>(text.size());
        const double size = font.size;

        double x = 0;
        double y = 0;
        double advance_x = 0;
        double advance_y = 0;
        std::uint32_t last_code = 0;
        std::uint32_t code = 0;
        while (length > 0) {
            const auto at = static_cast<std::size_t>(cursor - start);
            if ((code = utf8_to_char_code(&cursor)) == 0) {
                cursor = start + at; // the terminator is not laid out
                break;
            }
            ++stats.code_points;
            // The caret in front of this glyph: the pen after the last one,
            // before this glyph's kerning moves it. Computed with the same one
            // addition the end of the loop makes, so it is bit for bit the
            // width() of the prefix.
            const double caret = x + advance_x;
            if (font.spacing == string_spacing && last_code != 0
                && FT_HAS_KERNING(result.face)) {
                FT_Vector kerning {};
                FT_Set_Pixel_Sizes(result.face, result.ppem, result.ppem);
                FT_Get_Kerning(result.face, last_code, code, FT_KERNING_DEFAULT,
                               &kerning);
                advance_x += static_cast<double>(kerning.x) / 64.0;
                advance_y += static_cast<double>(kerning.y) / 64.0;
            }
            x += advance_x;
            y += advance_y;
            if (boundary != nullptr && !(*boundary)(at, caret, x)) {
                result.stopped = true;
                result.consumed = at;
                return result;
            }

            // FontCacheEntry::CreateGlyph and GlyphLayoutEngine::_CreateGlyph:
            // the font's glyph, else the first fallback that has one, else a
            // zero-width nothing, the font's space, or its .notdef.
            Laid laid;
            laid.x = x;
            laid.y = y;
            laid.face = result.face;
            laid.code = code;
            laid.index = FT_Get_Char_Index(result.face, code);
            bool zero_width = false;
            if (laid.index == 0) {
                if (!fallbacks_populated) {
                    fallbacks = fallbacks_for(font);
                    fallbacks_populated = true;
                }
                for (FT_Face fallback : fallbacks) {
                    if (const FT_UInt index = FT_Get_Char_Index(fallback, code)) {
                        laid.face = fallback;
                        laid.index = index;
                        break;
                    }
                }
                if (laid.index == 0) {
                    if (render_as_zero_width(code))
                        zero_width = true;
                    else if (render_as_space(code))
                        laid.index = FT_Get_Char_Index(result.face, 0x20);
                }
            }

            const Advance* advance = nullptr;
            if (!zero_width) {
                advance = &advance_of(laid.face, result.ppem, result.load_flags,
                                      laid.index);
                if (!advance->loaded)
                    advance = nullptr;
            }
            if (!zero_width && advance == nullptr) {
                // GlyphLayoutEngine.h:334-337: no glyph, no advance.
                result.glyphs.push_back(laid);
                advance_x = 0;
                advance_y = 0;
            } else {
                if (zero_width) {
                    advance_x = 0;
                    advance_y = 0;
                } else if (font.spacing == char_spacing) {
                    advance_x = advance->precise_x * size;
                    advance_y = advance->precise_y * size;
                } else {
                    advance_x = advance->x;
                    advance_y = advance->y;
                }
                if (delta != nullptr) {
                    advance_x += is_haiku_white_space(code) ? delta->space
                                                            : delta->nonspace;
                }
                laid.ink = !zero_width;
                result.glyphs.push_back(laid);
            }

            last_code = code;
            if (cursor - start + 1 > length)
                break;
        }
        x += advance_x;
        y += advance_y;
        result.advance_x = x;
        result.advance_y = y;
        result.consumed = std::min(static_cast<std::size_t>(cursor - start),
                                   text.size());
        return result;
    }
};

TextEngine::TextEngine()
    : impl_(std::make_unique<Impl>())
{
}

TextEngine::TextEngine(Log log)
    : impl_(std::make_unique<Impl>())
{
    impl_->log = std::move(log);
}

TextEngine::~TextEngine() = default;

void TextEngine::set_log(Log log)
{
    impl_->log = std::move(log);
}

bool TextEngine::available() const
{
    return impl_->library != nullptr;
}

GlyphRasterizer TextEngine::rasterizer()
{
    static const GlyphRasterizer identity = [] {
        GlyphRasterizer result;
        FT_Library library = nullptr;
        if (FT_Init_FreeType(&library) != 0)
            return result;
        FT_Int major = 0;
        FT_Int minor = 0;
        FT_Int patch = 0;
        FT_Library_Version(library, &major, &minor, &patch);
        result.engine = glyph_rasterizer_freetype;
        result.version = (static_cast<std::uint32_t>(major) << 16)
            | (static_cast<std::uint32_t>(minor) << 8) | static_cast<std::uint32_t>(patch);
        // On a throwaway library, so no engine's filter is touched: only
        // whether the call is implemented is wanted.
        if (FT_Library_SetLcdFilter(library, FT_LCD_FILTER_NONE)
            != FT_Err_Unimplemented_Feature) {
            result.flags |= glyph_rasterizer_lcd_filtering;
        }
        FT_UInt interpreter = 0;
        if (FT_Property_Get(library, "truetype", "interpreter-version", &interpreter) == 0)
            result.tt_interpreter = interpreter;
        FT_Done_FreeType(library);
        return result;
    }();
    return identity;
}

std::string TextEngine::describe(const GlyphRasterizer& rasterizer)
{
    if (rasterizer.engine != glyph_rasterizer_freetype)
        return "rasteriser " + std::to_string(rasterizer.engine) + " (not FreeType)";
    std::string text = "FreeType " + std::to_string(rasterizer.version >> 16) + "."
        + std::to_string((rasterizer.version >> 8) & 0xffu) + "."
        + std::to_string(rasterizer.version & 0xffu);
    text += (rasterizer.flags & glyph_rasterizer_lcd_filtering) != 0
        ? ", LCD filtering" : ", no LCD filtering";
    if (rasterizer.tt_interpreter != 0)
        text += ", TrueType interpreter v" + std::to_string(rasterizer.tt_interpreter);
    return text;
}

void TextEngine::add_font_files(const std::vector<std::string>& paths)
{
    std::lock_guard<std::mutex> lock(added_files_mutex);
    for (const std::string& path : paths) {
        if (std::find(added_files.begin(), added_files.end(), path)
            == added_files.end())
            added_files.push_back(path);
    }
    added_files_count.store(added_files.size());
}

std::size_t TextEngine::font_files_generation()
{
    return added_files_count.load();
}

void TextEngine::set_font_search_override(
    std::optional<std::vector<std::string>> directories)
{
    std::lock_guard<std::mutex> lock(font_search_override_mutex);
    font_search_override = std::move(directories);
}

bool TextEngine::has_identical_face(const FontIdentity& identity)
{
    return impl_->has_identical(identity);
}

TextEngine::FaceChoice TextEngine::selected_face(const Font& font)
{
    return impl_->resolve(font).choice;
}

std::vector<std::string> TextEngine::candidate_files(const Font& font)
{
    std::vector<std::string> paths;
    for (auto& candidate : impl_->candidates(face_key(font)))
        paths.push_back(std::move(candidate.path));
    return paths;
}

float TextEngine::width(std::string_view text, const Font& font,
                        const EscapementDelta* delta)
{
    const auto laid = impl_->layout(text, font, delta, false);
    if (laid.face != nullptr)
        return static_cast<float>(laid.advance_x);
    // No face at all: an estimate, walked the way the server walks the string
    // so at least the glyph count and the delta are the server's.
    const std::string terminated(text);
    const char* cursor = terminated.c_str();
    const char* const start = cursor;
    const auto length = static_cast<std::ptrdiff_t>(text.size());
    float estimate = 0;
    std::uint32_t code = 0;
    ++impl_->stats.layouts;
    while (length > 0 && (code = utf8_to_char_code(&cursor)) != 0) {
        ++impl_->stats.code_points;
        estimate += font.size * 0.6f;
        if (delta != nullptr)
            estimate += is_haiku_white_space(code) ? delta->space : delta->nonspace;
        if (cursor - start + 1 > length)
            break;
    }
    return estimate;
}

namespace {

// width()'s no-face estimate, one code point at a time: `visit(at, caret)` before
// each code point, false to stop. Returns the bytes walked and the estimate.
template <typename Visit>
std::pair<std::size_t, float> walk_estimate(std::string_view text, const Font& font,
                                            Visit&& visit)
{
    const std::string terminated(text);
    const char* cursor = terminated.c_str();
    const char* const start = cursor;
    const auto length = static_cast<std::ptrdiff_t>(text.size());
    float estimate = 0;
    while (length > 0) {
        const auto at = static_cast<std::size_t>(cursor - start);
        if (utf8_to_char_code(&cursor) == 0 || !visit(at, estimate))
            return {at, estimate};
        estimate += font.size * 0.6f;
        if (cursor - start + 1 > length)
            break;
    }
    return {std::min(static_cast<std::size_t>(cursor - start), text.size()), estimate};
}

} // namespace

TextEngine::Stops TextEngine::stops(std::string_view text, const Font& font)
{
    Stops out;
    out.offsets.reserve(text.size() + 1);
    out.caret.reserve(text.size() + 1);
    out.pen.reserve(text.size());
    const Impl::Boundary record = [&out](std::size_t at, double caret, double pen) {
        out.offsets.push_back(at);
        out.caret.push_back(static_cast<float>(caret));
        out.pen.push_back(static_cast<float>(pen));
        return true;
    };
    const auto laid = impl_->layout(text, font, nullptr, false, &record);
    if (laid.face != nullptr) {
        out.offsets.push_back(laid.consumed);
        out.caret.push_back(static_cast<float>(laid.advance_x));
        return out;
    }
    const auto [walked, total] = walk_estimate(text, font, [&](std::size_t at, float x) {
        ++impl_->stats.code_points;
        out.offsets.push_back(at);
        out.caret.push_back(x);
        out.pen.push_back(x);
        return true;
    });
    ++impl_->stats.layouts;
    out.offsets.push_back(walked);
    out.caret.push_back(total);
    return out;
}

std::size_t TextEngine::fit(std::string_view text, const Font& font, float max_width)
{
    // The caret only moves right (no advance here is negative), so the first
    // stop past max_width ends the search: everything after it is wider still.
    std::size_t fits = 0;
    const Impl::Boundary until = [&](std::size_t at, double caret, double) {
        if (static_cast<float>(caret) > max_width)
            return false;
        fits = at;
        return true;
    };
    const auto laid = impl_->layout(text, font, nullptr, false, &until);
    if (laid.face != nullptr) {
        if (!laid.stopped && static_cast<float>(laid.advance_x) <= max_width)
            fits = laid.consumed;
        return fits;
    }
    ++impl_->stats.layouts;
    bool stopped = false;
    const auto [walked, total] = walk_estimate(text, font, [&](std::size_t at, float x) {
        ++impl_->stats.code_points;
        if (x > max_width) {
            stopped = true;
            return false;
        }
        fits = at;
        return true;
    });
    if (!stopped && total <= max_width)
        fits = walked;
    return fits;
}

const TextEngine::Stats& TextEngine::stats() const
{
    return impl_->stats;
}

float TextEngine::draw(std::string_view text, Point baseline,
                       const DrawState& state, Surface& surface,
                       const EscapementDelta* delta, bool disable_hinting)
{
    // A view transform that is not translation-only is the drawing consumer's
    // NeedsVector() (AGGTextRenderer.cpp:144-148): it sends the font down the
    // outline path, which changes the load flags and so the advances too.
    const bool view_linear = !view_is_linear_identity(state.transform);
    const auto laid = impl_->layout(text, state.font, delta, view_linear);
    if (laid.face == nullptr)
        return width(text, state.font, delta);

    const Font& font = state.font;
    const bool antialias = font_antialiased(font);
    const RenderSettings settings = render_settings(font);
    // StringRenderer::fSubpixelAntiAliased (AGGTextRenderer.cpp:141).
    const bool subpixel_aa = settings.subpixel && antialias;
    const bool hinting = settings.hinting && !disable_hinting;

    // AGGTextRenderer::SetFont's embedded transform (AGGTextRenderer.cpp:73-82)
    // and RenderString's string transform (:376-381): embedded, then the
    // baseline, then the view -- Painter::SetTransform's matrix, which brackets
    // the view's affine between translations by -/+ the drawing offset
    // (Painter.cpp:371-383).
    agg::TransAffine transform;
    {
        // Transformable::ShearBy and RotateBy (Transformable.cpp:288-323),
        // about B_ORIGIN.
        const double shear = (90.0 - font.shear) * std::numbers::pi / 180.0;
        if (shear != 0.0) {
            transform.multiply(agg::TransAffine::translation(-0.0f, -0.0f));
            transform.multiply({1.0, std::tan(0.0), std::tan(shear), 1.0, 0.0, 0.0});
            transform.multiply(agg::TransAffine::translation(0.0f, 0.0f));
        }
        const double rotation = -font.rotation * std::numbers::pi / 180.0;
        if (rotation != 0.0) {
            transform.multiply(agg::TransAffine::translation(-0.0f, -0.0f));
            transform.multiply(agg::TransAffine::rotation(rotation));
            transform.multiply(agg::TransAffine::translation(0.0f, 0.0f));
        }
    }
    // Transformable::TranslateBy (Transformable.cpp:277-284) skips a zero offset.
    if (baseline.x != 0.0f || baseline.y != 0.0f)
        transform.multiply(agg::TransAffine::translation(baseline.x, baseline.y));
    if (!state.transform.is_identity()) {
        agg::TransAffine view = agg::TransAffine::translation(-state.x_offset, -state.y_offset);
        view.multiply({state.transform.sx, state.transform.shy, state.transform.shx,
                       state.transform.sy, state.transform.tx, state.transform.ty});
        view.multiply(agg::TransAffine::translation(state.x_offset, state.y_offset));
        transform.multiply(view);
    }
    // fCurves.approximation_scale(transform.scale()) (:383), and the offset a
    // cached bitmap is placed at instead of going through the transform: a
    // BPoint, so floats (:390-392, Transformable.cpp:188-199).
    const double approximation_scale = transform.scale();
    // RP_SET_TRANSFORM's six floats are unvalidated wire data. A transform
    // whose scale is not finite, or beyond anything a surface can show,
    // would flatten every curve into its 2^20-point cap and hand the
    // rasteriser outlines millions of pixels wide; app_server would grind
    // through that, the client draws no vector glyphs for it instead.
    const bool vector_drawable = std::isfinite(approximation_scale)
        && approximation_scale <= max_vector_scale;
    double offset_dx = 0;
    double offset_dy = 0;
    transform.transform(&offset_dx, &offset_dy);
    const float offset_x = static_cast<float>(offset_dx);
    const float offset_y = static_cast<float>(offset_dy);

    const painter::Canvas canvas {surface.pixels().data(), surface.width(), surface.height()};
    // StringRenderer's fBounds, IntRect(INT32_MAX, INT32_MAX, INT32_MIN,
    // INT32_MIN) to start (AGGTextRenderer.cpp:133).
    std::int32_t bounds_left = std::numeric_limits<std::int32_t>::max();
    std::int32_t bounds_top = std::numeric_limits<std::int32_t>::max();
    std::int32_t bounds_right = std::numeric_limits<std::int32_t>::min();
    std::int32_t bounds_bottom = std::numeric_limits<std::int32_t>::min();
    agg::VertexList vector_path;
    bool vector = false;

    Impl::CacheEntry* entry = impl_->cache_entry_for(laid.face, font.face, font.size,
                                                     laid.rendering, hinting);
    for (const auto& glyph_at : laid.glyphs) {
        if (!glyph_at.ink || entry == nullptr)
            continue;
        // A glyph from a fallback face is made by the fallback's own entry --
        // a font of the fallback style at this size with every other
        // attribute at ServerFont's defaults (GlyphLayoutEngine.h:456-458) --
        // and cached in this one (FontCacheEntry::CreateGlyph).
        Impl::CacheEntry* engine = entry;
        if (glyph_at.face != laid.face) {
            Font fallback;
            fallback.size = font.size;
            fallback.identity = font.identity;
            engine = impl_->cache_entry_for(glyph_at.face, 0, font.size,
                                            render_type_for(fallback, view_linear),
                                            hinting);
            if (engine == nullptr)
                continue;
        }
        const Impl::CachedGlyph* glyph
            = Impl::cached_glyph(*entry, *engine, glyph_at.code, glyph_at.index);
        if (glyph == nullptr || !glyph->bounds_valid())
            continue;
        const double x = glyph_at.x;
        const double y = glyph_at.y;
        // ConsumeGlyph's glyphBounds (AGGTextRenderer.cpp:204-209).
        bounds_left = std::min(bounds_left, static_cast<std::int32_t>(agg::saturating_int(glyph->x1 + x)));
        bounds_top = std::min(bounds_top, static_cast<std::int32_t>(agg::saturating_int(glyph->y1 + y - 1)));
        bounds_right = std::max(bounds_right, static_cast<std::int32_t>(agg::saturating_int(glyph->x2 + x + 1)));
        bounds_bottom = std::max(bounds_bottom, static_cast<std::int32_t>(agg::saturating_int(glyph->y2 + y + 1)));

        if (glyph->data == Impl::GlyphData::outline) {
            if (!vector_drawable || !transformed_path_fits(*glyph, x, y, transform))
                continue;
            // The outline, at its untransformed pen position, through
            // conv_curve, the false-bold contour when there is one, and the
            // string transform, into the rasteriser with the rest of the
            // string (AGGTextRenderer.cpp:266-287).
            agg::VertexList path = agg::conv_curve(
                integer_path_vertices(glyph->path, x, y), approximation_scale);
            const double contour = static_cast<double>(font.false_bold_width) * 2.0;
            if (contour != 0.0)
                path = agg::conv_contour(path, contour);
            path = agg::conv_transform(path, transform);
            vector_path.insert(vector_path.end(), path.begin(), path.end());
            vector = true;
            continue;
        }

        // A cached bitmap, placed by the serialized scanline adaptor at the
        // rounded pen position plus the transform's offset
        // (agg_scanline_storage_aa.h:684-685, AGGTextRenderer.cpp:229-236).
        const int origin_x = agg_iround(x + offset_x);
        const int origin_y = agg_iround(y + offset_y);
        if (glyph->data == Impl::GlyphData::subpix) {
            Impl::for_each_cell(*glyph, [&](int cx, int cy, std::uint8_t r, std::uint8_t g,
                                            std::uint8_t b) {
                surface.paint_subpixel_coverage(origin_x + cx, origin_y + cy, state.high,
                                                state, r, g, b);
            });
        } else {
            Impl::for_each_cell(*glyph, [&](int cx, int cy, std::uint8_t cover, std::uint8_t,
                                            std::uint8_t) {
                surface.paint_text_coverage(origin_x + cx, origin_y + cy, state.high, state,
                                            cover);
            });
        }
    }

    // StringRenderer::Finish (AGGTextRenderer.cpp:150-186): the vector glyphs
    // in one pass, then the decorations.
    if (vector)
        painter::fill_text_path(canvas, state, vector_path, subpixel_aa, antialias);
    const auto horizontal_line = [&](float line_y) {
        // _DrawHorizontalLine, AGGTextRenderer.cpp:330-351.
        double lx = static_cast<float>(bounds_left);
        double ly = line_y;
        double rx = static_cast<float>(bounds_right);
        double ry = line_y;
        transform.transform(&lx, &ly);
        transform.transform(&rx, &ry);
        const float left_x = static_cast<float>(lx);
        const float left_y = static_cast<float>(ly);
        const float right_x = static_cast<float>(rx);
        const float right_y = static_cast<float>(ry);
        agg::VertexList line {
            {left_x + 0.5, left_y + 0.5, agg::path_cmd_move_to},
            {right_x + 0.5, right_y + 0.5, agg::path_cmd_line_to}};
        agg::StrokeStyle style;
        style.width = font.size / 12.0f;
        painter::fill_text_path(canvas, state, agg::conv_stroke(line, style), subpixel_aa,
                                antialias);
    };
    const double end_y = laid.advance_y;
    if ((font.face & underscore_face) != 0)
        horizontal_line(static_cast<float>(end_y + 2));
    if ((font.face & strikeout_face) != 0) {
        // ServerFont::GetHeight (ServerFont.cpp, FontStyle.cpp:50-57, 126-131).
        const FT_Face face = laid.face;
        if (FT_IS_SCALABLE(face) && face->units_per_EM != 0) {
            const float ascent = static_cast<float>(
                static_cast<double>(face->ascender) / face->units_per_EM) * font.size;
            const float descent = static_cast<float>(
                static_cast<double>(-face->descender) / face->units_per_EM) * font.size;
            horizontal_line(static_cast<float>(end_y - (ascent + descent) / 4));
        }
    }

    // The untransformed advance, matching ServerFont::StringWidth (see the note
    // on Layout::advance_x). The caller turns it into a pen position of
    // {where.x + advance, where.y}, which is still wrong for a rotated string
    // -- app_server's StringRenderer::Finish transforms that point. The server
    // no longer reads that reply (RemoteDrawingEngine::DrawString computes its
    // own pen), so it is left as it was.
    return static_cast<float>(laid.advance_x);
}

} // namespace haiku_remote
