#pragma once

// How a desktop of one size is shown in a window of another: the presentation
// the SDL front end gets from SDL_RenderSetLogicalSize, written out so the X11
// front end (which has no renderer to ask) shows a desktop exactly the same way.
//
// sdl_main.cpp sets the renderer's logical size to the desktop's and never sets
// SDL_HINT_RENDER_LOGICAL_SIZE_MODE or integer scaling, so SDL letterboxes: the
// desktop is scaled uniformly, by a factor that need not be an integer, to the
// largest size that fits, and centred, with black bars on the two sides that are
// left over. Pointer events are mapped back through the same viewport. Everything
// here reproduces SDL 2.30's arithmetic (src/render/SDL_render.c), including its
// truncations, and cites the line it comes from.
//
// The scaling filter is SDL's too: sdl_main.cpp never sets
// SDL_HINT_RENDER_SCALE_QUALITY, so it is nearest-neighbour unless the user's
// environment sets SDL_RENDER_SCALE_QUALITY, which SDL reads as the hint.

#include "haiku_remote/types.hpp"

#include <cstdint>
#include <span>

namespace haiku_remote {

// Where a logical canvas of logical_width x logical_height lands in an output of
// device pixels: the viewport (x, y, width, height), in output pixels, and the
// uniform scale SDL renders it at.
struct Letterbox {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    float scale = 1.0f;
    int logical_width = 0;
    int logical_height = 0;

    // Every output pixel is exactly one logical pixel.
    [[nodiscard]] bool identity() const
    {
        return x == 0 && y == 0 && width == logical_width && height == logical_height;
    }

    friend bool operator==(const Letterbox&, const Letterbox&) = default;
};

// SDL_RenderSetLogicalSize's letterbox policy (UpdateLogicalSize,
// SDL_render.c:2281-2397, the non-integer, letterbox branch). An empty output or
// canvas gives an empty viewport.
[[nodiscard]] Letterbox letterbox(int output_width, int output_height, int logical_width,
                                  int logical_height);

struct IntPoint {
    int x = 0;
    int y = 0;

    friend bool operator==(const IntPoint&, const IntPoint&) = default;
};

// A pointer position in output pixels, mapped to the logical canvas the way
// SDL_RendererEventWatch maps mouse motion and button events
// (SDL_render.c:764-799): subtract the viewport's origin, divide by the scale,
// truncate toward zero. Like SDL it does not clamp, so a position over a black
// bar maps outside the canvas (a negative coordinate, or one >= its size).
[[nodiscard]] IntPoint output_to_logical(const Letterbox& view, int x, int y);

enum class ScaleFilter {
    nearest,
    linear,
};

// SDL_GetScaleMode (SDL_render.c:1241-1254) applied to the value of
// SDL_HINT_RENDER_SCALE_QUALITY: unset or "nearest" is nearest, "linear" is
// linear, "best" -- anisotropic on Direct3D, linear on every renderer that has
// no such mode -- is linear, and anything else is read as a number, 0 nearest.
[[nodiscard]] ScaleFilter scale_filter_from_hint(const char* hint);

// One layer of what is shown, in logical coordinates: BGRA pixels for the
// inclusive rectangle `rect`, `stride` bytes per row, the first at rect's top
// left. The first layer is the canvas itself; each later one (the toolbar, the
// statistics panel) is drawn over those before it.
struct ScaleLayer {
    const std::uint8_t* pixels = nullptr;
    int stride = 0;
    IntRect rect;
};

// Fill `area` (output pixels, inclusive, clipped to the output) of `out` --
// BGRA rows `out_stride` bytes apart, its first byte output pixel (0, 0) --
// with the layers as shown through `view`. Outside the viewport is black, the
// colour SDL_RenderClear leaves. The layers are composed first and the result
// filtered, so a linear filter blends across a layer's edge where SDL, which
// filters each texture on its own, would not; with nearest the two agree.
//
// Nearest takes, for output pixel d of the viewport, the logical pixel under its
// centre: floor((d + 1/2) * logical / viewport). Linear samples the same point
// between the four texels around it, as a GL_LINEAR, clamp-to-edge texture does,
// with 8-bit fractional weights (floor(256 * position)) rounded once.
void scale_layers(std::span<const ScaleLayer> layers, const Letterbox& view,
                  ScaleFilter filter, std::uint8_t* out, int out_stride, IntRect area);

// The output pixels whose value can depend on logical rectangle `changed`
// (inclusive): a superset, clipped to the viewport, so repainting it after the
// change brings the output up to date.
[[nodiscard]] IntRect output_damage(const Letterbox& view, ScaleFilter filter,
                                    IntRect changed);

} // namespace haiku_remote
