#pragma once

// The text scenes shared by the text half of the Painter oracle
// (tools/painter_oracle/text_oracle.cpp) and tests/render_tests.cpp.
//
// Each scene is one Painter::DrawString of one string in one font, on a small
// canvas with a background that differs in every pixel. The oracle replays it
// through app_server's own AGGTextRenderer, GlyphLayoutEngine, FontCacheEntry
// and FontEngine, the Painter and PixelFormat behind them, and the vendored
// FreeType 2.6.3 the DeBeOS image renders glyphs with (third_party/freetype),
// all compiled on the host; the test draws the same string through the
// client's TextEngine and compares every pixel for equality. Nothing in here
// is rendering logic -- it is data, plus the background both sides paint.
//
// The faces are files in tests/fonts (see its README), so both sides read the
// same bytes on every host. Plain C++17, no dependencies: both sides include it.

#include <cstdint>
#include <vector>

namespace text_scenes {

struct Face {
    const char* file;    // under tests/fonts
    const char* family;  // FreeType's family_name for it
    const char* style;   // FreeType's style_name
    bool fixed;          // FT_IS_FIXED_WIDTH (ServerFont::IsFixedWidth)
};

// Noto Sans and DejaVu Sans are TrueType with bytecode, so hinted text runs
// FreeType's v35 interpreter; Bitstream Charter is Type 1 (.pfb), hinted by
// FreeType's PostScript hinter -- the path the image's be_fixed_font fell on in
// the live comparison. DejaVu Sans Mono is monospaced, for
// HINTING_MODE_MONOSPACED_ONLY.
inline const std::vector<Face>& faces()
{
    static const std::vector<Face> list = {
        {"NotoSans-Regular.ttf", "Noto Sans", "Regular", false},
        {"DejaVuSans.ttf", "DejaVu Sans", "Book", false},
        {"bchr.pfb", "Bitstream Charter", "Regular", false},
        {"DejaVuSansMono.ttf", "DejaVu Sans Mono", "Book", true},
    };
    return list;
}

// GlobalSubpixelSettings.h:14-18
constexpr std::uint8_t hinting_off = 0;
constexpr std::uint8_t hinting_on = 1;
constexpr std::uint8_t hinting_monospaced_only = 2;

// headers/os/interface/Font.h
constexpr std::uint32_t disable_antialiasing = 0x00000001;
constexpr std::uint8_t char_spacing = 0;
constexpr std::uint8_t string_spacing = 1;
constexpr std::uint8_t bitmap_spacing = 2;
constexpr std::uint16_t underscore_face = 0x0002;

// drawing_mode, GraphicsDefs.h
constexpr std::uint32_t op_copy = 0;
constexpr std::uint32_t op_over = 1;
constexpr std::uint32_t op_blend = 4;
constexpr std::uint32_t op_alpha = 10;

struct Scene {
    const char* name;
    int width;
    int height;
    int face;               // index into faces()
    float size;
    const char* text;
    float x;                // DrawString's baseline
    float y;
    // The desktop settings that are not on the wire: gDefaultHintingMode and
    // gSubpixelAntialiasing (DesktopSettings.cpp:88-89 default them to
    // HINTING_MODE_ON and true). The client is told both per font, in the
    // identity block (RemoteMessage::AddFontIdentity).
    std::uint8_t hinting = hinting_on;
    bool subpixel = true;
    std::uint32_t flags = 0;              // font flags
    std::uint8_t spacing = bitmap_spacing;
    std::uint16_t face_bits = 0;          // ServerFont::Face()
    float rotation = 0;
    float shear = 90;
    float false_bold = 0;
    std::uint32_t mode = op_copy;
    std::uint32_t source_alpha = 0;       // B_PIXEL_ALPHA
    std::uint32_t alpha_function = 0;     // B_ALPHA_OVERLAY
    std::uint8_t high[4] = {20, 24, 90, 255};
    std::uint8_t low[4] = {224, 220, 200, 255};
    // The view transform, BAffineTransform order (sx, shy, shx, sy, tx, ty).
    double transform[6] = {1, 0, 0, 1, 0, 0};
};

// The background both sides paint before drawing: every pixel differs from its
// neighbours and the canvas is not symmetric, so a glyph blended against the
// wrong destination, or placed a pixel off, cannot match by accident.
inline void background(int x, int y, std::uint8_t rgb[3])
{
    rgb[0] = static_cast<std::uint8_t>(196 + (x * 5 + y * 3) % 47);
    rgb[1] = static_cast<std::uint8_t>(200 + (x * 3 + y * 7) % 41);
    rgb[2] = static_cast<std::uint8_t>(188 + (x * 11 + y * 2) % 53);
}

inline std::vector<Scene> scenes()
{
    const char* sample = "Hamburgefonstiv 0123 AWAY fi";
    const char* charter = "Terminal ~/home $ ls -la 0O1lI|";
    std::vector<Scene> list;
    auto add = [&](const char* name, int face, float size, const char* text) -> Scene& {
        Scene s;
        s.name = name;
        s.width = 260;
        s.height = size > 20 ? 64 : 34;
        s.face = face;
        s.size = size;
        s.text = text;
        s.x = 5;
        s.y = size > 20 ? 44 : 22;
        list.push_back(s);
        return list.back();
    };
    // The default desktop: hinted, subpixel antialiasing on -- the LCD bitmap
    // path (glyph_ren_subpix), at the sizes the desktop uses.
    for (int face = 0; face < 3; ++face) {
        static const char* names[3][4] = {
            {"noto 9", "noto 12", "noto 16", "noto 24"},
            {"dejavu 9", "dejavu 12", "dejavu 16", "dejavu 24"},
            {"charter 9", "charter 12", "charter 16", "charter 24"},
        };
        const float sizes[4] = {9, 12, 16, 24};
        for (int i = 0; i < 4; ++i)
            add(names[face][i], face, sizes[i], face == 2 ? charter : sample);
    }
    add("noto 12.5", 0, 12.5f, sample);       // the ppem truncates to 12
    add("noto 30", 0, 30, "Hamburg 0123");    // the last cached-bitmap size
    // Subpixel antialiasing off: the greyscale bitmap path (glyph_ren_native_gray8).
    add("noto 12 gray", 0, 12, sample).subpixel = false;
    add("dejavu 12 gray", 1, 12, sample).subpixel = false;
    add("charter 12 gray", 2, 12, charter).subpixel = false;
    // Hinting off, B_DISABLE_ANTIALIASING and anything over 30px all go down
    // the outline path (FontCacheEntry::_RenderTypeFor): AGG rasterises the
    // decomposed outline, through the subpixel rasteriser when subpixel
    // antialiasing is on and the font is antialiased.
    add("noto 12 unhinted", 0, 12, sample).hinting = hinting_off;
    add("dejavu 12 unhinted", 1, 12, sample).hinting = hinting_off;
    add("charter 12 unhinted", 2, 12, charter).hinting = hinting_off;
    {
        Scene& s = add("noto 12 unhinted gray", 0, 12, sample);
        s.hinting = hinting_off;
        s.subpixel = false;
    }
    add("noto 12 aliased", 0, 12, sample).flags = disable_antialiasing;
    add("charter 12 aliased", 2, 12, charter).flags = disable_antialiasing;
    add("noto 36", 0, 36, "Hamburg 0123");
    add("charter 36", 2, 36, "Term ~/home");
    // HINTING_MODE_MONOSPACED_ONLY hints only a fixed-width face.
    add("mono 12 monospaced-only", 3, 12, sample).hinting = hinting_monospaced_only;
    add("noto 12 monospaced-only", 0, 12, sample).hinting = hinting_monospaced_only;
    // Spacing: B_CHAR_SPACING lays out on unhinted advances, B_STRING_SPACING
    // kerns.
    add("noto 12 char spacing", 0, 12, sample).spacing = char_spacing;
    add("dejavu 12 string spacing", 1, 12, "AWAY To Vo LT").spacing = string_spacing;
    // Drawing modes: B_OP_COPY blends against the low colour, the others
    // against the destination.
    add("noto 12 over", 0, 12, sample).mode = op_over;
    add("charter 12 over", 2, 12, charter).mode = op_over;
    add("noto 12 blend", 0, 12, sample).mode = op_blend;
    {
        Scene& s = add("noto 12 alpha", 0, 12, sample);
        s.mode = op_alpha;
        s.high[3] = 150;
    }
    {
        Scene& s = add("noto 12 alpha gray", 0, 12, sample);
        s.mode = op_alpha;
        s.high[3] = 150;
        s.subpixel = false;
    }
    // Decorations and the embedded transform.
    add("noto 12 underscore", 0, 12, sample).face_bits = underscore_face;
    {
        Scene& s = add("noto 16 rotated", 0, 16, "Hamburg");
        s.rotation = 12;
        s.height = 64;
        s.y = 50;
    }
    add("noto 16 sheared", 0, 16, sample).shear = 75;
    add("noto 16 false bold", 0, 16, sample).false_bold = 1;
    {
        Scene& s = add("noto 12 view scaled", 0, 12, "Hamburg 0123");
        s.transform[0] = 1.5;
        s.transform[3] = 1.5;
        s.height = 44;
    }
    // The view-transform conformance checks' own string, size and scales
    // (render_tests.cpp, issue #21): a 2x view scale, anti-aliased and
    // B_DISABLE_ANTIALIASING, and a 10x one. The forward pixel scatter those
    // checks guard against inks a lattice here, so these are where "filled,
    // not a lattice" is stated exactly.
    for (const bool aliased : {false, true}) {
        Scene& s = add(aliased ? "noto 18 aliased view scaled 2x"
                               : "noto 18 view scaled 2x",
                       0, 18, "Hamburgefonstiv");
        s.transform[0] = 2;
        s.transform[3] = 2;
        s.width = 320;
        s.height = 64;
        if (aliased)
            s.flags = disable_antialiasing;
    }
    {
        Scene& s = add("noto 18 view scaled 10x", 0, 18, "Hm");
        s.transform[0] = 10;
        s.transform[3] = 10;
        s.x = 1;
        s.y = 14;
        s.width = 330;
        s.height = 190;
    }
    return list;
}

} // namespace text_scenes
