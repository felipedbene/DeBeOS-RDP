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

Color bitmap_pixel(const Bitmap& bitmap, int x, int y)
{
    const auto offset = static_cast<std::size_t>((y * bitmap.width + x) * 4);
    return {bitmap.bgra[offset + 2], bitmap.bgra[offset + 1],
            bitmap.bgra[offset], bitmap.bgra[offset + 3]};
}

// Tiling wraps with a *positive* remainder: the server tiles through
// agg::image_accessor_wrap<..., wrap_mode_repeat, wrap_mode_repeat>, and
// wrap_mode_repeat biases by a large multiple of the period before taking the
// remainder, so a source coordinate to the left of the bitmap lands in the
// right-hand tile rather than at a negative index (DrawBitmapGeneric.h:20-30).
int wrap_index(long long value, int period)
{
    const long long wrapped = value % period;
    return static_cast<int>(wrapped < 0 ? wrapped + period : wrapped);
}

// Index of, and weight given to, the left (or top) source pixel for one
// destination column (or row) of a scaled bilinear draw:
//
//     index  = i * (sourcePixels - 1) / (sourcePixels * scale - 1)
//     weight = 255 - (uint16)(frac(index) * 255)
//
// verbatim from DrawBitmapBilinear.h:543-578, `float` included: the server holds
// the index in a float and takes the fraction there, and rounding it in double
// instead moves a weight by one on indices that are not exact binary fractions.
//
// Note that the mapping is *corner*-aligned -- destination pixel 0 lands exactly
// on source pixel 0 and the last destination pixel exactly on the last source
// pixel -- rather than pixel-centre aligned. That is the server's convention, and
// matching it is the point.
struct FilterWeight {
    int index = 0;
    unsigned weight = 255;
};

FilterWeight filter_weight(double i, int source_pixels, double scale)
{
    const double denominator = source_pixels * scale - 1.0;
    // The server divides unguarded: a one-pixel source, or a one-pixel
    // destination, is 0/0 there. Both mean "the first source pixel at full
    // weight", which is what its integer conversion of the resulting NaN cannot
    // be relied on to produce.
    if (source_pixels <= 1 || !(denominator > 1e-9))
        return {0, 255};
    // NaN reaches here from an unvalidated wire rect, and std::clamp does not
    // filter it out -- every comparison against NaN is false -- so it is folded
    // explicitly before a conversion that would otherwise be undefined. The
    // bounds keep both casts below in range.
    double ratio = i * (source_pixels - 1) / denominator;
    if (std::isnan(ratio))
        ratio = 0;
    const auto index = static_cast<float>(std::clamp(ratio, 0.0, 1.0e9));
    const float floored = std::floor(index);
    const auto fractional = static_cast<unsigned>((index - floored) * 255.0f);
    return {static_cast<int>(floored), 255 - fractional};
}

