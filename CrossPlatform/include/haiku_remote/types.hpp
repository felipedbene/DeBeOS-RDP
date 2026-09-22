#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace haiku_remote {

struct Point {
    float x = 0;
    float y = 0;

    friend bool operator==(const Point&, const Point&) = default;
};

// Haiku rectangles have inclusive right and bottom edges.
struct Rect {
    float left = 0;
    float top = 0;
    float right = -1;
    float bottom = -1;

    [[nodiscard]] float width() const { return right - left + 1; }
    [[nodiscard]] float height() const { return bottom - top + 1; }
    [[nodiscard]] bool empty() const { return right < left || bottom < top; }

    friend bool operator==(const Rect&, const Rect&) = default;
};

struct IntRect {
    int left = 0;
    int top = 0;
    int right = -1;
    int bottom = -1;

    [[nodiscard]] bool empty() const { return right < left || bottom < top; }
};

// Bounding box of a rect that arrived off the wire. Coordinates are unvalidated
// floats, and converting one that is NaN or outside int range is undefined
// behaviour, so clamp first: no surface is wider than Surface::max_dimension, so
// a bound of +-1e9 cannot change which pixels are covered.
inline int raster_coordinate(double value, int on_nan)
{
    if (std::isnan(value))
        return on_nan;
    return static_cast<int>(std::clamp(value, -1.0e9, 1.0e9));
}

inline IntRect raster_bounds(const Rect& rect)
{
    return {
        raster_coordinate(std::floor(rect.left), 1),
        raster_coordinate(std::floor(rect.top), 1),
        raster_coordinate(std::ceil(rect.right), 0),
        raster_coordinate(std::ceil(rect.bottom), 0),
    };
}

inline IntRect intersect(IntRect a, IntRect b)
{
    return {
        std::max(a.left, b.left),
        std::max(a.top, b.top),
        std::min(a.right, b.right),
        std::min(a.bottom, b.bottom),
    };
}

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    friend bool operator==(const Color&, const Color&) = default;
};

enum class DrawingMode : std::uint32_t {
    copy = 0,
    over = 1,
    erase = 2,
    invert = 3,
    add = 4,
    subtract = 5,
    blend = 6,
    min = 7,
    max = 8,
    select = 9,
    alpha = 10,
};

struct Font {
    std::uint8_t direction = 0;
    std::uint8_t encoding = 0;
    std::uint32_t flags = 0;
    std::uint8_t spacing = 0;
    float shear = 0;
    float rotation = 0;
    float false_bold_width = 0;
    float size = 12;
    std::uint16_t face = 0;
    std::uint16_t family = 0;
    std::uint16_t style = 0;
};

struct GradientStop {
    Color color;
    float offset = 0;
};

struct Gradient {
    enum class Kind : std::uint32_t {
        linear = 0,
        radial = 1,
        radial_focus = 2,
        diamond = 3,
        conic = 4,
        none = 5,
    };

    Kind kind = Kind::none;
    Point start;
    Point end;
    Point center;
    Point focal;
    float radius = 0;
    float angle = 0;
    std::vector<GradientStop> stops;
};

struct Transform {
    double sx = 1;
    double shy = 0;
    double shx = 0;
    double sy = 1;
    double tx = 0;
    double ty = 0;

    [[nodiscard]] bool is_identity() const
    {
        return sx == 1 && shy == 0 && shx == 0 && sy == 1 && tx == 0 && ty == 0;
    }
};

struct DrawState {
    Color high {0, 0, 0, 255};
    Color low {255, 255, 255, 255};
    float pen_size = 1;
    std::array<std::uint8_t, 8> pattern {0xff, 0xff, 0xff, 0xff,
                                         0xff, 0xff, 0xff, 0xff};
    Font font;
    Transform transform;
    std::int32_t x_offset = 0;
    std::int32_t y_offset = 0;
    std::uint32_t line_cap = 0;
    std::uint32_t line_join = 0;
    float miter_limit = 10;
    std::vector<Rect> clip_rects;
    // Whether RP_CONSTRAIN_CLIPPING_REGION has ever been received for this
    // token. An *empty* region is a legal and meaningful value -- it means
    // "nothing may be drawn" -- so emptiness alone cannot stand in for "no
    // clipping". app_server reaches that state: ServerWindow.cpp:2511-2533
    // bails out of _DispatchViewDrawingMessage when the drawing region is
    // empty *except* for AS_VIEW_END_LAYER, where it constrains the engine to
    // the empty region and then plays the layer back anyway.
    bool clipping_set = false;
    DrawingMode drawing_mode = DrawingMode::copy;
    bool constant_alpha = false;
    bool blend_modes_enabled = false;
    bool force_opaque = true;

    [[nodiscard]] bool pattern_is_high(int x, int y) const
    {
        const int px = (x - x_offset) & 7;
        const int py = (y - y_offset) & 7;
        return (pattern[static_cast<std::size_t>(py)] & (1u << (7 - px))) != 0;
    }

    [[nodiscard]] Color source_at(int x, int y) const
    {
        return pattern_is_high(x, y) ? high : low;
    }

    [[nodiscard]] Point map_point(Point point) const
    {
        if (transform.is_identity())
            return point;
        const double x = point.x - x_offset;
        const double y = point.y - y_offset;
        return {
            static_cast<float>(
                transform.sx * x + transform.shx * y + transform.tx + x_offset),
            static_cast<float>(
                transform.shy * x + transform.sy * y + transform.ty + y_offset),
        };
    }

    [[nodiscard]] Point unmap_point(Point point) const
    {
        if (transform.is_identity())
            return point;
        const double determinant =
            transform.sx * transform.sy - transform.shx * transform.shy;
        if (std::abs(determinant) < 1e-12)
            return point;
        const double x = point.x - x_offset - transform.tx;
        const double y = point.y - y_offset - transform.ty;
        return {
            static_cast<float>(
                (transform.sy * x - transform.shx * y) / determinant + x_offset),
            static_cast<float>(
                (-transform.shy * x + transform.sx * y) / determinant + y_offset),
        };
    }
};

} // namespace haiku_remote
