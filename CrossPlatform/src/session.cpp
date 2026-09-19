#include "haiku_remote/session.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <sstream>

namespace haiku_remote {
namespace {

constexpr std::uint32_t b_gray1 = 0x0001;
constexpr std::uint32_t b_gray8 = 0x0002;
constexpr std::uint32_t b_rgb24 = 0x0003;
constexpr std::uint32_t b_cmap8 = 0x0004;
constexpr std::uint32_t b_rgb32 = 0x0008;
constexpr std::uint32_t b_rgba32 = 0x2008;
constexpr std::size_t max_decoded_bitmap_size = 256 * 1024 * 1024;
constexpr std::size_t max_readback_size = 64 * 1024 * 1024;

constexpr std::uint32_t shape_move_to = 0x80000000;
constexpr std::uint32_t shape_close = 0x40000000;
constexpr std::uint32_t shape_bezier_to = 0x20000000;
constexpr std::uint32_t shape_line_to = 0x10000000;
constexpr std::uint32_t shape_small_arc_ccw = 0x08000000;
constexpr std::uint32_t shape_small_arc_cw = 0x04000000;
constexpr std::uint32_t shape_large_arc_ccw = 0x02000000;
constexpr std::uint32_t shape_large_arc_cw = 0x01000000;

Point transform_point(Point point, Point offset, float scale)
{
    return {offset.x + 0.5f + point.x * scale,
            offset.y + 0.5f + point.y * scale};
}

void append_cubic(std::vector<Point>& path, Point start, Point control1,
                  Point control2, Point end)
{
    constexpr int segments = 24;
    for (int i = 1; i <= segments; ++i) {
        const double t = static_cast<double>(i) / segments;
        const double u = 1.0 - t;
        path.push_back({
            static_cast<float>(u * u * u * start.x
                + 3 * u * u * t * control1.x
                + 3 * u * t * t * control2.x + t * t * t * end.x),
            static_cast<float>(u * u * u * start.y
                + 3 * u * u * t * control1.y
                + 3 * u * t * t * control2.y + t * t * t * end.y),
        });
    }
}

void append_svg_arc(std::vector<Point>& path, Point start, Point end,
                    double rx, double ry, double angle, bool large_arc, bool sweep)
{
    if (start.x == end.x && start.y == end.y)
        return;
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx == 0 || ry == 0) {
        path.push_back(end);
        return;
    }
    const double cos_a = std::cos(angle);
    const double sin_a = std::sin(angle);
    const double dx2 = (start.x - end.x) / 2;
    const double dy2 = (start.y - end.y) / 2;
    const double x1p = cos_a * dx2 + sin_a * dy2;
    const double y1p = -sin_a * dx2 + cos_a * dy2;
    const double lambda = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lambda > 1) {
        const double factor = std::sqrt(lambda);
        rx *= factor;
        ry *= factor;
    }
    const double rx2 = rx * rx;
    const double ry2 = ry * ry;
    const double denominator = rx2 * y1p * y1p + ry2 * x1p * x1p;
    const double numerator = rx2 * ry2 - denominator;
    double coefficient = denominator > 0
        ? std::sqrt(std::max(0.0, numerator) / denominator) : 0;
    if (large_arc == sweep)
        coefficient = -coefficient;
    const double cxp = coefficient * rx * y1p / ry;
    const double cyp = coefficient * -ry * x1p / rx;
    const double cx = cos_a * cxp - sin_a * cyp + (start.x + end.x) / 2;
    const double cy = sin_a * cxp + cos_a * cyp + (start.y + end.y) / 2;
    const double ux = (x1p - cxp) / rx;
    const double uy = (y1p - cyp) / ry;
    const double vx = (-x1p - cxp) / rx;
    const double vy = (-y1p - cyp) / ry;
    const double theta1 = std::atan2(uy, ux);
    double sweep_angle = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweep && sweep_angle > 0)
        sweep_angle -= 2 * 3.14159265358979323846;
    if (sweep && sweep_angle < 0)
        sweep_angle += 2 * 3.14159265358979323846;
    const int segments = std::max(4, static_cast<int>(
        std::ceil(std::abs(sweep_angle) * 32 / 3.14159265358979323846)));
    for (int i = 1; i <= segments; ++i) {
        const double theta = theta1 + sweep_angle * i / segments;
        const double ct = std::cos(theta);
        const double st = std::sin(theta);
        path.push_back({
            static_cast<float>(cx + rx * cos_a * ct - ry * sin_a * st),
            static_cast<float>(cy + rx * sin_a * ct + ry * cos_a * st),
        });
    }
    path.back() = end;
}

