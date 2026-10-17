#include "haiku_remote/viewport.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace haiku_remote {

Letterbox letterbox(int output_width, int output_height, int logical_width,
                    int logical_height)
{
    Letterbox view;
    view.logical_width = logical_width;
    view.logical_height = logical_height;
    if (output_width <= 0 || output_height <= 0 || logical_width <= 0
        || logical_height <= 0) {
        view.width = view.height = 0;
        return view;
    }
    // The float arithmetic is SDL's, operation for operation, so a viewport
    // edge that falls on a rounding boundary falls the same way.
    const float want_aspect = static_cast<float>(logical_width) / static_cast<float>(logical_height);
    const float real_aspect = static_cast<float>(output_width) / static_cast<float>(output_height);
    if (std::fabs(static_cast<double>(want_aspect - real_aspect)) < 0.0001) {
        // SDL_render.c:2338-2343: the same shape, scaled to fill.
        view.scale = static_cast<float>(output_width) / static_cast<float>(logical_width);
        view.width = output_width;
        view.height = output_height;
    } else if (want_aspect > real_aspect) {
        // SDL_render.c:2356-2361: wider than the output -- bars above and below.
        view.scale = static_cast<float>(output_width) / static_cast<float>(logical_width);
        view.width = output_width;
        view.height = static_cast<int>(
            std::floor(static_cast<double>(static_cast<float>(logical_height) * view.scale)));
        view.y = (output_height - view.height) / 2;
    } else {
        // SDL_render.c:2375-2380: narrower -- bars at the sides.
        view.scale = static_cast<float>(output_height) / static_cast<float>(logical_height);
        view.height = output_height;
        view.width = static_cast<int>(
            std::floor(static_cast<double>(static_cast<float>(logical_width) * view.scale)));
        view.x = (output_width - view.width) / 2;
    }
    return view;
}

IntPoint output_to_logical(const Letterbox& view, int x, int y)
{
    // SDL_render.c:769-772 (motion) and :796-799 (buttons): Sint32 minus the
    // viewport's origin, divided by the float scale (times a DPI scale that is
    // 1 wherever the window and its drawable are measured in the same pixels,
    // as on X11), then truncated by the int conversion.
    if (view.scale <= 0.0f)
        return {x, y};
    return {static_cast<int>(static_cast<float>(x - view.x) / view.scale),
            static_cast<int>(static_cast<float>(y - view.y) / view.scale)};
}

ScaleFilter scale_filter_from_hint(const char* hint)
{
    if (hint == nullptr || ::strcasecmp(hint, "nearest") == 0)
        return ScaleFilter::nearest;
    if (::strcasecmp(hint, "linear") == 0 || ::strcasecmp(hint, "best") == 0)
        return ScaleFilter::linear;
    return std::atoi(hint) == 0 ? ScaleFilter::nearest : ScaleFilter::linear;
}

namespace {

// The topmost layer's pixel at logical (x, y), which the first layer covers.
const std::uint8_t* fetch(std::span<const ScaleLayer> layers, int x, int y)
{
    for (std::size_t i = layers.size(); i-- > 1;) {
        const ScaleLayer& layer = layers[i];
        if (x >= layer.rect.left && x <= layer.rect.right && y >= layer.rect.top
            && y <= layer.rect.bottom)
            return layer.pixels + static_cast<std::ptrdiff_t>(y - layer.rect.top) * layer.stride
                + static_cast<std::ptrdiff_t>(x - layer.rect.left) * 4;
    }
    const ScaleLayer& base = layers.front();
    return base.pixels + static_cast<std::ptrdiff_t>(y - base.rect.top) * base.stride
        + static_cast<std::ptrdiff_t>(x - base.rect.left) * 4;
}

// floor(256 * ((d + 1/2) * logical / viewport - 1/2)), clamped at the near edge:
// where output pixel d's centre samples, in 1/256ths of a logical pixel.
std::int64_t linear_position(int d, int logical, int viewport)
{
    const std::int64_t at = (static_cast<std::int64_t>(2 * d + 1) * logical * 256)
            / (2 * static_cast<std::int64_t>(viewport))
        - 128;
    return std::max<std::int64_t>(at, 0);
}

} // namespace

