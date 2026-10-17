#include "haiku_remote/surface.hpp"

#include "painter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace haiku_remote {
namespace {

std::size_t surface_buffer_size(int width, int height)
{
    if (width <= 0 || height <= 0
        || width > Surface::max_dimension || height > Surface::max_dimension) {
        throw std::invalid_argument(
            "surface dimensions are outside the supported range");
    }
    return static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height) * 4;
}

std::uint8_t brightness(Color color)
{
    return static_cast<std::uint8_t>(
        (308 * color.r + 600 * color.g + 116 * color.b) / 1024);
}

std::uint8_t blend_channel(std::uint8_t destination, std::uint8_t source,
                           std::uint32_t alpha)
{
    return static_cast<std::uint8_t>(
        ((static_cast<int>(source) - static_cast<int>(destination))
             * static_cast<int>(alpha)
         + (static_cast<int>(destination) << 8))
        >> 8);
}

} // namespace

Surface::Surface(int width, int height)
    : width_(width)
    , height_(height)
    , pixels_(surface_buffer_size(width, height))
{
    clear();
}

Color Surface::pixel(int x, int y) const
{
    if (x < 0 || x >= width_ || y < 0 || y >= height_)
        return {};
    const auto offset = static_cast<std::size_t>((y * width_ + x) * 4);
    return {pixels_[offset + 2], pixels_[offset + 1], pixels_[offset], pixels_[offset + 3]};
}

void Surface::set_pixel(int x, int y, Color color)
{
    if (x < 0 || x >= width_ || y < 0 || y >= height_)
        return;
    const auto offset = static_cast<std::size_t>((y * width_ + x) * 4);
    pixels_[offset] = color.b;
    pixels_[offset + 1] = color.g;
    pixels_[offset + 2] = color.r;
    pixels_[offset + 3] = color.a;
}

void Surface::clear(Color color)
{
    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x)
            set_pixel(x, y, color);
}

void Surface::resize(int width, int height)
{
    // Validated before anything is touched, so a refused size leaves the
    // surface exactly as it was.
    Surface next(width, height);
    // Screen coordinates are origin-anchored on both sides of a mode change
    // (Screen::Frame() is BRect(0, 0, w - 1, h - 1), Screen.cpp:228-234), so
    // the overlap keeps its coordinates: row y, column x of the old canvas is
    // row y, column x of the new one. Nothing is scaled or re-centred.
    const int rows = std::min(height_, height);
    const auto row_bytes = static_cast<std::size_t>(std::min(width_, width)) * 4;
    for (int y = 0; y < rows; ++y) {
        std::memcpy(next.pixels_.data() + static_cast<std::size_t>(y) * next.stride(),
                    pixels_.data() + static_cast<std::size_t>(y) * stride(),
                    row_bytes);
    }
    *this = std::move(next);
}

bool Surface::visible(int x, int y, const DrawState* state) const
{
    if (x < 0 || x >= width_ || y < 0 || y >= height_)
        return false;
    if (state == nullptr)
        return true;
    if (state->clip_rects.empty()) {
        // An empty clipping region clips everything away; only the absence of
        // any RP_CONSTRAIN_CLIPPING_REGION means "unclipped". See
        // DrawState::clipping_set.
        return !state->clipping_set;
    }
    for (const auto& rect : state->clip_rects) {
        const auto bounds = raster_bounds(rect);
        if (x >= bounds.left && x <= bounds.right
            && y >= bounds.top && y <= bounds.bottom)
            return true;
    }
    return false;
}

