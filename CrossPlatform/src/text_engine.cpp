#include "haiku_remote/text_engine.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_LCD_FILTER_H
#include FT_OUTLINE_H
#include <hb-ft.h>
#include <hb.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace haiku_remote {
namespace {

constexpr std::uint16_t italic_face = 0x0001;
constexpr std::uint16_t bold_face = 0x0020;
constexpr std::uint8_t fixed_spacing = 3;

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
    rotate.xx = static_cast<FT_Fixed>(std::cos(rotation) * ft_fixed_one);
    rotate.xy = static_cast<FT_Fixed>(-std::sin(rotation) * ft_fixed_one);
    rotate.yx = static_cast<FT_Fixed>(std::sin(rotation) * ft_fixed_one);
    rotate.yy = static_cast<FT_Fixed>(std::cos(rotation) * ft_fixed_one);

    const double shear = static_cast<double>(font.shear) * degrees_to_radians;
    FT_Matrix result;
    result.xx = static_cast<FT_Fixed>(ft_fixed_one);
    result.xy = static_cast<FT_Fixed>(-std::cos(shear) * ft_fixed_one);
    result.yx = 0;
    result.yy = static_cast<FT_Fixed>(ft_fixed_one);

    // FT_Matrix_Multiply(a, b) computes `b = a * b`.
    FT_Matrix_Multiply(&rotate, &result);
    return result;
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

// Always called, never skipped: the FT_Face cache is shared between fonts, and
// FT_Set_Transform is face state, so an earlier rotated font's matrix would
// otherwise stay installed and rotate an unrotated string.
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

    friend bool operator<(const FaceKey& a, const FaceKey& b)
    {
        return std::tie(a.mono, a.bold, a.italic)
            < std::tie(b.mono, b.bold, b.italic);
    }
};

std::vector<std::string> font_candidates(FaceKey key)
{
    std::vector<std::string> result;
    if (const char* configured = std::getenv(
            key.mono ? "HAIKU_REMOTE_MONO_FONT" : "HAIKU_REMOTE_FONT")) {
        result.emplace_back(configured);
    }

    if (key.mono) {
        if (key.bold && key.italic) {
            result.emplace_back("/usr/share/fonts/dejavu-sans-mono-fonts/"
                                "DejaVuSansMono-BoldOblique.ttf");
        } else if (key.bold) {
            result.emplace_back("/usr/share/fonts/dejavu-sans-mono-fonts/"
                                "DejaVuSansMono-Bold.ttf");
        } else if (key.italic) {
            result.emplace_back("/usr/share/fonts/dejavu-sans-mono-fonts/"
                                "DejaVuSansMono-Oblique.ttf");
        }
        result.emplace_back("/usr/share/fonts/google-noto/NotoSansMono-Regular.ttf");
        result.emplace_back("/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf");
        result.emplace_back("C:/Windows/Fonts/consola.ttf");
        result.emplace_back("/System/Library/Fonts/Menlo.ttc");
    } else {
        if (key.bold)
            result.emplace_back("/usr/share/fonts/cantarell/Cantarell-Bold.otf");
        result.emplace_back("/usr/share/fonts/google-noto/NotoSans-Regular.ttf");
        result.emplace_back("/usr/share/fonts/cantarell/Cantarell-Regular.otf");
        result.emplace_back("C:/Windows/Fonts/arial.ttf");
        result.emplace_back("/System/Library/Fonts/Helvetica.ttc");
    }
    return result;
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

// Decodes the UTF-8 scalar at `offset`, reporting its byte length. A truncated
// or malformed sequence is reported as its lead byte alone: classification only
// has to be right for the well-formed whitespace scalars above, and advancing a
// single byte keeps this walk in step with HarfBuzz's cluster byte offsets.
std::uint32_t decode_utf8(std::string_view text, std::size_t offset,
                          std::size_t& length)
{
    const auto lead = static_cast<unsigned char>(text[offset]);
    length = 1;
    std::size_t continuations = 0;
    std::uint32_t code = lead;
    if ((lead & 0xf8u) == 0xf0u) {
        continuations = 3;
        code = lead & 0x07u;
    } else if ((lead & 0xf0u) == 0xe0u) {
        continuations = 2;
        code = lead & 0x0fu;
    } else if ((lead & 0xe0u) == 0xc0u) {
        continuations = 1;
        code = lead & 0x1fu;
    } else {
        return lead;
    }

    if (offset + continuations >= text.size())
        return lead;
    for (std::size_t i = 1; i <= continuations; ++i) {
        const auto byte = static_cast<unsigned char>(text[offset + i]);
        if ((byte & 0xc0u) != 0x80u)
            return lead;
        code = code << 6 | (byte & 0x3fu);
    }
    length = continuations + 1;
    return code;
}

// The extra advance an escapement_delta adds for each character of `text`, as
// (byte offset of the character, extra advance) pairs in byte order.
std::vector<std::pair<std::size_t, float>> character_deltas(
    std::string_view text, const EscapementDelta& delta)
{
    std::vector<std::pair<std::size_t, float>> result;
    for (std::size_t offset = 0; offset < text.size();) {
        std::size_t length = 1;
        const auto code = decode_utf8(text, offset, length);
        result.emplace_back(
            offset, is_haiku_white_space(code) ? delta.space : delta.nonspace);
        offset += length;
    }
    return result;
}

} // namespace

