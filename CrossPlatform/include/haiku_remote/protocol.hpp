#pragma once

#include "haiku_remote/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace haiku_remote {

// The opcode set, transcribed from the server's own enum in
// src/servers/app/drawing/interface/remote/RemoteMessage.h -- names and values
// verbatim, lower-cased for the C++ enumerator.
//
// It is one table, and everything that needs the opcode set is generated from
// it: the Op enum below, op_name()'s wire names, and all_ops(). Two
// hand-maintained lists drift -- they did: op_name() named 29 of the 82 codes
// the server can send and the rest logged as "RP_UNKNOWN", so the decode-error
// and unhandled-opcode diagnostics were blind for exactly the complex payloads
// (shapes, gradients, bitmap rects, font state) that need naming. A code the
// table does not define is still reported with its number, because "RP_UNKNOWN"
// alone says nothing.
//
// Do not add an enumerator to Op by hand: add a row here. A hand-added
// enumerator is caught at build time anyway, because op_name()'s switch has no
// default arm and -Wswitch then fails the build for the unnamed value.
//
// X(enumerator, value, wire name). No // comments and no blank lines inside the
// definition: line splicing happens before comment removal, so either one
// silently truncates the table.
#define HAIKU_REMOTE_OP_TABLE(X) \
    X(init_connection, 1, "RP_INIT_CONNECTION") \
    X(update_display_mode, 2, "RP_UPDATE_DISPLAY_MODE") \
    X(close_connection, 3, "RP_CLOSE_CONNECTION") \
    X(get_system_palette, 4, "RP_GET_SYSTEM_PALETTE") \
    X(get_system_palette_result, 5, "RP_GET_SYSTEM_PALETTE_RESULT") \
    X(hello, 6, "RP_HELLO") \
    X(hello_ack, 7, "RP_HELLO_ACK") \
    X(authenticate, 10, "RP_AUTHENTICATE") \
    X(auth_result, 11, "RP_AUTH_RESULT") \
    X(create_state, 20, "RP_CREATE_STATE") \
    X(delete_state, 21, "RP_DELETE_STATE") \
    X(enable_sync_drawing, 22, "RP_ENABLE_SYNC_DRAWING") \
    X(disable_sync_drawing, 23, "RP_DISABLE_SYNC_DRAWING") \
    X(invalidate_rect, 24, "RP_INVALIDATE_RECT") \
    X(invalidate_region, 25, "RP_INVALIDATE_REGION") \
    X(set_offsets, 40, "RP_SET_OFFSETS") \
    X(set_high_color, 41, "RP_SET_HIGH_COLOR") \
    X(set_low_color, 42, "RP_SET_LOW_COLOR") \
    X(set_pen_size, 43, "RP_SET_PEN_SIZE") \
    X(set_stroke_mode, 44, "RP_SET_STROKE_MODE") \
    X(set_blending_mode, 45, "RP_SET_BLENDING_MODE") \
    X(set_pattern, 46, "RP_SET_PATTERN") \
    X(set_drawing_mode, 47, "RP_SET_DRAWING_MODE") \
    X(set_font, 48, "RP_SET_FONT") \
    X(set_transform, 49, "RP_SET_TRANSFORM") \
    X(constrain_clipping_region, 60, "RP_CONSTRAIN_CLIPPING_REGION") \
    X(copy_rect_no_clipping, 61, "RP_COPY_RECT_NO_CLIPPING") \
    X(invert_rect, 62, "RP_INVERT_RECT") \
    X(draw_bitmap, 63, "RP_DRAW_BITMAP") \
    X(draw_bitmap_rects, 64, "RP_DRAW_BITMAP_RECTS") \
    X(stroke_arc, 80, "RP_STROKE_ARC") \
    X(stroke_bezier, 81, "RP_STROKE_BEZIER") \
    X(stroke_ellipse, 82, "RP_STROKE_ELLIPSE") \
    X(stroke_polygon, 83, "RP_STROKE_POLYGON") \
    X(stroke_rect, 84, "RP_STROKE_RECT") \
    X(stroke_round_rect, 85, "RP_STROKE_ROUND_RECT") \
    X(stroke_shape, 86, "RP_STROKE_SHAPE") \
    X(stroke_triangle, 87, "RP_STROKE_TRIANGLE") \
    X(stroke_line, 88, "RP_STROKE_LINE") \
    X(stroke_line_array, 89, "RP_STROKE_LINE_ARRAY") \
    X(fill_arc, 100, "RP_FILL_ARC") \
    X(fill_bezier, 101, "RP_FILL_BEZIER") \
    X(fill_ellipse, 102, "RP_FILL_ELLIPSE") \
    X(fill_polygon, 103, "RP_FILL_POLYGON") \
    X(fill_rect, 104, "RP_FILL_RECT") \
    X(fill_round_rect, 105, "RP_FILL_ROUND_RECT") \
    X(fill_shape, 106, "RP_FILL_SHAPE") \
    X(fill_triangle, 107, "RP_FILL_TRIANGLE") \
    X(fill_region, 108, "RP_FILL_REGION") \
    X(fill_arc_gradient, 120, "RP_FILL_ARC_GRADIENT") \
    X(fill_bezier_gradient, 121, "RP_FILL_BEZIER_GRADIENT") \
    X(fill_ellipse_gradient, 122, "RP_FILL_ELLIPSE_GRADIENT") \
    X(fill_polygon_gradient, 123, "RP_FILL_POLYGON_GRADIENT") \
    X(fill_rect_gradient, 124, "RP_FILL_RECT_GRADIENT") \
    X(fill_round_rect_gradient, 125, "RP_FILL_ROUND_RECT_GRADIENT") \
    X(fill_shape_gradient, 126, "RP_FILL_SHAPE_GRADIENT") \
    X(fill_triangle_gradient, 127, "RP_FILL_TRIANGLE_GRADIENT") \
    X(fill_region_gradient, 128, "RP_FILL_REGION_GRADIENT") \
    X(stroke_point_color, 140, "RP_STROKE_POINT_COLOR") \
    X(stroke_line_1px_color, 141, "RP_STROKE_LINE_1PX_COLOR") \
    X(stroke_rect_1px_color, 142, "RP_STROKE_RECT_1PX_COLOR") \
    X(fill_rect_color, 160, "RP_FILL_RECT_COLOR") \
    X(fill_region_color_no_clipping, 161, "RP_FILL_REGION_COLOR_NO_CLIPPING") \
    X(draw_string, 180, "RP_DRAW_STRING") \
    X(draw_string_with_offsets, 181, "RP_DRAW_STRING_WITH_OFFSETS") \
    X(draw_string_result, 182, "RP_DRAW_STRING_RESULT") \
    X(string_width, 183, "RP_STRING_WIDTH") \
    X(string_width_result, 184, "RP_STRING_WIDTH_RESULT") \
    X(read_bitmap, 185, "RP_READ_BITMAP") \
    X(read_bitmap_result, 186, "RP_READ_BITMAP_RESULT") \
    X(set_cursor, 200, "RP_SET_CURSOR") \
    X(set_cursor_visible, 201, "RP_SET_CURSOR_VISIBLE") \
    X(move_cursor_to, 202, "RP_MOVE_CURSOR_TO") \
    X(mouse_moved, 220, "RP_MOUSE_MOVED") \
    X(mouse_down, 221, "RP_MOUSE_DOWN") \
    X(mouse_up, 222, "RP_MOUSE_UP") \
    X(mouse_wheel_changed, 223, "RP_MOUSE_WHEEL_CHANGED") \
    X(key_down, 240, "RP_KEY_DOWN") \
    X(key_up, 241, "RP_KEY_UP") \
    X(unmapped_key_down, 242, "RP_UNMAPPED_KEY_DOWN") \
    X(unmapped_key_up, 243, "RP_UNMAPPED_KEY_UP") \
    X(modifiers_changed, 244, "RP_MODIFIERS_CHANGED") \
    X(stroke_arc_gradient, 260, "RP_STROKE_ARC_GRADIENT") \
    X(stroke_bezier_gradient, 261, "RP_STROKE_BEZIER_GRADIENT") \
    X(stroke_ellipse_gradient, 262, "RP_STROKE_ELLIPSE_GRADIENT") \
    X(stroke_polygon_gradient, 263, "RP_STROKE_POLYGON_GRADIENT") \
    X(stroke_rect_gradient, 264, "RP_STROKE_RECT_GRADIENT") \
    X(stroke_round_rect_gradient, 265, "RP_STROKE_ROUND_RECT_GRADIENT") \
    X(stroke_shape_gradient, 266, "RP_STROKE_SHAPE_GRADIENT") \
    X(stroke_triangle_gradient, 267, "RP_STROKE_TRIANGLE_GRADIENT") \
    X(stroke_line_gradient, 268, "RP_STROKE_LINE_GRADIENT") \
    X(tier_begin_frame, 280, "RP_TIER_BEGIN_FRAME") \
    X(codec_tile, 281, "RP_CODEC_TILE") \
    X(tier_end_frame, 282, "RP_TIER_END_FRAME") \
    X(audio_packet, 283, "RP_AUDIO_PACKET") \
    X(frame_ack, 284, "RP_FRAME_ACK")

