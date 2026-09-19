#pragma once

#include "haiku_remote/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace haiku_remote {

enum class Op : std::uint16_t {
    init_connection = 1,
    update_display_mode = 2,
    close_connection = 3,
    get_system_palette = 4,
    get_system_palette_result = 5,
    hello = 6,
    hello_ack = 7,
    // Broker transport-security preamble; spoken by the WebSocket transport
    // before the session starts, never seen by Session.
    authenticate = 10,
    auth_result = 11,
    create_state = 20,
    delete_state = 21,
    enable_sync_drawing = 22,
    disable_sync_drawing = 23,
    invalidate_rect = 24,
    invalidate_region = 25,
    set_offsets = 40,
    set_high_color = 41,
    set_low_color = 42,
    set_pen_size = 43,
    set_stroke_mode = 44,
    set_blending_mode = 45,
    set_pattern = 46,
    set_drawing_mode = 47,
    set_font = 48,
    set_transform = 49,
    constrain_clipping_region = 60,
    copy_rect_no_clipping = 61,
    invert_rect = 62,
    draw_bitmap = 63,
    draw_bitmap_rects = 64,
    stroke_arc = 80,
    stroke_bezier = 81,
    stroke_ellipse = 82,
    stroke_polygon = 83,
    stroke_rect = 84,
    stroke_round_rect = 85,
    stroke_shape = 86,
    stroke_triangle = 87,
    stroke_line = 88,
    stroke_line_array = 89,
    fill_arc = 100,
    fill_bezier = 101,
    fill_ellipse = 102,
    fill_polygon = 103,
    fill_rect = 104,
    fill_round_rect = 105,
    fill_shape = 106,
    fill_triangle = 107,
    fill_region = 108,
    fill_arc_gradient = 120,
    fill_bezier_gradient = 121,
    fill_ellipse_gradient = 122,
    fill_polygon_gradient = 123,
    fill_rect_gradient = 124,
    fill_round_rect_gradient = 125,
    fill_shape_gradient = 126,
    fill_triangle_gradient = 127,
    fill_region_gradient = 128,
    stroke_point_color = 140,
    stroke_line_1px_color = 141,
    stroke_rect_1px_color = 142,
    fill_rect_color = 160,
    fill_region_color_no_clipping = 161,
    draw_string = 180,
    draw_string_with_offsets = 181,
    draw_string_result = 182,
    string_width = 183,
    string_width_result = 184,
    read_bitmap = 185,
    read_bitmap_result = 186,
    set_cursor = 200,
    set_cursor_visible = 201,
    move_cursor_to = 202,
    mouse_moved = 220,
    mouse_down = 221,
    mouse_up = 222,
    mouse_wheel_changed = 223,
    key_down = 240,
    key_up = 241,
    unmapped_key_down = 242,
    unmapped_key_up = 243,
    modifiers_changed = 244,
    stroke_arc_gradient = 260,
    stroke_bezier_gradient = 261,
    stroke_ellipse_gradient = 262,
    stroke_polygon_gradient = 263,
    stroke_rect_gradient = 264,
    stroke_round_rect_gradient = 265,
    stroke_shape_gradient = 266,
    stroke_triangle_gradient = 267,
    stroke_line_gradient = 268,
};

constexpr std::size_t message_header_size = 6;

// URP/1 protocol version carried in RP_HELLO / RP_HELLO_ACK. The negotiated
// version is min(client, server).
constexpr std::uint32_t protocol_version = 1;

// URP/1 capability bits advertised in the RP_HELLO feature bitmap. The server
// may only use a feature the client advertised, and echoes the negotiated
// intersection back in RP_HELLO_ACK.
//
// This client shapes and measures text itself (text_engine), so it can answer
// RP_STRING_WIDTH with RP_STRING_WIDTH_RESULT.
constexpr std::uint32_t cap_string_width_reply = 1u << 0;

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes);

    [[nodiscard]] std::size_t remaining() const;
    std::uint8_t u8();
    bool boolean();
    std::uint16_t u16();
    std::int32_t i32();
    std::uint32_t u32();
    float f32();
    double f64();
    Point point();
    Rect rect();
    Color color();
    Font font();
    Transform transform();
    Gradient gradient();
    std::vector<Rect> region();
    std::string string();
    std::vector<std::uint8_t> raw(std::size_t count);

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
    void require(std::size_t count) const;
};

class Writer {
public:
    explicit Writer(Op op);

    void u8(std::uint8_t value);
    void boolean(bool value);
    void u16(std::uint16_t value);
    void i32(std::int32_t value);
    void u32(std::uint32_t value);
    void f32(float value);
    void point(Point value);
    void string(std::string_view value);
    void raw(std::span<const std::uint8_t> value);
    std::vector<std::uint8_t> finish();

private:
    std::vector<std::uint8_t> bytes_;
};

struct Message {
    Op op;
    std::vector<std::uint8_t> payload;
};

class Framer {
public:
    std::vector<Message> feed(std::span<const std::uint8_t> bytes);
    void reset();
    [[nodiscard]] std::size_t pending_bytes() const { return buffer_.size(); }

private:
    static constexpr std::size_t max_message_size = 64 * 1024 * 1024;
    std::vector<std::uint8_t> buffer_;
};

[[nodiscard]] std::string_view op_name(Op op);
[[nodiscard]] bool is_session_level(Op op);

} // namespace haiku_remote