struct TextEngine::Impl {
    FT_Library library = nullptr;
    std::map<FaceKey, FT_Face> faces;

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
        for (const auto& [key, face] : faces) {
            (void)key;
            FT_Done_Face(face);
        }
        if (library != nullptr)
            FT_Done_FreeType(library);
    }

    FT_Face face_for(const Font& font)
    {
        if (library == nullptr)
            return nullptr;
        const FaceKey key {
            font.spacing == fixed_spacing,
            (font.face & bold_face) != 0,
            (font.face & italic_face) != 0,
        };
        if (const auto found = faces.find(key); found != faces.end())
            return found->second;
        for (const auto& path : font_candidates(key)) {
            if (!std::filesystem::exists(path))
                continue;
            FT_Face face = nullptr;
            if (FT_New_Face(library, path.c_str(), 0, &face) == 0) {
                faces.emplace(key, face);
                return face;
            }
        }
        return nullptr;
    }

    struct Shaped {
        FT_Face face = nullptr;
        std::vector<hb_glyph_info_t> glyphs;
        std::vector<hb_glyph_position_t> positions;
        // Escapement-delta advance charged to each glyph, parallel to `glyphs`.
        std::vector<float> extra;
        // `advance` is the *untransformed* horizontal advance, which is what
        // the server's own answer to the same question is: ServerFont::
        // StringWidth uses StringWidthConsumer, whose `Finish(x, y)` keeps a
        // bare `x` with no embedded transform applied -- unlike
        // AGGTextRenderer's StringRenderer::Finish, which does transform the
        // reported pen position. Rotation therefore must NOT change this.
        float advance = 0;
        // Embedded rotate/shear transform for `face`, already installed on it.
        FT_Matrix matrix {};
        bool transformed = false;
    };

    // Charges every character's escapement delta to the last glyph of the
    // cluster it belongs to, so the extra advance lands between clusters, and
    // folds the same amounts into the total advance.
    //
    // Haiku adds the delta to a character's advance *after* the glyph is
    // emitted (GlyphLayoutEngine.h:349-352) and then folds the final advance
    // into the pen position it reports (GlyphLayoutEngine.h:367-369) -- so the
    // last character's delta moves the pen past the last glyph even though it
    // shifts no ink. Summing every character's delta into `advance` reproduces
    // that exactly.
    //
    // Haiku emits one glyph per character, so its rule is per-glyph; HarfBuzz
    // may map several characters to one glyph (a ligature) or one character to
    // several. Grouping by cluster keeps the total identical to Haiku's in
    // either case, and is per-glyph whenever the mapping is one to one.
    // `shaped.extra` arrives sized to the glyph run and zeroed.
    static void charge_delta(Shaped& shaped, std::string_view text,
                             const EscapementDelta& delta)
    {
        if (shaped.glyphs.empty() || text.empty())
            return;

        // Byte offset -> last glyph carrying that offset as its cluster.
        std::vector<int> cluster_owner(text.size(), -1);
        for (std::size_t i = 0; i < shaped.glyphs.size(); ++i) {
            const std::size_t at = shaped.glyphs[i].cluster;
            if (at < text.size())
                cluster_owner[at] = static_cast<int>(i);
        }

        // A character before the first cluster start (HarfBuzz should not
        // produce one, but nothing here depends on that) is charged to the
        // first glyph rather than dropped.
        int owner = 0;
        for (const auto& [offset, extra] : character_deltas(text, delta)) {
            if (cluster_owner[offset] >= 0)
                owner = cluster_owner[offset];
            shaped.extra[static_cast<std::size_t>(owner)] += extra;
            shaped.advance += extra;
        }
    }

    Shaped shape(std::string_view text, const Font& font,
                 const EscapementDelta* delta)
    {
        Shaped result;
        result.face = face_for(font);
        if (result.face == nullptr)
            return result;
        const auto pixel_size = static_cast<FT_UInt>(
            std::max(1L, std::lround(font.size)));
        FT_Set_Pixel_Sizes(result.face, 0, pixel_size);
        // Shaping runs on an *untransformed* face, and the matrix is only
        // handed to `draw` to install around the render loop.
        //
        // This is not a style choice. Measured here: with the matrix installed
        // before hb_shape, a 90-degree rotated "Hamburgefonstiv" at size 18
        // came back with a total advance of -0.19 px instead of 147.81 and
        // every glyph piled into a 19x16 box, because HarfBuzz's horizontal
        // advances came back rotated -- it reports no vertical advance for a
        // horizontal run, so the rotated advance simply vanished. It also made
        // `width()` rotation-dependent, which would contradict the server's own
        // fallback (ServerFont::StringWidth is untransformed).
        //
        // It matches app_server too: FontEngine::Init never calls
        // FT_Set_Transform, so Haiku's layout face is untransformed and only
        // the rendering path carries the embedded transform.
        //
        // The reset is unconditional because FT_Set_Transform is face state and
        // the face cache is shared between fonts: a previous rotated font's
        // matrix left installed would silently rotate this string's advances.
        result.transformed = font_is_transformed(font);
        result.matrix = font_matrix(font);
        install_transform(result.face, result.matrix, false);

        hb_font_t* hb_font = hb_ft_font_create_referenced(result.face);
        const auto load_flags = (font.flags & 0x00000001u) == 0
            ? FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD
            : FT_LOAD_DEFAULT | FT_LOAD_TARGET_MONO;
        // Haiku lays out text with FreeType's hinted advances. HarfBuzz
        // otherwise uses fractional linear advances, which makes terminal text
        // wider than the background cell rectangles sent by app_server.
        hb_ft_font_set_load_flags(hb_font, load_flags);
        hb_ft_font_changed(hb_font);
        hb_buffer_t* buffer = hb_buffer_create();
        hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0,
                           static_cast<int>(text.size()));
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(hb_font, buffer, nullptr, 0);

        unsigned count = 0;
        const auto* infos = hb_buffer_get_glyph_infos(buffer, &count);
        const auto* positions = hb_buffer_get_glyph_positions(buffer, &count);
        result.glyphs.assign(infos, infos + count);
        result.positions.assign(positions, positions + count);
        for (const auto& position : result.positions)
            result.advance += static_cast<float>(position.x_advance) / 64.0f;
        result.extra.assign(result.glyphs.size(), 0.0f);
        if (delta != nullptr)
            charge_delta(result, text, *delta);

        hb_buffer_destroy(buffer);
        hb_font_destroy(hb_font);
        return result;
    }
};