void Surface::composite(int x, int y, Color source, const DrawState& state,
                        bool high_selected, std::uint8_t coverage)
{
    if (!visible(x, y, &state))
        return;
    const Color destination = pixel(x, y);
    Color target = source;
    bool write = true;
    switch (state.drawing_mode) {
    case DrawingMode::copy:
        target.a = 255;
        break;
    case DrawingMode::over:
    case DrawingMode::alpha:
        break;
    case DrawingMode::erase:
        if (!high_selected) return;
        target = state.low;
        break;
    case DrawingMode::invert:
        if (!high_selected) return;
        target = {static_cast<std::uint8_t>(255 - destination.r),
                  static_cast<std::uint8_t>(255 - destination.g),
                  static_cast<std::uint8_t>(255 - destination.b), 255};
        break;
    case DrawingMode::add:
        target = {
            static_cast<std::uint8_t>(std::min(255, destination.r + source.r)),
            static_cast<std::uint8_t>(std::min(255, destination.g + source.g)),
            static_cast<std::uint8_t>(std::min(255, destination.b + source.b)), 255};
        break;
    case DrawingMode::subtract:
        target = {
            static_cast<std::uint8_t>(std::max(0, destination.r - source.r)),
            static_cast<std::uint8_t>(std::max(0, destination.g - source.g)),
            static_cast<std::uint8_t>(std::max(0, destination.b - source.b)), 255};
        break;
    case DrawingMode::blend:
        target = {static_cast<std::uint8_t>((destination.r + source.r) >> 1),
                  static_cast<std::uint8_t>((destination.g + source.g) >> 1),
                  static_cast<std::uint8_t>((destination.b + source.b) >> 1), 255};
        break;
    case DrawingMode::min:
        write = brightness(source) < brightness(destination);
        break;
    case DrawingMode::max:
        write = brightness(source) > brightness(destination);
        break;
    case DrawingMode::select:
        if (!high_selected) return;
        if (destination.r == state.high.r && destination.g == state.high.g
            && destination.b == state.high.b) {
            target = state.low;
        } else if (destination.r == state.low.r && destination.g == state.low.g
                   && destination.b == state.low.b) {
            target = state.high;
        } else {
            write = false;
        }
        break;
    }
    if (!write)
        return;

    std::uint32_t alpha = target.a;
    if (state.force_opaque || state.drawing_mode == DrawingMode::copy)
        alpha = 255;
    if (state.blend_modes_enabled && state.constant_alpha)
        alpha = state.high.a;
    alpha = (alpha * coverage) / 255;
    if (alpha == 255) {
        target.a = 255;
        set_pixel(x, y, target);
    } else {
        set_pixel(x, y, {
            blend_channel(destination.r, target.r, alpha),
            blend_channel(destination.g, target.g, alpha),
            blend_channel(destination.b, target.b, alpha),
            255,
        });
    }
}

// `x`/`y` are device pixels and are composited where they are told to be. They
// are deliberately *not* mapped through state.transform: coverage arrives from
// TextEngine::draw, which has already put the view transform into the glyph
// outline ahead of rasterisation, so mapping here would apply it twice.
//
// It used to map, and that was issue #21. Forward-mapping an already-rasterised
// glyph does not fill: under {sx=2, sy=2} each source pixel lands on one
// destination pixel and the gaps between them are never written, so a glyph
// became a lattice of dots with roughly the *same* ink count as the unscaled
// one. app_server never does this either -- a transform that is not
// translation-only makes it rasterise the transformed outline instead
// (StringRenderer::NeedsVector, AGGTextRenderer.cpp:146, selecting
// glyph_ren_outline at FontCacheEntry.cpp:429-435).
void Surface::paint_coverage(int x, int y, Color color, const DrawState& state,
                             std::uint8_t coverage, bool high_selected)
{
    composite(x, y, color, state, high_selected, coverage);
}

void Surface::paint_subpixel_coverage(
    int x, int y, Color color, const DrawState& state,
    std::uint8_t red_coverage, std::uint8_t green_coverage,
    std::uint8_t blue_coverage)
{
    // Device pixels, unmapped, for the reason spelled out above paint_coverage.
    if (state.drawing_mode != DrawingMode::copy
        && state.drawing_mode != DrawingMode::over
        && state.drawing_mode != DrawingMode::alpha) {
        const auto average = static_cast<std::uint8_t>(
            (static_cast<unsigned>(red_coverage)
             + static_cast<unsigned>(green_coverage)
             + static_cast<unsigned>(blue_coverage)) / 3);
        composite(x, y, color, state, true, average);
        return;
    }
    if (!visible(x, y, &state))
        return;

    const auto destination = pixel(x, y);
    std::uint32_t alpha = color.a;
    if (state.force_opaque || state.drawing_mode == DrawingMode::copy)
        alpha = 255;
    if (state.blend_modes_enabled && state.constant_alpha)
        alpha = state.high.a;
    const auto channel_alpha = [alpha](std::uint8_t coverage) {
        return (alpha * coverage) / 255;
    };
    set_pixel(x, y, {
        blend_channel(destination.r, color.r, channel_alpha(red_coverage)),
        blend_channel(destination.g, color.g, channel_alpha(green_coverage)),
        blend_channel(destination.b, color.b, channel_alpha(blue_coverage)),
        255,
    });
}

