#pragma once

// The subpixel (LCD) text-blend cases shared by the Painter oracle
// (tools/painter_oracle/text_blend.cpp) and tests/render_tests.cpp.
//
// Each case is one glyph pixel: a destination pixel, a drawing state, and the
// three LCD covers FreeType gives that pixel, left to right. The oracle sets
// the state through app_server's own Painter setters -- SetDrawingMode,
// SetBlendingMode, SetPattern(B_SOLID_HIGH) as SolidPatternGuard does for
// every string (Painter.cpp:1492), SetHighColor, SetLowColor -- and blends
// through the fBlendSolidHSpanSubpix span PixelFormat::SetDrawingMode
// installed, with the renderer colour _SetRendererColor gave the subpixel
// renderer (Painter.cpp:1721-1726): exactly the call the glyph scanline makes.
// The test runs the same case through the client and compares the pixel for
// equality.
//
// Data only, plain C++17 with no dependencies (both sides include it). The
// product is enumerated by for_each_case() so the two sides cannot disagree on
// the order.

#include <cstdint>

namespace text_blend_cases {

struct Rgba {
    std::uint8_t r, g, b, a;
};

// drawing_mode, source_alpha, alpha_function as GraphicsDefs.h numbers them.
struct Mode {
    std::uint32_t mode;
    std::uint32_t source_alpha;   // B_PIXEL_ALPHA 0, B_CONSTANT_ALPHA 1
    std::uint32_t alpha_function; // B_ALPHA_OVERLAY 0, B_ALPHA_COMPOSITE 1, ...
};

// Every drawing mode, and B_OP_ALPHA under each source alpha and the two real
// alpha functions, plus three Porter-Duff functions under B_PIXEL_ALPHA
// (SOURCE_IN 2, XOR 9, DARKEN 13), whose subpixel span is AggCompOpAdapter's
// no-op. B_CONSTANT_ALPHA with a Porter-Duff function is left out on purpose:
// SetDrawingMode installs no span for it (PixelFormat.cpp:275-298 has no
// else), so the result is whatever the previous mode left -- nothing a client
// can be told, and nothing an oracle can pin.
inline constexpr Mode modes[] = {
    {0, 0, 0},  {1, 0, 0},  {2, 0, 0},  {3, 0, 0},  {4, 0, 0},
    {5, 0, 0},  {6, 0, 0},  {7, 0, 0},  {8, 0, 0},  {9, 0, 0},
    {10, 0, 0}, {10, 0, 1}, {10, 1, 0}, {10, 1, 1},
    {10, 0, 2}, {10, 0, 9}, {10, 0, 13},
};

// High colours: opaque, half, and almost transparent -- B_OP_ALPHA weighs the
// covers by the alpha, everything else must ignore it.
inline constexpr Rgba highs[] = {
    {10, 220, 90, 255},
    {200, 40, 170, 128},
    {250, 250, 5, 3},
};

inline constexpr Rgba low = {30, 40, 250, 255};

// Destinations: asymmetric in every channel; the first high colour and the
// low colour themselves (B_OP_SELECT swaps exactly those); black and white
// (B_OP_ADD / SUBTRACT saturate, MIN / MAX flip). Opaque, as the screen is.
inline constexpr Rgba destinations[] = {
    {200, 100, 50, 255},
    {10, 220, 90, 255},
    {30, 40, 250, 255},
    {0, 0, 0, 255},
    {255, 255, 255, 255},
};

// Covers, left to right: none, full, a ramp each way, and single subpixels --
// any swap of the left and right cover changes the result.
struct Covers {
    std::uint8_t c[3];
};
inline constexpr Covers covers[] = {
    {{0, 0, 0}},     {{255, 255, 255}}, {{64, 128, 192}},
    {{192, 128, 64}}, {{255, 0, 0}},    {{0, 0, 255}},
    {{17, 250, 3}},
};

// gSubpixelOrderingRGB: the server default (true) and BGR.
inline constexpr bool orderings[] = {true, false};

struct Case {
    Mode mode;
    Rgba high;
    Rgba destination;
    Covers covers;
    bool ordering_rgb;
};

template <typename F>
void for_each_case(F&& f)
{
    for (bool ordering : orderings)
        for (const Mode& mode : modes)
            for (const Rgba& high : highs)
                for (const Rgba& destination : destinations)
                    for (const Covers& c : covers)
                        f(Case {mode, high, destination, c, ordering});
}

} // namespace text_blend_cases