// The scale-only bilinear sampler, bit for bit. Weights are 255-based but the
// accumulator is shifted by 16, and 255 * 255 is 65025, not 65536 -- so a
// uniformly white bitmap magnifies to 253, not 255. The server has always done
// that (DrawBitmapBilinear.h:104-124) and fidelity to app_server is the
// product, so it is reproduced rather than corrected.
//
// The server's loop peels three exact cases off the edges of the destination
// rect, each of which the full path would darken: the last column interpolates
// vertically only with a >> 8, the last row horizontally only with a >> 8, and
// the bottom-right corner is copied verbatim (ibid. :230-325). `single_row`
// covers its `fSource->height() > 1` guard, which makes a one-row bitmap take
// the horizontal-only path everywhere.
Color sample_bilinear_scaled(const Bitmap& bitmap, FilterWeight wx,
                             FilterWeight wy, bool last_column, bool last_row)
{
    const int x0 = std::clamp(wx.index, 0, bitmap.width - 1);
    const int y0 = std::clamp(wy.index, 0, bitmap.height - 1);
    const int x1 = std::min(x0 + 1, bitmap.width - 1);
    const int y1 = std::min(y0 + 1, bitmap.height - 1);
    const bool single_row = bitmap.height == 1;
    const bool column_hit = last_column && wx.weight == 255;
    const bool row_hit = last_row && wy.weight == 255;

    const auto top_left = bitmap_pixel(bitmap, x0, y0);
    if (column_hit && row_hit)
        return top_left;

    const unsigned left = wx.weight;
    const unsigned right = 255 - left;
    const unsigned top = wy.weight;
    const unsigned bottom = 255 - top;

    if (column_hit && !single_row) {
        const auto below = bitmap_pixel(bitmap, x0, y1);
        const auto mix = [&](std::uint8_t a, std::uint8_t b) {
            return static_cast<std::uint8_t>((a * top + b * bottom) >> 8);
        };
        return {mix(top_left.r, below.r), mix(top_left.g, below.g),
                mix(top_left.b, below.b), mix(top_left.a, below.a)};
    }
    const auto top_right = bitmap_pixel(bitmap, x1, y0);
    if (row_hit || single_row) {
        const auto mix = [&](std::uint8_t a, std::uint8_t b) {
            return static_cast<std::uint8_t>((a * left + b * right) >> 8);
        };
        return {mix(top_left.r, top_right.r), mix(top_left.g, top_right.g),
                mix(top_left.b, top_right.b), mix(top_left.a, top_right.a)};
    }
    const auto bottom_left = bitmap_pixel(bitmap, x0, y1);
    const auto bottom_right = bitmap_pixel(bitmap, x1, y1);
    const auto mix = [&](std::uint8_t a, std::uint8_t b, std::uint8_t c,
                         std::uint8_t d) {
        return static_cast<std::uint8_t>(
            ((a * left + b * right) * top + (c * left + d * right) * bottom)
            >> 16);
    };
    return {mix(top_left.r, top_right.r, bottom_left.r, bottom_right.r),
            mix(top_left.g, top_right.g, bottom_left.g, bottom_right.g),
            mix(top_left.b, top_right.b, bottom_left.b, bottom_right.b),
            mix(top_left.a, top_right.a, bottom_left.a, bottom_right.a)};
}

// The bilinear sampler for the tiled and transformed cases, which the server
// routes through AGG instead: agg::span_image_filter_rgba_bilinear over either
// an image_accessor_wrap (tiled) or an image_accessor_clone (edge clamp)
// (DrawBitmapGeneric.h:20-30, :95-110). It differs from the scale-only path
// above in two ways that are visible in the output, so they are kept separate
// rather than unified: the sample point is the destination pixel *centre*
// shifted back half a source pixel, and the 1/256 weights sum to 65536 exactly,
// so this path does not darken.
//
// `fx`/`fy` are already that shifted source coordinate. They derive from rects
// that arrived off the wire, so NaN reaches here; converting one to an integer is
// undefined, and clamping does not filter it out (every comparison against NaN is
// false), so it is folded to zero first.
Color sample_bilinear_generic(const Bitmap& bitmap, double fx, double fy,
                              bool wrap)
{
    if (std::isnan(fx))
        fx = 0;
    if (std::isnan(fy))
        fy = 0;
    const double floor_x = std::floor(std::clamp(fx, -1.0e9, 1.0e9));
    const double floor_y = std::floor(std::clamp(fy, -1.0e9, 1.0e9));
    const auto x_hr = static_cast<unsigned>(
        std::clamp((fx - floor_x) * 256.0, 0.0, 255.0));
    const auto y_hr = static_cast<unsigned>(
        std::clamp((fy - floor_y) * 256.0, 0.0, 255.0));
    const auto x0 = static_cast<long long>(floor_x);
    const auto y0 = static_cast<long long>(floor_y);
    const auto at = [&](long long x, long long y) {
        if (wrap) {
            return bitmap_pixel(bitmap, wrap_index(x, bitmap.width),
                                wrap_index(y, bitmap.height));
        }
        return bitmap_pixel(
            bitmap,
            static_cast<int>(std::clamp<long long>(x, 0, bitmap.width - 1)),
            static_cast<int>(std::clamp<long long>(y, 0, bitmap.height - 1)));
    };
    const auto top_left = at(x0, y0);
    const auto top_right = at(x0 + 1, y0);
    const auto bottom_left = at(x0, y0 + 1);
    const auto bottom_right = at(x0 + 1, y0 + 1);
    const unsigned w00 = (256 - x_hr) * (256 - y_hr);
    const unsigned w10 = x_hr * (256 - y_hr);
    const unsigned w01 = (256 - x_hr) * y_hr;
    const unsigned w11 = x_hr * y_hr;
    const auto mix = [&](std::uint8_t a, std::uint8_t b, std::uint8_t c,
                         std::uint8_t d) {
        return static_cast<std::uint8_t>(
            (a * w00 + b * w10 + c * w01 + d * w11) >> 16);
    };
    return {mix(top_left.r, top_right.r, bottom_left.r, bottom_right.r),
            mix(top_left.g, top_right.g, bottom_left.g, bottom_right.g),
            mix(top_left.b, top_right.b, bottom_left.b, bottom_right.b),
            mix(top_left.a, top_right.a, bottom_left.a, bottom_right.a)};
}

} // namespace

