#include "haiku_remote/surface.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

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

std::vector<Color> gradient_lut(const Gradient& gradient, bool force_opaque)
{
    constexpr int size = 256;
    const auto fix = [force_opaque](Color color) {
        if (force_opaque)
            color.a = 255;
        return color;
    };
    if (gradient.stops.empty())
        return std::vector<Color>(size, {0, 0, 0, static_cast<std::uint8_t>(
                                                   force_opaque ? 255 : 0)});
    std::vector<Color> result(size, fix(gradient.stops.front().color));
    auto from = gradient.stops.front();
    int index = static_cast<int>(std::floor(
        size * static_cast<double>(from.offset) / 255.0 + 0.5));
    index = std::clamp(index, 0, size);
    for (std::size_t stop = 1; stop < gradient.stops.size(); ++stop) {
        const auto to = gradient.stops[stop];
        int offset = static_cast<int>(std::floor(
            (size - 1) * static_cast<double>(to.offset) / 255.0 + 0.5));
        offset = std::min(offset, size - 1);
        const int distance = offset - index;
        if (distance >= 0) {
            for (int i = std::max(index, 0); i <= offset; ++i) {
                const double f = static_cast<double>(offset - i) / (distance + 1);
                const double t = 1.0 - f;
                result[static_cast<std::size_t>(i)] = fix({
                    static_cast<std::uint8_t>(std::floor(from.color.r * f
                                                        + to.color.r * t + 0.5)),
                    static_cast<std::uint8_t>(std::floor(from.color.g * f
                                                        + to.color.g * t + 0.5)),
                    static_cast<std::uint8_t>(std::floor(from.color.b * f
                                                        + to.color.b * t + 0.5)),
                    static_cast<std::uint8_t>(std::floor(from.color.a * f
                                                        + to.color.a * t + 0.5)),
                });
            }
        }
        index = offset + 1;
        from = to;
    }
    for (int i = std::max(index, 0); i < size; ++i)
        result[static_cast<std::size_t>(i)] = fix(from.color);
    return result;
}

Color gradient_color(const Gradient& gradient, const std::vector<Color>& lut,
                     int x, int y, const DrawState& state)
{
    const auto local = state.unmap_point(
        {static_cast<float>(x + 0.5), static_cast<float>(y + 0.5)});
    const double px = local.x;
    const double py = local.y;
    double parameter = 0;
    switch (gradient.kind) {
    case Gradient::Kind::linear: {
        const double dx = gradient.end.x - gradient.start.x;
        const double dy = gradient.end.y - gradient.start.y;
        const double length_squared = dx * dx + dy * dy;
        if (length_squared > 0) {
            parameter = ((px - gradient.start.x) * dx
                         + (py - gradient.start.y) * dy)
                / length_squared;
        }
        break;
    }
    case Gradient::Kind::radial:
    case Gradient::Kind::radial_focus: {
        const double dx = px - gradient.center.x;
        const double dy = py - gradient.center.y;
        if (gradient.radius > 0)
            parameter = std::sqrt(dx * dx + dy * dy) / gradient.radius;
        break;
    }
    case Gradient::Kind::diamond:
        parameter = std::max(std::abs(px - gradient.center.x),
                             std::abs(py - gradient.center.y))
            / 100.0;
        break;
    case Gradient::Kind::conic:
        parameter = std::abs(std::atan2(py - gradient.center.y,
                                       px - gradient.center.x))
            / std::numbers::pi;
        break;
    case Gradient::Kind::none:
        break;
    }
    const int index = std::clamp(
        static_cast<int>(parameter * static_cast<double>(lut.size())),
        0, static_cast<int>(lut.size()) - 1);
    return lut[static_cast<std::size_t>(index)];
}

