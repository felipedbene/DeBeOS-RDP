#include <string_view>
#include "haiku_remote/text_engine.hpp"

#include "haiku_remote/font_fetch.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_LCD_FILTER_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <atomic>
#include <map>
#include <mutex>
#include <numbers>
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

// The *linear* part of a view transform, in the y-up space FT_Set_Transform
// works in.
//
// DrawState::transform holds Haiku's BAffineTransform exactly as Painter
// installs it -- translate(-offset) * affine * translate(offset)
// (src/servers/app/drawing/Painter/Painter.cpp:372-383) -- and that whole
// bracket is what DrawState::map_point reproduces. So the translation is the
// baseline's business and only sx/shx/shy/sy belong in the glyph outline.
//
// FreeType's y axis points up and the screen's points down, so converting a
// screen-space linear map into font space is a conjugation by diag(1, -1):
// the two cross terms change sign, the two diagonal terms do not. The rotation
// arm of `font_matrix` above is the same conversion done by hand -- app_server
// builds its embedded transform as RotateBy(-rotation) in screen space
// (AGGTextRenderer.cpp:81-82) and ServerFont::GetTransformedFace builds the
// matching FreeType matrix with +sin on yx (ServerFont.cpp:406-409).
FT_Matrix view_matrix(const Transform& transform)
{
    FT_Matrix result;
    result.xx = to_ft_fixed(transform.sx);
    result.xy = to_ft_fixed(-transform.shx);
    result.yx = to_ft_fixed(-transform.shy);
    result.yy = to_ft_fixed(transform.sy);
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

// A resource bound that arrives with the transform. Moving the transform into
// the outline means a wire-supplied scale now multiplies the glyph's *pixel*
// dimensions, and FT_Render_Glyph allocates width * rows bytes -- three times
// that in LCD mode. Nothing legible survives past this size on a surface capped
// at Surface::max_dimension, so an outline wider or taller than this is skipped
// rather than rendered.
constexpr FT_Pos max_glyph_extent = 4096;

bool glyph_is_renderable(FT_GlyphSlot slot)
{
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE)
        return true;
    FT_BBox box;
    FT_Outline_Get_CBox(&slot->outline, &box);
    // 26.6 fixed point.
    return (box.xMax - box.xMin) <= max_glyph_extent * 64
        && (box.yMax - box.yMin) <= max_glyph_extent * 64;
}

// Applies a 16.16 FT_Matrix to a point in FreeType's y-up space.
std::pair<float, float> transform_point(const FT_Matrix& matrix, float x,
                                        float y)
{
    const double dx = x;
    const double dy = y;
    return {
        static_cast<float>(
            (static_cast<double>(matrix.xx) * dx
             + static_cast<double>(matrix.xy) * dy) / ft_fixed_one),
        static_cast<float>(
            (static_cast<double>(matrix.yx) * dx
             + static_cast<double>(matrix.yy) * dy) / ft_fixed_one),
    };
}

// Applies RP_SET_FONT's `false_bold_width` to a loaded outline, between
// FT_Load_Glyph and FT_Render_Glyph -- app_server's equivalent is
// `fContour.width(font.FalseBoldWidth() * 2.0)` in AGGTextRenderer::SetFont,
// an outward contour of the glyph path. Stock software reaches this for glow
// and outline passes (Tracker's TextWidget, HaikuControlLook, MediaPlayer's
// SubtitleBitmap), not just font demos. Emboldening changes glyph weight only;
// it does not change the advance, so no width reply moves.
//
// Returns true when rendering should proceed, so it can sit in the existing
// load-and-render conjunction.
bool embolden_glyph(FT_GlyphSlot slot, FT_Pos strength)
{
    if (strength > 0 && slot->format == FT_GLYPH_FORMAT_OUTLINE)
        FT_Outline_EmboldenXY(&slot->outline, strength, strength);
    return true;
}