// One implementation of the server's subpixel spans: the Painter port's
// (painter::blend_text_subpix -> PixelFormat::blend_solid_hspan_subpix), the
// span solid shapes use too. This is the pure, one-pixel view of it the text
// tests check, run on a 1x1 canvas holding `d`.
std::optional<Color> blend_subpixel_text(Color d, const DrawState& state,
                                         std::uint8_t cover_left,
                                         std::uint8_t cover_middle,
                                         std::uint8_t cover_right,
                                         bool ordering_rgb)
{
    std::uint8_t pixel[4] = {d.b, d.g, d.r, d.a};
    const std::uint8_t covers[3] = {cover_left, cover_middle, cover_right};
    if (!painter::blend_text_subpix({pixel, 1, 1}, state, 0, 0, state.high, covers,
                                    ordering_rgb))
        return std::nullopt;
    return Color {pixel[2], pixel[1], pixel[0], pixel[3]};
}

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
    if (!visible(x, y, &state))
        return;
    const std::uint8_t covers[3] = {red_coverage, green_coverage, blue_coverage};
    painter::blend_text_subpix(canvas(), state, x, y, color, covers);
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
    if (bitmap.width <= 0 || bitmap.height <= 0)
        return;
    // Either tiling bit tiles in *both* axes: the server's only tiled accessor
    // is agg::image_accessor_wrap<..., wrap_mode_repeat, wrap_mode_repeat>,
    // selected by `(fOptions & B_TILE_BITMAP) != 0`, so B_TILE_BITMAP_X alone
    // still repeats vertically (BitmapPainter.cpp:166-169,
    // DrawBitmapGeneric.h:26-30). A quirk, matched deliberately.
    const bool tiled = (options & tile_bitmap) != 0;
    const bool bilinear = (options & filter_bitmap_bilinear) != 0;
    IntRect dst;
    if (state.transform.is_identity()) {
        dst = raster_bounds(destination);
    } else {
        const std::array<Point, 4> corners {{
            state.map_point({destination.left, destination.top}),
            state.map_point({destination.right, destination.top}),
            state.map_point({destination.right, destination.bottom}),
            state.map_point({destination.left, destination.bottom}),
        }};
        float left = corners.front().x;
        float top = corners.front().y;
        float right = corners.front().x;
        float bottom = corners.front().y;
        for (const auto point : corners) {
            left = std::min(left, point.x);
            top = std::min(top, point.y);
            right = std::max(right, point.x);
            bottom = std::max(bottom, point.y);
        }
        dst = raster_bounds({left, top, right, bottom});
    }
    dst = intersect(dst, {0, 0, width_ - 1, height_ - 1});
    const double source_width = std::max(1.0f, source.width());
    const double source_height = std::max(1.0f, source.height());
    const double destination_width = std::max(1.0f, destination.width());
    const double destination_height = std::max(1.0f, destination.height());
    // Scales for the non-tiled paths. A tiled draw does not scale at all: the
    // server pins scaleX/scaleY to 1 for it and keeps the source rect only as
    // the tile phase (BitmapPainter.cpp:196-227, _DetermineTransform), so a
    // small bitmap tiled across a large rect repeats at its own size instead of
    // being stretched to fit -- which is what this client did for as long as the
    // options word was discarded.
    const double scale_x = destination_width / source_width;
    const double scale_y = destination_height / source_height;
    // Which destination column and row take the server's exact edge cases. Both
    // are computed through raster_coordinate() because the rect arrived off the
    // wire and may be NaN or absurd.
    const int last_column = raster_coordinate(std::floor(destination.right), 0);
    const int last_row = raster_coordinate(std::floor(destination.bottom), 0);
    // The scale-only bilinear path applies a constant integer shift for a
    // cropped source, the way the server folds `destinationRect.left -
    // offset.x` into every weight (DrawBitmapBilinear.h:540-541).
    const int source_shift_x = raster_coordinate(source.left, 0);
    const int source_shift_y = raster_coordinate(source.top, 0);
    for (int y = dst.top; y <= dst.bottom; ++y) {
        for (int x = dst.left; x <= dst.right; ++x) {
            const auto local = state.unmap_point(
                {static_cast<float>(x), static_cast<float>(y)});
            if (local.x < destination.left || local.x > destination.right
                || local.y < destination.top || local.y > destination.bottom) {
                continue;
            }
            const double offset_x = local.x - destination.left;
            const double offset_y = local.y - destination.top;
            Color color;
            if (tiled) {
                // Scale 1, phase from the source rect's origin: the source
                // coordinate is `destination_x - offset.x` where the server's
                // offset.x is `destinationRect.left - sourceRect.left`
                // (BitmapPainter.cpp:228-229).
                const double sx = offset_x + source.left;
                const double sy = offset_y + source.top;
                color = bilinear
                    ? sample_bilinear_generic(bitmap, sx, sy, true)
                    : bitmap_pixel(
                        bitmap,
                        wrap_index(raster_coordinate(std::floor(sx), 0),
                                   bitmap.width),
                        wrap_index(raster_coordinate(std::floor(sy), 0),
                                   bitmap.height));
            } else if (bilinear && state.transform.is_identity()) {
                auto wx = filter_weight(offset_x, bitmap.width, scale_x);
                auto wy = filter_weight(offset_y, bitmap.height, scale_y);
                wx.index += source_shift_x;
                wy.index += source_shift_y;
                color = sample_bilinear_scaled(bitmap, wx, wy,
                                               x == last_column,
                                               y == last_row);
            } else if (bilinear) {
                // Under a transform the server samples through AGG instead, at
                // the destination pixel centre less half a source pixel.
                color = sample_bilinear_generic(
                    bitmap,
                    source.left
                        + (offset_x + 0.5) * source_width / destination_width
                        - 0.5,
                    source.top
                        + (offset_y + 0.5) * source_height / destination_height
                        - 0.5,
                    false);
            } else {
                // raster_coordinate() rather than a bare cast: these rects are
                // unvalidated wire floats, and converting a NaN or out-of-range
                // one to int is undefined.
                const int sx = std::clamp(
                    raster_coordinate(source.left
                        + offset_x * source_width / destination_width, 0),
                    0, bitmap.width - 1);
                const int sy = std::clamp(
                    raster_coordinate(source.top
                        + offset_y * source_height / destination_height, 0),
                    0, bitmap.height - 1);
                color = bitmap_pixel(bitmap, sx, sy);
            }
            composite(x, y, color, state, true);
        }
    }
}

} // namespace haiku_remote