template<typename Paint>
void rasterize_polygon(std::span<const Point> points, int width, int height,
                       Paint paint)
{
    if (points.size() < 3)
        return;
    float min_x = points.front().x;
    float max_x = points.front().x;
    float min_y = points.front().y;
    float max_y = points.front().y;
    for (const auto point : points) {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }
    const auto bounds = intersect(
        raster_bounds({min_x, min_y, max_x, max_y}),
        {0, 0, width - 1, height - 1});
    for (int y = bounds.top; y <= bounds.bottom; ++y) {
        for (int x = bounds.left; x <= bounds.right; ++x) {
            const double px = x + 0.5;
            const double py = y + 0.5;
            bool inside = false;
            for (std::size_t i = 0, j = points.size() - 1;
                 i < points.size(); j = i++) {
                const auto a = points[i];
                const auto b = points[j];
                const bool crosses = ((a.y > py) != (b.y > py))
                    && (px < (b.x - a.x) * (py - a.y) / (b.y - a.y) + a.x);
                if (crosses)
                    inside = !inside;
            }
            if (inside)
                paint(x, y);
        }
    }
}

template<typename Paint>
void rasterize_stroke(Point from, Point to, const DrawState* state, Paint paint)
{
    const double pen_size = state == nullptr
        ? 1.0 : std::max(0.0f, state->pen_size);
    if (pen_size <= 1.0) {
        int x0 = static_cast<int>(std::lround(from.x));
        int y0 = static_cast<int>(std::lround(from.y));
        const int x1 = static_cast<int>(std::lround(to.x));
        const int y1 = static_cast<int>(std::lround(to.y));
        const int dx = std::abs(x1 - x0);
        const int sx = x0 < x1 ? 1 : -1;
        const int dy = -std::abs(y1 - y0);
        const int sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        while (true) {
            paint(x0, y0);
            if (x0 == x1 && y0 == y1)
                break;
            const int twice = 2 * error;
            if (twice >= dy) {
                error += dy;
                x0 += sx;
            }
            if (twice <= dx) {
                error += dx;
                y0 += sy;
            }
        }
        return;
    }

    const double radius = pen_size / 2.0;
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double length_squared = dx * dx + dy * dy;
    const auto cap = state == nullptr ? 0u : state->line_cap;
    const int left = static_cast<int>(
        std::floor(std::min(from.x, to.x) - radius - 1));
    const int top = static_cast<int>(
        std::floor(std::min(from.y, to.y) - radius - 1));
    const int right = static_cast<int>(
        std::ceil(std::max(from.x, to.x) + radius + 1));
    const int bottom = static_cast<int>(
        std::ceil(std::max(from.y, to.y) + radius + 1));
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const double px = x + 0.5;
            const double py = y + 0.5;
            double t = length_squared > 0
                ? ((px - from.x) * dx + (py - from.y) * dy) / length_squared
                : 0;
            if (cap == 2 && length_squared > 0) {
                const double extension = radius / std::sqrt(length_squared);
                if (t < -extension || t > 1 + extension)
                    continue;
            } else if (cap == 0 && (t < 0 || t > 1)) {
                continue;
            }
            if (cap == 1)
                t = std::clamp(t, 0.0, 1.0);
            const double nearest_x = from.x + t * dx;
            const double nearest_y = from.y + t * dy;
            const double distance_x = px - nearest_x;
            const double distance_y = py - nearest_y;
            if (distance_x * distance_x + distance_y * distance_y
                <= radius * radius) {
                paint(x, y);
            }
        }
    }
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