TextEngine::TextEngine()
    : impl_(std::make_unique<Impl>())
{
}

TextEngine::~TextEngine() = default;

bool TextEngine::available() const
{
    return impl_->library != nullptr;
}

float TextEngine::width(std::string_view text, const Font& font,
                        const EscapementDelta* delta)
{
    const auto shaped = impl_->shape(text, font, delta);
    if (shaped.face != nullptr)
        return shaped.advance;
    std::size_t codepoints = 0;
    for (const unsigned char c : text)
        if ((c & 0xc0) != 0x80)
            ++codepoints;
    float estimate = static_cast<float>(codepoints) * font.size * 0.6f;
    if (delta != nullptr) {
        // No face, so no glyphs to charge -- but the delta is the server's own
        // arithmetic, not the font's, so it still belongs in the total.
        for (const auto& [offset, extra] : character_deltas(text, *delta)) {
            (void)offset;
            estimate += extra;
        }
    }
    return estimate;
}

float TextEngine::draw(std::string_view text, Point baseline,
                       const DrawState& state, Surface& surface,
                       const EscapementDelta* delta)
{
    const auto shaped = impl_->shape(text, state.font, delta);
    if (shaped.face == nullptr)
        return width(text, state.font, delta);

    // Installed for the render pass only -- `shape` deliberately left the face
    // untransformed so HarfBuzz's advances stay in layout space. `shape` resets
    // it on every call, so it need not be undone here.
    install_transform(shaped.face, shaped.matrix, shaped.transformed);

    // Accumulated baseline offset in FreeType's y-up space, *before* the
    // embedded rotate/shear transform -- exactly the x/y Haiku's
    // GlyphLayoutEngine hands its consumer. Both the glyph outline (via
    // FT_Set_Transform above) and this offset go through the same matrix, which
    // is what makes a rotated string march off along the rotated baseline
    // instead of staying horizontal.
    float offset_x = 0;
    float offset_y = 0;
    // 26.6 fixed point, the units FT_Outline_EmboldenXY works in.
    const auto embolden_strength = static_cast<FT_Pos>(
        std::lround(static_cast<double>(state.font.false_bold_width) * 64.0));
    for (std::size_t i = 0; i < shaped.glyphs.size(); ++i) {
        const auto& info = shaped.glyphs[i];
        const auto& position = shaped.positions[i];
        const bool antialias = (state.font.flags & 0x00000001u) == 0;
        const auto load_flags = antialias
            ? FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD
            : FT_LOAD_DEFAULT | FT_LOAD_TARGET_MONO;
        const auto render_mode = antialias
            ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_MONO;
        if (FT_Load_Glyph(shaped.face, info.codepoint, load_flags) == 0
            && embolden_glyph(shaped.face->glyph, embolden_strength)
            && FT_Render_Glyph(shaped.face->glyph, render_mode) == 0) {
            const auto& glyph = *shaped.face->glyph;
            const float glyph_x = offset_x
                + static_cast<float>(position.x_offset) / 64.0f;
            const float glyph_y = offset_y
                + static_cast<float>(position.y_offset) / 64.0f;
            const auto [placed_x, placed_y] = shaped.transformed
                ? transform_point(shaped.matrix, glyph_x, glyph_y)
                : std::pair<float, float> {glyph_x, glyph_y};
            const int origin_x = static_cast<int>(
                std::floor(baseline.x + placed_x)) + glyph.bitmap_left;
            const int origin_y = static_cast<int>(
                std::floor(baseline.y - placed_y)) - glyph.bitmap_top;
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
        offset_x += static_cast<float>(position.x_advance) / 64.0f
            + shaped.extra[i];
        offset_y += static_cast<float>(position.y_advance) / 64.0f;
    }
    // The untransformed advance, matching ServerFont::StringWidth (see the note
    // on Shaped::advance). The caller turns it into a pen position of
    // {where.x + advance, where.y}, which is still wrong for a rotated string
    // -- app_server's StringRenderer::Finish transforms that point. Fixing it
    // means changing the RP_DRAW_STRING reply in session.cpp; that file is not
    // touched here.
    return shaped.advance;
}

} // namespace haiku_remote
