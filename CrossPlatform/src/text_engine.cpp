#include "haiku_remote/text_engine.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include <hb-ft.h>
#include <hb.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace haiku_remote {
namespace {

constexpr std::uint16_t italic_face = 0x0001;
constexpr std::uint16_t bold_face = 0x0020;
constexpr std::uint8_t fixed_spacing = 3;

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
        float advance = 0;
    };

    Shaped shape(std::string_view text, const Font& font)
    {
        Shaped result;
        result.face = face_for(font);
        if (result.face == nullptr)
            return result;
        const auto pixel_size = static_cast<FT_UInt>(
            std::max(1L, std::lround(font.size)));
        FT_Set_Pixel_Sizes(result.face, 0, pixel_size);

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

float TextEngine::width(std::string_view text, const Font& font)
{
    const auto shaped = impl_->shape(text, font);
    if (shaped.face != nullptr)
        return shaped.advance;
    std::size_t codepoints = 0;
    for (const unsigned char c : text)
        if ((c & 0xc0) != 0x80)
            ++codepoints;
    return static_cast<float>(codepoints) * font.size * 0.6f;
}

float TextEngine::draw(std::string_view text, Point baseline,
                       const DrawState& state, Surface& surface)
{
    const auto shaped = impl_->shape(text, state.font);
    if (shaped.face == nullptr)
        return width(text, state.font);

    float pen_x = baseline.x;
    float pen_y = baseline.y;
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
            && FT_Render_Glyph(shaped.face->glyph, render_mode) == 0) {
            const auto& glyph = *shaped.face->glyph;
            const int origin_x = static_cast<int>(std::floor(
                pen_x + static_cast<float>(position.x_offset) / 64.0f))
                + glyph.bitmap_left;
            const int origin_y = static_cast<int>(std::floor(
                pen_y - static_cast<float>(position.y_offset) / 64.0f))
                - glyph.bitmap_top;
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
        pen_x += static_cast<float>(position.x_advance) / 64.0f;
        pen_y -= static_cast<float>(position.y_advance) / 64.0f;
    }
    return shaped.advance;
}

} // namespace haiku_remote