// Applies the combined embedded * view matrix to a loaded, already hinted glyph
// outline, between FT_Load_Glyph and FT_Render_Glyph. Returns true so it can sit
// in the load-and-render conjunction.
//
// This is deliberately *not* FT_Set_Transform. app_server transforms the hinted
// outline itself, through an AGG conv_transform with no condition on the matrix
// (FontCacheEntry::TransformedOutline, AGGTextRenderer.cpp:384-391), so every
// affine the wire can carry reaches the rasteriser. FT_Set_Transform does not
// promise that: FreeType 2.15 silently drops any matrix that fails
// FT_Matrix_Check (32 * |det| <= the sum of the squared terms), keeping the
// identity instead. Measured on MSYS2's FreeType 2.15.0 with Arial's 'H' at
// 18px: {2, 0, 0, 0} and {40, 0, 0, 1} both came back untransformed, so a
// singular view transform drew the whole string unscaled and a 40x stretch drew
// nothing stretched -- while FreeType 2.13.2 on Linux applied both. The outline
// transform is the same arithmetic FT_Load_Glyph would have applied after
// hinting, minus the version-dependent veto.
bool transform_glyph(FT_GlyphSlot slot, const FT_Matrix& matrix,
                     bool transformed)
{
    if (transformed && slot->format == FT_GLYPH_FORMAT_OUTLINE)
        FT_Outline_Transform(&slot->outline, &matrix);
    return true;
}

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
// nullptr. `want == nullptr` accepts face index 0 whatever its style.
//
// The loop over num_faces is what makes the macOS candidates mean anything: a
// .ttc collection holds Regular, Bold, Oblique and Bold Oblique in one file and
// FT_New_Face(..., 0, ...) always returns the first of them.
FT_Face open_matching(FT_Library library, const std::string& path,
                      const FaceKey* want, long& index_out)
{
    FT_Face face = nullptr;
    if (FT_New_Face(library, path.c_str(), 0, &face) != 0)
        return nullptr;
    if (want == nullptr || style_matches(style_of(face), *want)) {
        index_out = 0;
        return face;
    }
    const long count = face->num_faces;
    FT_Done_Face(face);
    for (long index = 1; index < count; ++index) {
        if (FT_New_Face(library, path.c_str(), index, &face) != 0)
            continue;
        if (style_matches(style_of(face), *want)) {
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
    };
    mutable std::vector<IndexedFace> index_;

    // The face-0 subset of the index, for trait selection: classified by what
    // FreeType reports (family name + fixed-width flag) rather than by filename,
    // so a renamed file (NotoMono -> NotoSansMono across Haiku revisions, #54)
    // no longer breaks text.
    struct Discovered {
        std::string path;
        bool mono = false;
        std::string family;   // lowercased FreeType family name
        int weight = 0;
    };
    mutable std::vector<Discovered> discovered_;
    mutable bool discovery_done_ = false;
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
        if (FT_Init_FreeType(&library) != 0) {
            library = nullptr;
        } else {
            // Match Haiku's default subpixel renderer filtering.
            (void)FT_Library_SetLcdFilter(library, FT_LCD_FILTER_DEFAULT);
        }
    }

    ~Impl()
    {
        for (FT_Face face : owned_faces)
            FT_Done_Face(face);
        if (library != nullptr)
            FT_Done_FreeType(library);
    }

    FT_Face adopt(FT_Face face)
    {
        if (face != nullptr) {
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
        for (const auto& entry : index_) {
            if (entry.index != 0)
                continue;
            discovered_.push_back(
                {entry.path, entry.mono, lowered(entry.family), entry.weight});
        }
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
        return adopt(face);
    }

    // Discovered files for `key`'s pitch, best family first. Returns ALL files
    // of the chosen family; open_matching() then picks the exact style within.
    std::vector<std::string> discovered_candidates(const FaceKey& key) const
    {
        ensure_discovery();
        // A vector (not an initializer_list via ternary -- that dangles) of
        // views onto static string literals, so the elements outlive the call.
        std::vector<std::string_view> prefs;
        if (key.mono)
            prefs = {"noto sans mono", "noto mono", "dejavu sans mono",
                     "liberation mono", "consolas", "menlo"};
        else
            prefs = {"noto sans", "dejavu sans", "cantarell",
                     "liberation sans", "helvetica", "arial"};
        // Nearest weight first. A family is often a whole weight range --
        // Medium, SemiBold and ExtraBold carry no light/heavy word and, for
        // FreeType, may carry no bold flag either -- so the first file the
        // directory scan happened to return used to win. Readdir order is not a
        // property anyone controls; the weight class is. A file without an OS/2
        // table sorts as regular, and the sort is stable so ties keep scan order.
        const int target = key.bold ? 700 : 400;
        std::vector<const Discovered*> hits;
        auto gather = [&](std::string_view fam, bool exact) {
            for (const auto& e : discovered_) {
                if (e.mono != key.mono)
                    continue;
                const bool hit = exact ? (e.family == fam)
                                       : (e.family.find(fam) != std::string::npos);
                if (hit)
                    hits.push_back(&e);
            }
        };
        auto by_weight = [&]() {
            std::stable_sort(hits.begin(), hits.end(),
                [&](const Discovered* a, const Discovered* b) {
                    const int wa = a->weight != 0 ? a->weight : 400;
                    const int wb = b->weight != 0 ? b->weight : 400;
                    return std::abs(wa - target) < std::abs(wb - target);
                });
            std::vector<std::string> paths;
            for (const auto* e : hits)
                paths.push_back(e->path);
            return paths;
        };
        for (const auto& fam : prefs) { gather(fam, true);  if (!hits.empty()) return by_weight(); }
        for (const auto& fam : prefs) { gather(fam, false); if (!hits.empty()) return by_weight(); }
        std::vector<std::string> result;
        // Last resort: any matching-pitch family that is not symbols/emoji.
        for (const auto& e : discovered_) {
            if (e.mono != key.mono)
                continue;
            if (e.family.find("emoji") != std::string::npos
                || e.family.find("symbol") != std::string::npos)
                continue;
            result.push_back(e.path);
        }
        return result;
    }

    // Full candidate list: env override first (unchanged behaviour), then
    // discovered families, then the hardcoded table as a final fallback.
    std::vector<std::string> candidates(const FaceKey& key) const
    {
        std::vector<std::string> base = font_candidates(key);  // [env?, table...]
        const bool has_env = std::getenv(
            key.mono ? "HAIKU_REMOTE_MONO_FONT" : "HAIKU_REMOTE_FONT") != nullptr;
        std::vector<std::string> result;
        std::size_t i = 0;
        if (has_env && !base.empty()) { result.push_back(base[0]); i = 1; }
        for (auto& path : discovered_candidates(key))
            result.push_back(std::move(path));
        for (; i < base.size(); ++i)
            result.push_back(base[i]);
        return result;
    }

    // Walks the relaxation ladder, then -- only if every rung failed -- accepts
    // any file that opens at all, so an environment override pointing at an
    // unusual style still works as the last resort it has always been.
    Resolved select(const FaceKey& key)
    {
        Resolved result;
        for (const auto& rung : style_relaxations(key)) {
            for (const auto& path : candidates(rung)) {
                if (!std::filesystem::exists(path))
                    continue;
                long index = 0;
                FT_Face face = open_matching(library, path, &rung, index);
                if (face == nullptr)
                    continue;
                result.face = adopt(face);
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
        for (const auto& path : candidates(plain)) {
            if (!std::filesystem::exists(path))
                continue;
            long index = 0;
            FT_Face face = open_matching(library, path, nullptr, index);
            if (face == nullptr)
                continue;
            const FaceStyle have = style_of(face);
            result.face = adopt(face);
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
            slot.precise_x = static_cast<double>(face->glyph->advance.x) / units;
            slot.precise_y = static_cast<double>(face->glyph->advance.y) / units;
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
    };

    // GlyphLayoutEngine::LayoutGlyphs (src/servers/app/font/
    // GlyphLayoutEngine.h:256-372) without offsets: one glyph per UTF8ToCharCode
    // code, no shaping, no ligatures. B_STRING_SPACING kerns through
    // FT_Get_Kerning -- with the *character codes*, where FreeType expects glyph
    // indices (FontEngine::GetKerning, FontEngine.cpp:621-637); that is what
    // app_server does, so it is what this does. B_CHAR_SPACING advances by the
    // unhinted advance times the font's *unrounded* size; everything else by the
    // hinted advance.
    Layout layout(std::string_view text, const Font& font,
                  const EscapementDelta* delta, bool force_vector)
    {
        Layout result;
        result.face = face_for(font);
        if (result.face == nullptr)
            return result;
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
        while (length > 0 && (code = utf8_to_char_code(&cursor)) != 0) {
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

            // FontCacheEntry::CreateGlyph and GlyphLayoutEngine::_CreateGlyph:
            // the font's glyph, else the first fallback that has one, else a
            // zero-width nothing, the font's space, or its .notdef.
            Laid laid;
            laid.x = x;
            laid.y = y;
            laid.face = result.face;
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
    return impl_->candidates(face_key(font));
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
    while (length > 0 && (code = utf8_to_char_code(&cursor)) != 0) {
        estimate += font.size * 0.6f;
        if (delta != nullptr)
            estimate += is_haiku_white_space(code) ? delta->space : delta->nonspace;
        if (cursor - start + 1 > length)
            break;
    }
    return estimate;
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

    // The view transform belongs in the glyph *outline*, not in the pixels the
    // outline rasterised to. Forward-mapping a finished raster through
    // map_point() lands each source pixel on one destination pixel and never
    // writes the gaps between them, so a scaled string came out as a lattice of
    // dots instead of a scaled string (issue #21).
    //
    // app_server puts it in the outline for the same reason: a transform that is
    // not translation-only makes StringRenderer::NeedsVector() true
    // (AGGTextRenderer.cpp:146), which selects glyph_ren_outline over the cached
    // bitmap (FontCacheEntry.cpp:429-435), and the outline is then pushed
    // through `transform = embedded; transform.TranslateBy(baseLine); transform
    // *= fViewTransformation` before the rasteriser sees it
    // (AGGTextRenderer.cpp:379-391).
    //
    // FT_Matrix_Multiply(a, b) computes `b = a * b`, so this composes to
    // view * embedded -- the server's order, with the font's own rotate/shear
    // innermost. A view transform with an identity linear part composes exactly
    // (FT_MulFix by 0x10000 is lossless), so the untransformed path is
    // untouched.
    FT_Matrix render_matrix = laid.matrix;
    FT_Matrix view = view_matrix(state.transform);
    FT_Matrix_Multiply(&view, &render_matrix);
    const bool render_transformed = laid.transformed || view_linear;

    // The view transform's *translation* stays out of the matrix and rides on
    // the baseline instead, which is what the server does: it translates by the
    // baseline inside the transform and then multiplies the view transform in
    // (AGGTextRenderer.cpp:379-381), so the baseline lands at
    // viewTransformation(baseLine). DrawState::map_point is that mapping,
    // including Painter::SetTransform's translate(-offset)/translate(+offset)
    // bracket around the affine (Painter.cpp:372-383), and it returns the point
    // unchanged for an identity transform.
    const Point origin = state.map_point(baseline);

    // 26.6 fixed point, the units FT_Outline_EmboldenXY works in.
    const auto embolden_strength = static_cast<FT_Pos>(
        std::lround(static_cast<double>(state.font.false_bold_width) * 64.0));
    const bool antialias = font_antialiased(state.font);
    const bool subpixel = render_settings(state.font).subpixel;
    const bool outline = laid.rendering == GlyphRendering::outline;
    // A cached bitmap is rendered for the LCD only on glyph_ren_subpix; an
    // outline goes through AGG's subpixel rasteriser when subpixel antialiasing
    // is on and the font is antialiased (AGGTextRenderer.cpp:141, 278-290).
    const FT_Render_Mode render_mode = !antialias ? FT_RENDER_MODE_MONO
        : (outline ? (subpixel ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL)
                   : (laid.rendering == GlyphRendering::subpix
                          ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL));

    for (const auto& glyph_at : laid.glyphs) {
        if (!glyph_at.ink)
            continue;
        FT_Face face = glyph_at.face;
        FT_Set_Pixel_Sizes(face, laid.ppem, laid.ppem);
        install_transform(face, render_matrix, false);
        auto load_flags = laid.load_flags;
        // A transformed glyph is hinted at the font's own ppem and only then
        // transformed, as on the server (FontEngine::PrepareGlyph hints, then
        // AGGTextRenderer transforms the outline). `disable_hinting` is a test
        // hook: with it, a scaled render and a render at the scaled ppem share
        // one unhinted outline, so they can be compared on any face.
        if (disable_hinting)
            load_flags |= FT_LOAD_NO_HINTING;
        if (FT_Load_Glyph(face, glyph_at.index, load_flags) != 0
            || !transform_glyph(face->glyph, render_matrix, render_transformed)
            || !embolden_glyph(face->glyph, embolden_strength)
            || !glyph_is_renderable(face->glyph)) {
            continue;
        }

        // Where the glyph's origin lands on the surface. The layout's y grows
        // downwards and FreeType's upwards, so the y offset is negated going
        // into the y-up matrix and back out of it.
        const auto [placed_x, placed_up] = render_transformed
            ? transform_point(render_matrix, static_cast<float>(glyph_at.x),
                              static_cast<float>(-glyph_at.y))
            : std::pair<float, float> {static_cast<float>(glyph_at.x),
                                       static_cast<float>(-glyph_at.y)};
        const double pen_x = static_cast<double>(origin.x) + placed_x;
        const double pen_y = static_cast<double>(origin.y) - placed_up;
        int origin_x = 0;
        int origin_y = 0;
        if (outline && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            // An outline is rasterised where it is, to the subpixel
            // (FontCacheEntry::InitAdaptors hands the path adaptor the exact
            // x/y). FreeType renders relative to a whole-pixel origin, so move
            // the fractional part into the outline first.
            origin_x = static_cast<int>(std::floor(pen_x));
            origin_y = static_cast<int>(std::floor(pen_y));
            FT_Outline_Translate(
                &face->glyph->outline,
                static_cast<FT_Pos>(std::lround((pen_x - origin_x) * 64.0)),
                -static_cast<FT_Pos>(std::lround((pen_y - origin_y) * 64.0)));
        } else {
            // A cached bitmap is placed at the rounded pen position.
            origin_x = agg_iround(pen_x);
            origin_y = agg_iround(pen_y);
        }
        if (FT_Render_Glyph(face->glyph, render_mode) != 0)
            continue;

        const auto& glyph = *face->glyph;
        origin_x += glyph.bitmap_left;
        origin_y -= glyph.bitmap_top;
        const auto pitch = static_cast<unsigned>(std::abs(glyph.bitmap.pitch));
        for (unsigned row = 0; row < glyph.bitmap.rows; ++row) {
            const auto source_row = glyph.bitmap.pitch >= 0
                ? row : glyph.bitmap.rows - 1 - row;
            const auto* pixels = glyph.bitmap.buffer + source_row * pitch;
            if (glyph.bitmap.pixel_mode == FT_PIXEL_MODE_LCD) {
                const unsigned pixel_width = glyph.bitmap.width / 3;
                for (unsigned column = 0; column < pixel_width; ++column) {
                    const auto red = pixels[column * 3];
                    const auto green = pixels[column * 3 + 1];
                    const auto blue = pixels[column * 3 + 2];
                    if (red == 0 && green == 0 && blue == 0)
                        continue;
                    surface.paint_subpixel_coverage(
                        origin_x + static_cast<int>(column),
                        origin_y + static_cast<int>(row),
                        state.high, state, red, green, blue);
                }
            } else if (glyph.bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                for (unsigned column = 0; column < glyph.bitmap.width; ++column) {
                    if ((pixels[column / 8] & (0x80u >> (column & 7))) == 0)
                        continue;
                    surface.paint_coverage(
                        origin_x + static_cast<int>(column),
                        origin_y + static_cast<int>(row),
                        state.high, state, 255, true);
                }
            } else {
                for (unsigned column = 0; column < glyph.bitmap.width; ++column) {
                    const auto coverage = pixels[column];
                    if (coverage == 0)
                        continue;
                    surface.paint_coverage(
                        origin_x + static_cast<int>(column),
                        origin_y + static_cast<int>(row),
                        state.high, state, coverage, true);
                }
            }
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