std::vector<Point> rounded_rect_path(Rect rect, float x_radius, float y_radius)
{
    const double rx = std::min(
        std::abs(static_cast<double>(x_radius)),
        std::max(0.0, static_cast<double>(rect.width()) / 2));
    const double ry = std::min(
        std::abs(static_cast<double>(y_radius)),
        std::max(0.0, static_cast<double>(rect.height()) / 2));
    if (rx == 0 || ry == 0) {
        return {
            {rect.left, rect.top}, {rect.right, rect.top},
            {rect.right, rect.bottom}, {rect.left, rect.bottom},
        };
    }

    constexpr int corner_segments = 12;
    std::vector<Point> path;
    path.reserve(corner_segments * 4);
    const std::array<Point, 4> centers {{
        {static_cast<float>(rect.right - rx), static_cast<float>(rect.top + ry)},
        {static_cast<float>(rect.right - rx), static_cast<float>(rect.bottom - ry)},
        {static_cast<float>(rect.left + rx), static_cast<float>(rect.bottom - ry)},
        {static_cast<float>(rect.left + rx), static_cast<float>(rect.top + ry)},
    }};
    for (int corner = 0; corner < 4; ++corner) {
        const double start = -3.14159265358979323846 / 2
            + corner * 3.14159265358979323846 / 2;
        for (int i = 0; i <= corner_segments; ++i) {
            const double angle = start
                + i * 3.14159265358979323846 / (2 * corner_segments);
            path.push_back({
                static_cast<float>(centers[corner].x + std::cos(angle) * rx),
                static_cast<float>(centers[corner].y + std::sin(angle) * ry),
            });
        }
    }
    return path;
}

std::vector<Point> ellipse_path(Rect rect)
{
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
    return points;
}

std::vector<Point> arc_path(Rect rect, float angle, float span, bool filled)
{
    const double cx = (rect.left + rect.right + 1.0) / 2.0;
    const double cy = (rect.top + rect.bottom + 1.0) / 2.0;
    const double rx = rect.width() / 2.0;
    const double ry = rect.height() / 2.0;
    const int segments = std::max(2, static_cast<int>(std::ceil(std::abs(span))));
    std::vector<Point> points;
    points.reserve(static_cast<std::size_t>(segments + (filled ? 2 : 1)));
    if (filled)
        points.push_back({static_cast<float>(cx), static_cast<float>(cy)});
    for (int i = 0; i <= segments; ++i) {
        const double degrees = angle + span * i / segments;
        const double radians = -degrees * std::numbers::pi / 180.0;
        points.push_back({
            static_cast<float>(cx + std::cos(radians) * rx),
            static_cast<float>(cy + std::sin(radians) * ry),
        });
    }
    return points;
}

std::array<Point, 4> rect_path(Rect rect)
{
    return {{
        {rect.left, rect.top}, {rect.right, rect.top},
        {rect.right, rect.bottom}, {rect.left, rect.bottom},
    }};
}

std::size_t utf8_sequence_length(std::string_view text, std::size_t offset)
{
    const auto first = static_cast<unsigned char>(text[offset]);
    std::size_t length = 1;
    if ((first & 0xe0) == 0xc0) length = 2;
    else if ((first & 0xf0) == 0xe0) length = 3;
    else if ((first & 0xf8) == 0xf0) length = 4;
    if (length > text.size() - offset)
        return 1;
    for (std::size_t i = 1; i < length; ++i) {
        if ((static_cast<unsigned char>(text[offset + i]) & 0xc0) != 0x80)
            return 1;
    }
    return length;
}

} // namespace

Session::Session(int width, int height, Send send, Log log)
    : requested_width_(width)
    , requested_height_(height)
    , send_(std::move(send))
    , log_(std::move(log))
    , surface_(width, height)
{
}

void Session::start()
{
    send_message(Writer(Op::init_connection).finish());

    // URP/1 capability handshake, sent before any drawing. Announce our
    // protocol version and the features we implement so the server only
    // drives us with capabilities we actually have. This client shapes text
    // itself and answers RP_STRING_WIDTH, so it advertises
    // RP_CAP_STRING_WIDTH_REPLY. A server that predates the handshake simply
    // ignores this message.
    Writer hello(Op::hello);
    hello.u32(protocol_version);
    hello.u32(cap_string_width_reply);
    hello.u32(0); // max decode width (no Tier P)
    hello.u32(0); // max decode height
    hello.u32(static_cast<std::uint32_t>(requested_width_));
    hello.u32(static_cast<std::uint32_t>(requested_height_));
    send_message(hello.finish());
}