void scale_layers(std::span<const ScaleLayer> layers, const Letterbox& view,
                  ScaleFilter filter, std::uint8_t* out, int out_stride, IntRect area)
{
    if (layers.empty() || area.empty())
        return;
    const int lw = view.logical_width;
    const int lh = view.logical_height;
    const IntRect inside {view.x, view.y, view.x + view.width - 1, view.y + view.height - 1};
    if (view.identity()) {
        // 1:1, where both filters take each texel exactly: copy spans, the
        // layers in order, and blacken what the viewport does not cover.
        for (int oy = area.top; oy <= area.bottom; ++oy) {
            std::uint8_t* row = out + static_cast<std::ptrdiff_t>(oy) * out_stride;
            for (int ox = area.left; ox <= area.right; ++ox) {
                if (oy > inside.bottom || ox > inside.right) {
                    std::uint8_t* pixel = row + static_cast<std::ptrdiff_t>(ox) * 4;
                    pixel[0] = pixel[1] = pixel[2] = 0;
                    pixel[3] = 0xff;
                }
            }
            if (oy > inside.bottom)
                continue;
            for (const ScaleLayer& layer : layers) {
                const IntRect span = intersect(
                    intersect(layer.rect, {area.left, oy, area.right, oy}), inside);
                if (span.empty())
                    continue;
                std::memcpy(row + static_cast<std::ptrdiff_t>(span.left) * 4,
                            layer.pixels
                                + static_cast<std::ptrdiff_t>(oy - layer.rect.top) * layer.stride
                                + static_cast<std::ptrdiff_t>(span.left - layer.rect.left) * 4,
                            static_cast<std::size_t>(span.right - span.left + 1) * 4);
            }
        }
        return;
    }
    for (int oy = area.top; oy <= area.bottom; ++oy) {
        std::uint8_t* row = out + static_cast<std::ptrdiff_t>(oy) * out_stride;
        const bool row_inside = oy >= inside.top && oy <= inside.bottom && !inside.empty();
        for (int ox = area.left; ox <= area.right; ++ox) {
            std::uint8_t* pixel = row + static_cast<std::ptrdiff_t>(ox) * 4;
            if (!row_inside || ox < inside.left || ox > inside.right) {
                pixel[0] = pixel[1] = pixel[2] = 0;
                pixel[3] = 0xff;
                continue;
            }
            const int dx = ox - view.x;
            const int dy = oy - view.y;
            if (filter == ScaleFilter::nearest) {
                const auto sx = static_cast<int>(
                    (static_cast<std::int64_t>(2 * dx + 1) * lw) / (2 * static_cast<std::int64_t>(view.width)));
                const auto sy = static_cast<int>(
                    (static_cast<std::int64_t>(2 * dy + 1) * lh) / (2 * static_cast<std::int64_t>(view.height)));
                std::memcpy(pixel, fetch(layers, sx, sy), 4);
                continue;
            }
            const std::int64_t px = linear_position(dx, lw, view.width);
            const std::int64_t py = linear_position(dy, lh, view.height);
            const int x0 = std::min(static_cast<int>(px >> 8), lw - 1);
            const int y0 = std::min(static_cast<int>(py >> 8), lh - 1);
            const int x1 = std::min(x0 + 1, lw - 1);
            const int y1 = std::min(y0 + 1, lh - 1);
            const std::uint32_t fx = static_cast<std::uint32_t>(px & 0xff);
            const std::uint32_t fy = static_cast<std::uint32_t>(py & 0xff);
            const std::uint32_t w00 = (256 - fx) * (256 - fy);
            const std::uint32_t w10 = fx * (256 - fy);
            const std::uint32_t w01 = (256 - fx) * fy;
            const std::uint32_t w11 = fx * fy;
            const std::uint8_t* c00 = fetch(layers, x0, y0);
            const std::uint8_t* c10 = fetch(layers, x1, y0);
            const std::uint8_t* c01 = fetch(layers, x0, y1);
            const std::uint8_t* c11 = fetch(layers, x1, y1);
            for (int c = 0; c < 4; ++c)
                pixel[c] = static_cast<std::uint8_t>(
                    (c00[c] * w00 + c10[c] * w10 + c01[c] * w01 + c11[c] * w11 + 32768) >> 16);
        }
    }
}

IntRect output_damage(const Letterbox& view, ScaleFilter filter, IntRect changed)
{
    const IntRect inside {view.x, view.y, view.x + view.width - 1, view.y + view.height - 1};
    if (changed.empty() || inside.empty() || view.logical_width <= 0
        || view.logical_height <= 0)
        return {};
    if (filter == ScaleFilter::linear) {
        // A logical pixel is also the neighbour its two neighbours blend with.
        changed.left -= 1;
        changed.top -= 1;
        changed.right += 1;
        changed.bottom += 1;
    }
    // Output pixel d samples near (d + 1/2) * logical / viewport, so logical
    // pixels l..r reach output pixels floor(l * v / L) - 1 .. ceil((r + 1) * v / L).
    const auto low = [](int l, int v, int big) {
        return static_cast<int>((static_cast<std::int64_t>(std::max(l, 0)) * v) / big) - 1;
    };
    const auto high = [](int r, int v, int big) {
        return static_cast<int>((static_cast<std::int64_t>(r + 1) * v + big - 1) / big);
    };
    const IntRect reach {
        view.x + low(changed.left, view.width, view.logical_width),
        view.y + low(changed.top, view.height, view.logical_height),
        view.x + high(changed.right, view.width, view.logical_width),
        view.y + high(changed.bottom, view.height, view.logical_height),
    };
    return intersect(reach, inside);
}

} // namespace haiku_remote