namespace {

const DrawState& default_state()
{
    static const DrawState state;
    return state;
}

} // namespace

painter::Canvas Surface::canvas()
{
    return {pixels_.data(), width_, height_};
}

void Surface::fill_rect(Rect rect, const DrawState& state, const Gradient* gradient)
{
    painter::fill_rect(canvas(), state, rect, gradient);
}

void Surface::fill_rect_color(Rect rect, Color color, const DrawState* state)
{
    painter::fill_rect_color(canvas(), state ? *state : default_state(), rect, color);
}

void Surface::fill_region(std::span<const Rect> rects, const DrawState& state,
                          const Gradient* gradient)
{
    painter::fill_region(canvas(), state, rects, gradient);
}

void Surface::stroke_rect(Rect rect, Color color, const DrawState* state)
{
    painter::stroke_rect_color(canvas(), state ? *state : default_state(), rect, color);
}

void Surface::stroke_rect(Rect rect, const DrawState& state, const Gradient* gradient)
{
    painter::stroke_rect(canvas(), state, rect, gradient);
}

void Surface::stroke_line(Point from, Point to, const DrawState& state,
                          const Gradient* gradient)
{
    painter::stroke_line(canvas(), state, from, to, gradient);
}

void Surface::stroke_line_color(Point from, Point to, Color color,
                                const DrawState& state)
{
    painter::stroke_line_color(canvas(), state, from, to, color);
}

void Surface::stroke_point_color(Point point, Color color, const DrawState& state)
{
    painter::stroke_point_color(canvas(), state, point, color);
}

void Surface::stroke_line_array(std::span<const ColoredLine> lines,
                                const DrawState& state)
{
    std::vector<painter::ArrayLine> converted;
    converted.reserve(lines.size());
    for (const auto& line : lines)
        converted.push_back({line.from, line.to, line.color});
    painter::stroke_line_array(canvas(), state, converted);
}

void Surface::fill_ellipse(Rect rect, const DrawState& state, const Gradient* gradient)
{
    painter::draw_ellipse(canvas(), state, rect, true, gradient);
}

void Surface::stroke_ellipse(Rect rect, const DrawState& state, const Gradient* gradient)
{
    painter::draw_ellipse(canvas(), state, rect, false, gradient);
}

void Surface::fill_arc(Rect rect, float angle, float span, const DrawState& state,
                       const Gradient* gradient)
{
    painter::draw_arc(canvas(), state, rect, angle, span, true, gradient);
}

void Surface::stroke_arc(Rect rect, float angle, float span, const DrawState& state,
                         const Gradient* gradient)
{
    painter::draw_arc(canvas(), state, rect, angle, span, false, gradient);
}

void Surface::fill_round_rect(Rect rect, float x_radius, float y_radius,
                              const DrawState& state, const Gradient* gradient)
{
    painter::draw_round_rect(canvas(), state, rect, x_radius, y_radius, true, gradient);
}

void Surface::stroke_round_rect(Rect rect, float x_radius, float y_radius,
                                const DrawState& state, const Gradient* gradient)
{
    painter::draw_round_rect(canvas(), state, rect, x_radius, y_radius, false, gradient);
}

void Surface::draw_polygon(std::span<const Point> points, Rect bounds, bool filled,
                           bool closed, const DrawState& state,
                           const Gradient* gradient)
{
    painter::draw_polygon(canvas(), state, points, bounds, filled, closed, gradient);
}

namespace {

Rect bounds_of(std::span<const Point> points)
{
    if (points.empty())
        return {};
    Rect bounds {points[0].x, points[0].y, points[0].x, points[0].y};
    for (const auto p : points) {
        bounds.left = std::min(bounds.left, p.x);
        bounds.top = std::min(bounds.top, p.y);
        bounds.right = std::max(bounds.right, p.x);
        bounds.bottom = std::max(bounds.bottom, p.y);
    }
    return bounds;
}

} // namespace