bool Session::send_client_message(std::span<const std::uint8_t> bytes)
{
    return send_(bytes);
}

bool Session::request_full_repaint()
{
    Writer display(Op::update_display_mode);
    display.i32(requested_width_);
    display.i32(requested_height_);
    return send_message(display.finish());
}

void Session::ingest(std::span<const std::uint8_t> bytes)
{
    for (const auto& message : framer_.feed(bytes))
        handle(message);
}

bool Session::send_message(std::vector<std::uint8_t> bytes)
{
    return send_(bytes);
}

DrawState& Session::state(std::int32_t token)
{
    return states_[token];
}

void Session::handle(const Message& message)
{
    ++message_count_;
    try {
        Reader reader(message.payload);
        if (is_session_level(message.op))
            handle_session(message.op, reader);
        else
            handle_token(message.op, reader.i32(), reader);
    } catch (const std::exception& error) {
        if (log_) {
            std::ostringstream text;
            text << "decode error in " << op_name(message.op) << ": " << error.what();
            log_(text.str());
        }
    }
}

void Session::handle_session(Op op, Reader& reader)
{
    switch (op) {
    case Op::init_connection: {
        Writer palette(Op::get_system_palette);
        send_message(palette.finish());
        Writer display(Op::update_display_mode);
        display.i32(requested_width_);
        display.i32(requested_height_);
        send_message(display.finish());
        break;
    }
    case Op::hello_ack: {
        // Negotiated protocol version and capability intersection from the
        // server. Nothing is gated on them yet, but they are recorded so a
        // later milestone (or --stats style diagnostics) can inspect them.
        negotiated_version_ = reader.u32();
        negotiated_capabilities_ = reader.u32();
        if (log_) {
            std::ostringstream text;
            text << "hello ack: version " << negotiated_version_
                 << ", capabilities 0x" << std::hex << negotiated_capabilities_;
            log_(text.str());
        }
        break;
    }
    case Op::get_system_palette_result: {
        const auto count = std::min<std::uint32_t>(reader.u32(), 4096);
        palette_.clear();
        palette_.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
            palette_.push_back(reader.color());
        break;
    }
    case Op::create_state:
        states_[reader.i32()] = DrawState {};
        break;
    case Op::delete_state:
        states_.erase(reader.i32());
        break;
    case Op::copy_rect_no_clipping: {
        const int dx = reader.i32();
        const int dy = reader.i32();
        surface_.copy_rect(reader.rect(), dx, dy);
        break;
    }
    case Op::fill_region_color_no_clipping: {
        const auto region = reader.region();
        const auto color = reader.color();
        for (const auto& rect : region)
            surface_.fill_rect_color(rect, color);
        break;
    }
    case Op::invalidate_rect:
    case Op::invalidate_region:
    case Op::set_cursor:
    case Op::set_cursor_visible:
    case Op::move_cursor_to:
    case Op::close_connection:
        break;
    default:
        note_unhandled(op);
        break;
    }
}