bool Surface::visible(int x, int y, const DrawState* state) const
{
    if (x < 0 || x >= width_ || y < 0 || y >= height_)
        return false;
    if (state == nullptr || state->clip_rects.empty())
        return true;
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

void Surface::paint_coverage(int x, int y, Color color, const DrawState& state,
                             std::uint8_t coverage, bool high_selected)
{
    if (!state.transform.is_identity()) {
        const auto mapped = state.map_point(
            {static_cast<float>(x), static_cast<float>(y)});
        x = static_cast<int>(std::lround(mapped.x));
        y = static_cast<int>(std::lround(mapped.y));
    }
    composite(x, y, color, state, high_selected, coverage);
}

void Surface::paint_subpixel_coverage(
    int x, int y, Color color, const DrawState& state,
    std::uint8_t red_coverage, std::uint8_t green_coverage,
    std::uint8_t blue_coverage)
{
    if (!state.transform.is_identity()) {
        const auto mapped = state.map_point(
            {static_cast<float>(x), static_cast<float>(y)});
        x = static_cast<int>(std::lround(mapped.x));
        y = static_cast<int>(std::lround(mapped.y));
    }
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

void Surface::fill_rect(Rect rect, const DrawState& state)
{
    if (!state.transform.is_identity()) {
        const std::array<Point, 4> points {{
            {rect.left, rect.top}, {rect.right + 1, rect.top},
            {rect.right + 1, rect.bottom + 1}, {rect.left, rect.bottom + 1},
        }};
        fill_polygon(points, state);
        return;
    }
    const auto bounds = intersect(raster_bounds(rect), {0, 0, width_ - 1, height_ - 1});
    for (int y = bounds.top; y <= bounds.bottom; ++y) {
        for (int x = bounds.left; x <= bounds.right; ++x) {
            const bool high = state.pattern_is_high(x, y);
            composite(x, y, high ? state.high : state.low, state, high);
        }
    }
}

void Surface::fill_rect_color(Rect rect, Color color, const DrawState* state)
{
    if (state != nullptr && !state->transform.is_identity()) {
        DrawState solid = *state;
        solid.high = color;
        solid.low = color;
        solid.pattern.fill(0xff);
        const std::array<Point, 4> points {{
            {rect.left, rect.top}, {rect.right + 1, rect.top},
            {rect.right + 1, rect.bottom + 1}, {rect.left, rect.bottom + 1},
        }};
        fill_polygon(points, solid);
        return;
    }
    const auto bounds = intersect(raster_bounds(rect), {0, 0, width_ - 1, height_ - 1});
    for (int y = bounds.top; y <= bounds.bottom; ++y) {
        for (int x = bounds.left; x <= bounds.right; ++x) {
            if (!visible(x, y, state))
                continue;
            if (state == nullptr) {
                color.a = 255;
                set_pixel(x, y, color);
            } else {
                composite(x, y, color, *state, true);
            }
        }
    }
}

void Surface::stroke_rect(Rect rect, Color color, const DrawState* state)
{
    DrawState one_pixel;
    const DrawState* stroke_state = state;
    if (state != nullptr) {
        one_pixel = *state;
        one_pixel.pen_size = 1;
        stroke_state = &one_pixel;
    }
    const auto b = raster_bounds(rect);
    line({static_cast<float>(b.left), static_cast<float>(b.top)},
         {static_cast<float>(b.right), static_cast<float>(b.top)},
         color, stroke_state);
    line({static_cast<float>(b.right), static_cast<float>(b.top)},
         {static_cast<float>(b.right), static_cast<float>(b.bottom)},
         color, stroke_state);
    line({static_cast<float>(b.right), static_cast<float>(b.bottom)},
         {static_cast<float>(b.left), static_cast<float>(b.bottom)},
         color, stroke_state);
    line({static_cast<float>(b.left), static_cast<float>(b.bottom)},
         {static_cast<float>(b.left), static_cast<float>(b.top)},
         color, stroke_state);
}

void Surface::line(Point from, Point to, Color color, const DrawState* state,
                   bool use_pattern)
{
    if (state != nullptr) {
        from = state->map_point(from);
        to = state->map_point(to);
    }
    rasterize_stroke(from, to, state, [&](int x, int y) {
        if (!visible(x, y, state))
            return;
        if (state == nullptr) {
            set_pixel(x, y, color);
        } else if (use_pattern) {
            const bool high = state->pattern_is_high(x, y);
            composite(x, y, high ? state->high : state->low, *state, high);
        } else {
            composite(x, y, color, *state, true);
        }
    });
}

void Surface::fill_ellipse(Rect rect, const DrawState& state)
{
    if (!state.transform.is_identity()) {
        constexpr int segments = 180;
        const double cx = (rect.left + rect.right + 1.0) / 2.0;
        const double cy = (rect.top + rect.bottom + 1.0) / 2.0;
        const double rx = std::max(0.5, static_cast<double>(rect.width()) / 2.0);
        const double ry = std::max(0.5, static_cast<double>(rect.height()) / 2.0);
        std::vector<Point> points;
        points.reserve(segments);
        for (int i = 0; i < segments; ++i) {
            const double angle = 2 * std::numbers::pi * i / segments;
            points.push_back({
                static_cast<float>(cx + std::cos(angle) * rx),
                static_cast<float>(cy + std::sin(angle) * ry),
            });
        }
        fill_polygon(points, state);
        return;
    }
    const auto b = intersect(raster_bounds(rect), {0, 0, width_ - 1, height_ - 1});
    const double cx = (rect.left + rect.right + 1.0) / 2.0;
    const double cy = (rect.top + rect.bottom + 1.0) / 2.0;
    const double rx = std::max(0.5, rect.width() / 2.0);
    const double ry = std::max(0.5, rect.height() / 2.0);
    for (int y = b.top; y <= b.bottom; ++y) {
        for (int x = b.left; x <= b.right; ++x) {
            const double nx = (x + 0.5 - cx) / rx;
            const double ny = (y + 0.5 - cy) / ry;
            if (nx * nx + ny * ny <= 1.0) {
                const bool high = state.pattern_is_high(x, y);
                composite(x, y, high ? state.high : state.low, state, high);
            }
        }
    }
}

void Surface::stroke_ellipse(Rect rect, const DrawState& state)
{
    const double cx = (rect.left + rect.right + 1.0) / 2.0;
    const double cy = (rect.top + rect.bottom + 1.0) / 2.0;
    const double rx = std::max(0.5, rect.width() / 2.0);
    const double ry = std::max(0.5, rect.height() / 2.0);
    constexpr int segments = 360;
    Point previous {static_cast<float>(cx + rx), static_cast<float>(cy)};
    for (int i = 1; i <= segments; ++i) {
        const double angle = 2.0 * 3.14159265358979323846 * i / segments;
        Point next {static_cast<float>(cx + std::cos(angle) * rx),
                    static_cast<float>(cy + std::sin(angle) * ry)};
        line(previous, next, state.source_at(static_cast<int>(next.x),
                                             static_cast<int>(next.y)),
             &state, true);
        previous = next;
    }
}

void Surface::fill_polygon(std::span<const Point> points, const DrawState& state)
{
    std::vector<Point> transformed;
    if (!state.transform.is_identity()) {
        transformed.reserve(points.size());
        for (const auto point : points)
            transformed.push_back(state.map_point(point));
        points = transformed;
    }
    rasterize_polygon(points, width_, height_, [&](int x, int y) {
        const bool high = state.pattern_is_high(x, y);
        composite(x, y, high ? state.high : state.low, state, high);
    });
}

void Surface::stroke_polyline(std::span<const Point> points, const DrawState& state,
                              bool closed)
{
    if (points.size() < 2)
        return;
    for (std::size_t i = 1; i < points.size(); ++i)
        line(points[i - 1], points[i], state.high, &state, true);
    if (closed)
        line(points.back(), points.front(), state.high, &state, true);
}

void Surface::fill_gradient_polygon(std::span<const Point> points,
                                    const Gradient& gradient,
                                    const DrawState& state)
{
    const auto lut = gradient_lut(gradient, state.force_opaque);
    std::vector<Point> transformed;
    if (!state.transform.is_identity()) {
        transformed.reserve(points.size());
        for (const auto point : points)
            transformed.push_back(state.map_point(point));
        points = transformed;
    }
    rasterize_polygon(points, width_, height_, [&](int x, int y) {
        composite(x, y, gradient_color(gradient, lut, x, y, state), state, true);
    });
}

void Surface::stroke_gradient_polyline(std::span<const Point> points,
                                       const Gradient& gradient,
                                       const DrawState& state,
                                       bool closed)
{
    if (points.size() < 2)
        return;
    const auto lut = gradient_lut(gradient, state.force_opaque);
    const auto draw_segment = [&](Point from, Point to) {
        from = state.map_point(from);
        to = state.map_point(to);
        rasterize_stroke(from, to, &state, [&](int x, int y) {
            composite(x, y, gradient_color(gradient, lut, x, y, state),
                      state, true);
        });
    };
    for (std::size_t i = 1; i < points.size(); ++i)
        draw_segment(points[i - 1], points[i]);
    if (closed)
        draw_segment(points.back(), points.front());
}

void Surface::fill_arc(Rect rect, float angle, float span, const DrawState& state)
{
    const double cx = (rect.left + rect.right + 1.0) / 2.0;
    const double cy = (rect.top + rect.bottom + 1.0) / 2.0;
    const double rx = rect.width() / 2.0;
    const double ry = rect.height() / 2.0;
    const int segments = std::max(2, static_cast<int>(std::ceil(std::abs(span))));
    std::vector<Point> points;
    points.reserve(static_cast<std::size_t>(segments + 2));
    points.push_back({static_cast<float>(cx), static_cast<float>(cy)});
    for (int i = 0; i <= segments; ++i) {
        const double degrees = angle + span * i / segments;
        const double radians = -degrees * std::numbers::pi / 180.0;
        points.push_back({
            static_cast<float>(cx + std::cos(radians) * rx),
            static_cast<float>(cy + std::sin(radians) * ry),
        });
    }
    fill_polygon(points, state);
}

void Surface::stroke_arc(Rect rect, float angle, float span, const DrawState& state)
{
    const double cx = (rect.left + rect.right + 1.0) / 2.0;
    const double cy = (rect.top + rect.bottom + 1.0) / 2.0;
    const double rx = rect.width() / 2.0;
    const double ry = rect.height() / 2.0;
    const int segments = std::max(2, static_cast<int>(std::ceil(std::abs(span))));
    std::vector<Point> points;
    points.reserve(static_cast<std::size_t>(segments + 1));
    for (int i = 0; i <= segments; ++i) {
        const double degrees = angle + span * i / segments;
        const double radians = -degrees * std::numbers::pi / 180.0;
        points.push_back({
            static_cast<float>(cx + std::cos(radians) * rx),
            static_cast<float>(cy + std::sin(radians) * ry),
        });
    }
    stroke_polyline(points, state);
}

void Surface::fill_gradient_rect(Rect rect, const Gradient& gradient,
                                 const DrawState& state)
{
    const std::array<Point, 4> points {{
        {rect.left, rect.top}, {rect.right + 1, rect.top},
        {rect.right + 1, rect.bottom + 1}, {rect.left, rect.bottom + 1},
    }};
    fill_gradient_polygon(points, gradient, state);
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
    for (int y = 0; y < copy_height; ++y) {
        for (int x = 0; x < copy_width; ++x) {
            const int destination_x = b.left + dx + x;
            const int destination_y = b.top + dy + y;
            if (destination_x < 0 || destination_x >= width_
                || destination_y < 0 || destination_y >= height_)
                continue;
            const auto source_offset = static_cast<std::size_t>((y * copy_width + x) * 4);
            set_pixel(destination_x, destination_y,
                      {copy[source_offset + 2], copy[source_offset + 1],
                       copy[source_offset], copy[source_offset + 3]});
        }
    }
}

void Surface::draw_bitmap(const Bitmap& bitmap, Rect source, Rect destination,
                          const DrawState& state)
{
    if (bitmap.width <= 0 || bitmap.height <= 0)
        return;
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
    for (int y = dst.top; y <= dst.bottom; ++y) {
        for (int x = dst.left; x <= dst.right; ++x) {
            const auto local = state.unmap_point(
                {static_cast<float>(x), static_cast<float>(y)});
            if (local.x < destination.left || local.x > destination.right
                || local.y < destination.top || local.y > destination.bottom) {
                continue;
            }
            const int sx = std::clamp(
                static_cast<int>(source.left
                    + (local.x - destination.left)
                        * source_width / destination_width),
                0, bitmap.width - 1);
            const int sy = std::clamp(
                static_cast<int>(source.top
                    + (local.y - destination.top)
                        * source_height / destination_height),
                0, bitmap.height - 1);
            const auto offset = static_cast<std::size_t>((sy * bitmap.width + sx) * 4);
            const Color color {bitmap.bgra[offset + 2], bitmap.bgra[offset + 1],
                               bitmap.bgra[offset], bitmap.bgra[offset + 3]};
            composite(x, y, color, state, true);
        }
    }
}

} // namespace haiku_remote