// Op 10/11 are the broker's transport-security preamble, spoken by the
// WebSocket transport before the session starts and never seen by Session.
// Ops 220-244 travel client -> server (input events). Ops 280-284 are the
// reserved Tier P block, which no server sends yet; they are named so that the
// day one does, the log says so instead of "RP_UNKNOWN".
enum class Op : std::uint16_t {
#define HAIKU_REMOTE_OP_ENUMERATOR(name, value, wire_name) name = value,
    HAIKU_REMOTE_OP_TABLE(HAIKU_REMOTE_OP_ENUMERATOR)
#undef HAIKU_REMOTE_OP_ENUMERATOR
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

// Splits the server's byte stream into messages. A frame is `uint16 code`,
// `uint32 size`, payload -- where size counts the 6 byte header too.
//
// There is no frame delimiter on the wire, so once a declared size is wrong the
// offset of the next real frame is unknowable. This class therefore does not
// pretend to resynchronise: a size outside [message_header_size,
// max_message_size] is a fatal, reported protocol error. The offending code, the
// size it declared and where in the stream it sat all go into the exception
// message, and the decision latches -- the buffer is released and every later
// feed() rethrows the same diagnostic, so a hostile peer cannot make the client
// keep buffering, or re-parsing, a stream it has already declared unusable.
class Framer {
public:
    std::vector<Message> feed(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::size_t pending_bytes() const { return buffer_.size(); }
    // True once a bad frame header has ended framing for good.
    [[nodiscard]] bool failed() const { return !failure_.empty(); }
    // Byte offset, within the whole stream, of the next frame to be decoded.
    [[nodiscard]] std::uint64_t stream_offset() const { return stream_offset_; }

private:
    static constexpr std::size_t max_message_size = 64 * 1024 * 1024;
    std::vector<std::uint8_t> buffer_;
    std::string failure_;
    std::uint64_t stream_offset_ = 0;

    [[noreturn]] void fail(std::string description);
};

// Wire name of an opcode, e.g. "RP_FILL_POLYGON". A code the protocol does not
// define is reported with its value, "RP_UNKNOWN(4660)", because a bare
// "RP_UNKNOWN" identifies nothing. Returns by value for that reason: the
// unknown form has no static storage to hand out a view of.
[[nodiscard]] std::string op_name(Op op);
[[nodiscard]] bool is_session_level(Op op);

// Every opcode the table defines, in table order.
[[nodiscard]] std::span<const Op> all_ops();

} // namespace haiku_remote