void Session::handle_token(Op op, std::int32_t token, Reader& reader)
{
    auto& draw = state(token);
    switch (op) {
    case Op::set_high_color:
        draw.high = reader.color();
        break;
    case Op::set_low_color:
        draw.low = reader.color();
        break;
    case Op::set_pen_size:
        draw.pen_size = reader.f32();
        break;
    case Op::set_pattern: {
        const auto pattern = reader.raw(8);
        std::copy(pattern.begin(), pattern.end(), draw.pattern.begin());
        break;
    }
    case Op::set_font:
        draw.font = reader.font();
        break;
    case Op::set_transform:
        draw.transform = reader.transform();
        break;
    case Op::set_offsets:
        draw.x_offset = reader.i32();
        draw.y_offset = reader.i32();
        break;
    case Op::set_stroke_mode:
        draw.line_cap = reader.u32();
        draw.line_join = reader.u32();
        draw.miter_limit = reader.f32();
        break;
    case Op::set_blending_mode:
        draw.constant_alpha = reader.u32() == 1;
        (void)reader.u32();
        if (draw.blend_modes_enabled)
            draw.force_opaque = draw.constant_alpha;
        break;
    case Op::set_drawing_mode: {
        const auto raw = reader.u32();
        draw.drawing_mode = raw <= 10 ? static_cast<DrawingMode>(raw)
                                      : DrawingMode::over;
        draw.force_opaque = draw.drawing_mode == DrawingMode::copy;
        draw.blend_modes_enabled = draw.drawing_mode == DrawingMode::alpha;
        if (draw.blend_modes_enabled)
            draw.force_opaque = draw.constant_alpha;
        break;
    }
    case Op::constrain_clipping_region:
        draw.clip_rects = reader.region();
        break;
    case Op::fill_rect:
    {
        const auto rect = reader.rect();
        surface_.fill_rect(rect, draw);
        break;
    }
    case Op::fill_rect_color: {
        const auto rect = reader.rect();
        const auto color = reader.color();
        surface_.fill_rect_color(rect, color, &draw);
        break;
    }
    case Op::stroke_rect_1px_color: {
        const auto rect = reader.rect();
        surface_.stroke_rect(rect, reader.color(), &draw);
        break;
    }
    case Op::stroke_point_color: {
        const auto point = reader.point();
        const auto color = reader.color();
        surface_.fill_rect_color({point.x, point.y, point.x, point.y}, color, &draw);
        break;
    }
    case Op::stroke_line_1px_color: {
        const auto from = reader.point();
        const auto to = reader.point();
        auto one_pixel = draw;
        one_pixel.pen_size = 1;
        surface_.line(from, to, reader.color(), &one_pixel);
        break;
    }
    case Op::stroke_line:
        surface_.line(reader.point(), reader.point(), draw.high, &draw, true);
        break;
    case Op::stroke_line_gradient: {
        const std::array<Point, 2> points {reader.point(), reader.point()};
        surface_.stroke_gradient_polyline(points, reader.gradient(), draw);
        break;
    }
    case Op::stroke_line_array: {
        const auto count = reader.i32();
        if (count < 0 || count > (1 << 20))
            throw ProtocolError("invalid line-array count");
        for (std::int32_t i = 0; i < count; ++i) {
            const auto from = reader.point();
            const auto to = reader.point();
            const auto color = reader.color();
            surface_.line(from, to, color, &draw);
        }
        break;
    }
    case Op::stroke_rect: {
        const auto path = rect_path(reader.rect());
        surface_.stroke_polyline(path, draw, true);
        break;
    }
    case Op::fill_round_rect:
    case Op::stroke_round_rect:
    case Op::fill_round_rect_gradient:
    case Op::stroke_round_rect_gradient: {
        const auto rect = reader.rect();
        const auto path = rounded_rect_path(rect, reader.f32(), reader.f32());
        const bool gradient = op == Op::fill_round_rect_gradient
            || op == Op::stroke_round_rect_gradient;
        const bool filled = op == Op::fill_round_rect
            || op == Op::fill_round_rect_gradient;
        if (gradient) {
            const auto value = reader.gradient();
            if (filled)
                surface_.fill_gradient_polygon(path, value, draw);
            else
                surface_.stroke_gradient_polyline(path, value, draw, true);
        } else if (filled) {
            surface_.fill_polygon(path, draw);
        } else {
            surface_.stroke_polyline(path, draw, true);
        }
        break;
    }
    case Op::fill_ellipse:
        surface_.fill_ellipse(reader.rect(), draw);
        break;
    case Op::stroke_ellipse:
        surface_.stroke_ellipse(reader.rect(), draw);
        break;
    case Op::fill_ellipse_gradient:
    case Op::stroke_ellipse_gradient: {
        const auto path = ellipse_path(reader.rect());
        const auto gradient = reader.gradient();
        if (op == Op::fill_ellipse_gradient)
            surface_.fill_gradient_polygon(path, gradient, draw);
        else
            surface_.stroke_gradient_polyline(path, gradient, draw, true);
        break;
    }
    case Op::fill_arc: {
        const auto rect = reader.rect();
        const auto angle = reader.f32();
        const auto span = reader.f32();
        surface_.fill_arc(rect, angle, span, draw);
        break;
    }
    case Op::stroke_arc: {
        const auto rect = reader.rect();
        const auto angle = reader.f32();
        const auto span = reader.f32();
        surface_.stroke_arc(rect, angle, span, draw);
        break;
    }
    case Op::fill_arc_gradient:
    case Op::stroke_arc_gradient: {
        const auto rect = reader.rect();
        const auto angle = reader.f32();
        const auto span = reader.f32();
        const bool filled = op == Op::fill_arc_gradient;
        const auto path = arc_path(rect, angle, span, filled);
        const auto gradient = reader.gradient();
        if (filled)
            surface_.fill_gradient_polygon(path, gradient, draw);
        else
            surface_.stroke_gradient_polyline(path, gradient, draw);
        break;
    }
    case Op::invert_rect:
        surface_.invert_rect(reader.rect(), &draw);
        break;
    case Op::fill_region: {
        for (const auto& rect : reader.region())
            surface_.fill_rect(rect, draw);
        break;
    }
    case Op::fill_region_gradient: {
        const auto region = reader.region();
        const auto gradient = reader.gradient();
        for (const auto& rect : region)
            surface_.fill_gradient_rect(rect, gradient, draw);
        break;
    }
    case Op::fill_rect_gradient:
    case Op::stroke_rect_gradient: {
        const auto rect = reader.rect();
        const auto gradient = reader.gradient();
        if (op == Op::fill_rect_gradient) {
            surface_.fill_gradient_rect(rect, gradient, draw);
        } else {
            const auto path = rect_path(rect);
            surface_.stroke_gradient_polyline(path, gradient, draw, true);
        }
        break;
    }
    case Op::fill_polygon:
    case Op::stroke_polygon:
    case Op::fill_polygon_gradient:
    case Op::stroke_polygon_gradient: {
        (void)reader.rect();
        const bool closed = reader.boolean();
        const auto count = reader.i32();
        if (count < 0 || count > (1 << 20))
            throw ProtocolError("invalid polygon point count");
        std::vector<Point> points;
        points.reserve(static_cast<std::size_t>(count));
        for (std::int32_t i = 0; i < count; ++i)
            points.push_back(reader.point());
        const bool gradient = op == Op::fill_polygon_gradient
            || op == Op::stroke_polygon_gradient;
        const bool filled = op == Op::fill_polygon
            || op == Op::fill_polygon_gradient;
        if (gradient) {
            const auto value = reader.gradient();
            if (filled)
                surface_.fill_gradient_polygon(points, value, draw);
            else
                surface_.stroke_gradient_polyline(points, value, draw, closed);
        } else if (filled) {
            surface_.fill_polygon(points, draw);
        } else {
            surface_.stroke_polyline(points, draw, closed);
        }
        break;
    }
    case Op::fill_bezier:
    case Op::stroke_bezier:
    case Op::fill_bezier_gradient:
    case Op::stroke_bezier_gradient: {
        const auto start = reader.point();
        const auto control1 = reader.point();
        const auto control2 = reader.point();
        const auto end = reader.point();
        std::vector<Point> path {start};
        append_cubic(path, start, control1, control2, end);
        const bool gradient = op == Op::fill_bezier_gradient
            || op == Op::stroke_bezier_gradient;
        const bool filled = op == Op::fill_bezier
            || op == Op::fill_bezier_gradient;
        if (gradient) {
            const auto value = reader.gradient();
            if (filled)
                surface_.fill_gradient_polygon(path, value, draw);
            else
                surface_.stroke_gradient_polyline(path, value, draw);
        } else if (filled) {
            surface_.fill_polygon(path, draw);
        } else {
            surface_.stroke_polyline(path, draw);
        }
        break;
    }
    case Op::fill_triangle:
    case Op::stroke_triangle:
    case Op::fill_triangle_gradient:
    case Op::stroke_triangle_gradient: {
        std::array<Point, 3> points {
            reader.point(), reader.point(), reader.point()
        };
        (void)reader.rect();
        for (auto& point : points) {
            point.x += 0.5f;
            point.y += 0.5f;
        }
        const bool gradient = op == Op::fill_triangle_gradient
            || op == Op::stroke_triangle_gradient;
        const bool filled = op == Op::fill_triangle
            || op == Op::fill_triangle_gradient;
        if (gradient) {
            const auto value = reader.gradient();
            if (filled)
                surface_.fill_gradient_polygon(points, value, draw);
            else
                surface_.stroke_gradient_polyline(points, value, draw, true);
        } else if (filled) {
            surface_.fill_polygon(points, draw);
        } else {
            surface_.stroke_polyline(points, draw, true);
        }
        break;
    }
    case Op::fill_shape:
    case Op::stroke_shape:
    case Op::fill_shape_gradient:
    case Op::stroke_shape_gradient: {
        (void)reader.rect();
        const auto op_count = reader.i32();
        if (op_count < 0 || op_count > (1 << 20))
            throw ProtocolError("invalid shape op count");
        std::vector<std::uint32_t> operations;
        operations.reserve(static_cast<std::size_t>(op_count));
        for (std::int32_t i = 0; i < op_count; ++i)
            operations.push_back(reader.u32());
        const auto point_count = reader.i32();
        if (point_count < 0 || point_count > (1 << 20))
            throw ProtocolError("invalid shape point count");
        std::vector<Point> points;
        points.reserve(static_cast<std::size_t>(point_count));
        for (std::int32_t i = 0; i < point_count; ++i)
            points.push_back(reader.point());
        const auto offset = reader.point();
        const auto scale = reader.f32();

        std::vector<Point> path;
        std::size_t index = 0;
        Point current {};
        Point first {};
        bool started = false;
        for (const auto word : operations) {
            const auto flags = word & 0xff000000;
            const auto count = static_cast<std::size_t>(word & 0x00ffffff);
            if ((flags & shape_move_to) != 0 && index < points.size()) {
                current = transform_point(points[index++], offset, scale);
                first = current;
                path.push_back(current);
                started = true;
            }
            if ((flags & shape_line_to) != 0) {
                for (std::size_t i = 0; i < count && index < points.size(); ++i) {
                    current = transform_point(points[index++], offset, scale);
                    path.push_back(current);
                }
            }
            if ((flags & shape_bezier_to) != 0) {
                for (std::size_t i = 0; i < count / 3 && index + 2 < points.size(); ++i) {
                    const auto c1 = transform_point(points[index], offset, scale);
                    const auto c2 = transform_point(points[index + 1], offset, scale);
                    const auto end = transform_point(points[index + 2], offset, scale);
                    append_cubic(path, current, c1, c2, end);
                    current = end;
                    index += 3;
                }
            }
            const auto arc_mask = shape_small_arc_ccw | shape_small_arc_cw
                | shape_large_arc_ccw | shape_large_arc_cw;
            if ((flags & arc_mask) != 0) {
                const bool large = (flags & (shape_large_arc_ccw | shape_large_arc_cw)) != 0;
                const bool sweep = (flags & (shape_small_arc_cw | shape_large_arc_cw)) != 0;
                for (std::size_t i = 0; i < count / 3 && index + 2 < points.size(); ++i) {
                    const auto radii = points[index];
                    const auto angle = points[index + 1].x;
                    const auto end = transform_point(points[index + 2], offset, scale);
                    append_svg_arc(path, current, end,
                                   radii.x * scale, radii.y * scale,
                                   angle, large, sweep);
                    current = end;
                    index += 3;
                }
            }
            if ((flags & shape_close) != 0 && started && path.back() != first)
                path.push_back(first);
        }
        const bool gradient = op == Op::fill_shape_gradient
            || op == Op::stroke_shape_gradient;
        const bool filled = op == Op::fill_shape
            || op == Op::fill_shape_gradient;
        if (gradient) {
            const auto value = reader.gradient();
            if (filled)
                surface_.fill_gradient_polygon(path, value, draw);
            else
                surface_.stroke_gradient_polyline(path, value, draw);
        } else if (filled) {
            surface_.fill_polygon(path, draw);
        } else {
            surface_.stroke_polyline(path, draw);
        }
        break;
    }
    case Op::draw_bitmap: {
        const auto source = reader.rect();
        const auto destination = reader.rect();
        (void)reader.u32();
        const auto bitmap = read_bitmap(reader);
        surface_.draw_bitmap(bitmap, source, destination, draw);
        break;
    }
    case Op::draw_bitmap_rects: {
        (void)reader.u32();
        const auto color_space = reader.u32();
        (void)reader.u32();
        const auto count = reader.i32();
        if (count < 0 || count > (1 << 16))
            throw ProtocolError("invalid bitmap rectangle count");
        for (std::int32_t i = 0; i < count; ++i) {
            const auto destination = reader.rect();
            const auto bitmap = read_bitmap(reader, true, color_space);
            surface_.draw_bitmap(
                bitmap, {0, 0, static_cast<float>(bitmap.width - 1),
                         static_cast<float>(bitmap.height - 1)},
                destination, draw);
        }
        break;
    }
    case Op::draw_string: {
        const auto where = reader.point();
        const auto text = reader.string();
        const float advance = text_.draw(text, where, draw, surface_);
        if (std::getenv("HAIKU_REMOTE_TRACE_TEXT") != nullptr && log_) {
            std::ostringstream trace;
            trace << "text #" << message_count_ << " token=" << token
                  << " at=" << where.x << ',' << where.y
                  << " advance=" << advance
                  << " font=" << draw.font.size
                  << " offset=" << draw.x_offset << ',' << draw.y_offset
                  << " clip=";
            if (draw.clip_rects.empty()) {
                trace << "none";
            } else {
                float left = draw.clip_rects.front().left;
                float top = draw.clip_rects.front().top;
                float right = draw.clip_rects.front().right;
                float bottom = draw.clip_rects.front().bottom;
                for (const auto& rect : draw.clip_rects) {
                    left = std::min(left, rect.left);
                    top = std::min(top, rect.top);
                    right = std::max(right, rect.right);
                    bottom = std::max(bottom, rect.bottom);
                }
                trace << left << ',' << top << '-' << right << ',' << bottom
                      << " rects=[";
                for (std::size_t i = 0; i < draw.clip_rects.size(); ++i) {
                    if (i != 0)
                        trace << ';';
                    const auto& rect = draw.clip_rects[i];
                    trace << rect.left << ',' << rect.top << '-'
                          << rect.right << ',' << rect.bottom;
                }
                trace << ']';
            }
            trace << " value=\"";
            for (const unsigned char character : text) {
                if (character >= 0x20 && character < 0x7f
                    && character != '\\' && character != '"') {
                    trace << static_cast<char>(character);
                } else {
                    trace << "\\x";
                    constexpr char hex[] = "0123456789abcdef";
                    trace << hex[character >> 4] << hex[character & 0x0f];
                }
            }
            trace << '"';
            log_(trace.str());
        }
        Writer reply(Op::draw_string_result);
        reply.i32(token);
        reply.point({where.x + advance, where.y});
        send_message(reply.finish());
        break;
    }
    case Op::draw_string_with_offsets: {
        const auto text = reader.string();
        Point last {};
        std::string_view last_scalar;
        for (std::size_t offset = 0; offset < text.size();) {
            const auto length = utf8_sequence_length(text, offset);
            last = reader.point();
            last_scalar = std::string_view(text).substr(offset, length);
            text_.draw(last_scalar, last, draw, surface_);
            offset += length;
        }
        Writer reply(Op::draw_string_result);
        reply.i32(token);
        if (!last_scalar.empty())
            last.x += text_.width(last_scalar, draw.font);
        reply.point(last);
        send_message(reply.finish());
        break;
    }
    case Op::string_width: {
        const auto text = reader.string();
        Writer reply(Op::string_width_result);
        reply.i32(token);
        reply.f32(text_.width(text, draw.font));
        send_message(reply.finish());
        break;
    }
    case Op::read_bitmap: {
        const auto bounds = reader.rect();
        (void)reader.boolean();
        const auto raster = intersect(raster_bounds(bounds),
                                      {0, 0, surface_.width() - 1, surface_.height() - 1});
        if (raster.empty())
            break;
        const int width = raster.right - raster.left + 1;
        const int height = raster.bottom - raster.top + 1;
        const int bytes_per_row = (width * 3 + 3) & ~3;
        const auto bits_size = static_cast<std::size_t>(bytes_per_row)
            * static_cast<std::size_t>(height);
        if (bits_size > max_readback_size)
            throw ProtocolError("bitmap readback exceeds the 64 MiB safety limit");
        std::vector<std::uint8_t> bits(bits_size);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto color = surface_.pixel(raster.left + x, raster.top + y);
                const auto destination = static_cast<std::size_t>(
                    y * bytes_per_row + x * 3);
                bits[destination] = color.b;
                bits[destination + 1] = color.g;
                bits[destination + 2] = color.r;
            }
        }
        Writer reply(Op::read_bitmap_result);
        reply.i32(token);
        reply.i32(width);
        reply.i32(height);
        reply.i32(bytes_per_row);
        reply.u32(b_rgb24);
        reply.u32(0);
        reply.u32(static_cast<std::uint32_t>(bits.size()));
        reply.raw(bits);
        send_message(reply.finish());
        break;
    }
    case Op::enable_sync_drawing:
    case Op::disable_sync_drawing:
        break;
    default:
        note_unhandled(op);
        break;
    }
}