void Surface::fill_polygon(std::span<const Point> points, const DrawState& state)
{
    draw_polygon(points, bounds_of(points), true, true, state);
}

void Surface::stroke_polyline(std::span<const Point> points, const DrawState& state,
                              bool closed)
{
    draw_polygon(points, bounds_of(points), false, closed, state);
}

void Surface::draw_bezier(std::span<const Point, 4> points, bool filled,
                          const DrawState& state, const Gradient* gradient)
{
    painter::draw_bezier(canvas(), state, points, filled, gradient);
}

void Surface::draw_triangle(std::span<const Point, 3> points, Rect bounds,
                            bool filled, const DrawState& state,
                            const Gradient* gradient)
{
    painter::draw_triangle(canvas(), state, points, bounds, filled, gradient);
}

void Surface::draw_shape(std::span<const std::uint32_t> ops,
                         std::span<const Point> points, Point offset, float scale,
                         bool filled, const DrawState& state,
                         const Gradient* gradient)
{
    painter::draw_shape(canvas(), state, {ops, points, offset, scale}, filled, gradient);
}

void Surface::invert_rect(Rect rect, const DrawState* state)
{
    const auto b = intersect(raster_bounds(rect), {0, 0, width_ - 1, height_ - 1});
    for (int y = b.top; y <= b.bottom; ++y) {
        for (int x = b.left; x <= b.right; ++x) {
            if (!visible(x, y, state))
                continue;
            const auto c = pixel(x, y);
            set_pixel(x, y, {static_cast<std::uint8_t>(255 - c.r),
                             static_cast<std::uint8_t>(255 - c.g),
                             static_cast<std::uint8_t>(255 - c.b), 255});
        }
    }
}

void Surface::copy_rect(Rect source, int dx, int dy)
{
    const auto b = intersect(raster_bounds(source), {0, 0, width_ - 1, height_ - 1});
    if (b.empty())
        return;
    const int copy_width = b.right - b.left + 1;
    const int copy_height = b.bottom - b.top + 1;
    std::vector<std::uint8_t> copy(static_cast<std::size_t>(copy_width * copy_height * 4));
    for (int y = 0; y < copy_height; ++y) {
        const auto source_offset = static_cast<std::size_t>(((b.top + y) * width_ + b.left) * 4);
        std::memcpy(copy.data() + static_cast<std::size_t>(y * copy_width * 4),
                    pixels_.data() + source_offset,
                    static_cast<std::size_t>(copy_width * 4));
    }
    // `dx`/`dy` are raw int32s off the wire, so `b.left + dx + x` overflowed a
    // signed int for extreme offsets -- undefined behaviour ahead of the range
    // check that is supposed to reject them. 64-bit arithmetic keeps the
    // comparison meaningful.
    for (int y = 0; y < copy_height; ++y) {
        for (int x = 0; x < copy_width; ++x) {
            const long long destination_x
                = static_cast<long long>(b.left) + dx + x;
            const long long destination_y
                = static_cast<long long>(b.top) + dy + y;
            if (destination_x < 0 || destination_x >= width_
                || destination_y < 0 || destination_y >= height_)
                continue;
            const auto source_offset = static_cast<std::size_t>((y * copy_width + x) * 4);
            set_pixel(static_cast<int>(destination_x),
                      static_cast<int>(destination_y),
                      {copy[source_offset + 2], copy[source_offset + 1],
                       copy[source_offset], copy[source_offset + 3]});
        }
    }
}

void Surface::draw_bitmap(const Bitmap& bitmap, Rect source, Rect destination,
                          const DrawState& state, std::uint32_t options)
{
    // DrawingEngine::DrawBitmap through app_server's own BitmapPainter: see
    // painter.cpp, which follows each of its code paths and the way each one
    // rounds a fractional rect -- B_SUBPIXEL_PRECISE included.
    if (bitmap.width <= 0 || bitmap.height <= 0
        || bitmap.bgra.size() < static_cast<std::size_t>(bitmap.width)
                * static_cast<std::size_t>(bitmap.height) * 4) {
        return;
    }
    painter::draw_bitmap(canvas(), state, {bitmap.bgra.data(), bitmap.width, bitmap.height},
                         source, destination, options);
}

} // namespace haiku_remote