Bitmap Session::read_bitmap(Reader& reader, bool minimal,
                            std::uint32_t inherited_color_space)
{
    Bitmap result;
    result.width = reader.i32();
    result.height = reader.i32();
    const int bytes_per_row = reader.i32();
    const std::uint32_t color_space = minimal ? inherited_color_space : reader.u32();
    if (!minimal)
        (void)reader.u32();
    const auto bits_size = static_cast<std::size_t>(reader.u32());
    if (result.width <= 0 || result.height <= 0 || bytes_per_row <= 0
        || result.width > Surface::max_dimension
        || result.height > Surface::max_dimension) {
        throw ProtocolError("invalid bitmap dimensions");
    }

    std::size_t minimum_row_size = 0;
    switch (color_space) {
    case b_rgb32:
    case b_rgba32:
        minimum_row_size = static_cast<std::size_t>(result.width) * 4;
        break;
    case b_rgb24:
        minimum_row_size = static_cast<std::size_t>(result.width) * 3;
        break;
    case b_gray8:
    case b_cmap8:
        minimum_row_size = static_cast<std::size_t>(result.width);
        break;
    case b_gray1:
        minimum_row_size = (static_cast<std::size_t>(result.width) + 7) / 8;
        break;
    default:
        throw ProtocolError("unsupported bitmap color space");
    }
    if (static_cast<std::size_t>(bytes_per_row) < minimum_row_size)
        throw ProtocolError("bitmap row is shorter than its pixel data");
    const auto required_bits = static_cast<std::size_t>(bytes_per_row)
        * static_cast<std::size_t>(result.height);
    if (bits_size < required_bits || bits_size > reader.remaining())
        throw ProtocolError("bitmap data is truncated");
    const auto decoded_size = static_cast<std::size_t>(result.width)
        * static_cast<std::size_t>(result.height) * 4;
    if (decoded_size > max_decoded_bitmap_size)
        throw ProtocolError("decoded bitmap exceeds the 256 MiB safety limit");

    const auto bits = reader.raw(bits_size);
    result.bgra.resize(decoded_size);
    for (int y = 0; y < result.height; ++y) {
        const auto row = static_cast<std::size_t>(y)
            * static_cast<std::size_t>(bytes_per_row);
        for (int x = 0; x < result.width; ++x) {
            Color color;
            switch (color_space) {
            case b_rgb32:
            case b_rgba32: {
                const auto source = row + static_cast<std::size_t>(x * 4);
                color = {bits.at(source + 2), bits.at(source + 1),
                         bits.at(source), color_space == b_rgba32
                             ? bits.at(source + 3) : std::uint8_t {255}};
                break;
            }
            case b_rgb24: {
                const auto source = row + static_cast<std::size_t>(x * 3);
                color = {bits.at(source + 2), bits.at(source + 1), bits.at(source), 255};
                break;
            }
            case b_gray8: {
                const auto value = bits.at(row + static_cast<std::size_t>(x));
                color = {value, value, value, 255};
                break;
            }
            case b_gray1: {
                const auto byte = bits.at(row + static_cast<std::size_t>(x / 8));
                const auto value = (byte & (1u << (x & 7))) != 0 ? 255 : 0;
                color = {static_cast<std::uint8_t>(value),
                         static_cast<std::uint8_t>(value),
                         static_cast<std::uint8_t>(value), 255};
                break;
            }
            case b_cmap8: {
                const auto index = bits.at(row + static_cast<std::size_t>(x));
                color = index < palette_.size() ? palette_[index] : Color {};
                break;
            }
            }
            const auto destination = (
                static_cast<std::size_t>(y)
                    * static_cast<std::size_t>(result.width)
                + static_cast<std::size_t>(x)) * 4;
            result.bgra[destination] = color.b;
            result.bgra[destination + 1] = color.g;
            result.bgra[destination + 2] = color.r;
            result.bgra[destination + 3] = color.a;
        }
    }
    return result;
}

void Session::note_unhandled(Op op)
{
    auto& count = unhandled_[static_cast<std::uint16_t>(op)];
    ++count;
    if (count == 1 && log_) {
        std::ostringstream text;
        text << "unhandled op " << op_name(op)
             << " (" << static_cast<std::uint16_t>(op) << ')';
        log_(text.str());
    }
}

} // namespace haiku_remote
