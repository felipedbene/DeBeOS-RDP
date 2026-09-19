#include "haiku_remote/protocol.hpp"

#include <bit>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace haiku_remote {
namespace {

std::uint16_t read_u16_le(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0])
        | static_cast<std::uint16_t>(p[1]) << 8;
}

std::uint32_t read_u32_le(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0])
        | static_cast<std::uint32_t>(p[1]) << 8
        | static_cast<std::uint32_t>(p[2]) << 16
        | static_cast<std::uint32_t>(p[3]) << 24;
}

std::uint64_t read_u64_le(const std::uint8_t* p)
{
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(p[i]) << (i * 8);
    return value;
}

} // namespace

Reader::Reader(std::span<const std::uint8_t> bytes)
    : bytes_(bytes)
{
}

std::size_t Reader::remaining() const
{
    return bytes_.size() - offset_;
}

void Reader::require(std::size_t count) const
{
    if (count > remaining())
        throw ProtocolError("message payload is truncated");
}

std::uint8_t Reader::u8()
{
    require(1);
    return bytes_[offset_++];
}

bool Reader::boolean()
{
    return u8() != 0;
}

std::uint16_t Reader::u16()
{
    require(2);
    const auto value = read_u16_le(bytes_.data() + offset_);
    offset_ += 2;
    return value;
}

std::uint32_t Reader::u32()
{
    require(4);
    const auto value = read_u32_le(bytes_.data() + offset_);
    offset_ += 4;
    return value;
}

std::int32_t Reader::i32()
{
    return std::bit_cast<std::int32_t>(u32());
}

float Reader::f32()
{
    return std::bit_cast<float>(u32());
}

double Reader::f64()
{
    require(8);
    const auto bits = read_u64_le(bytes_.data() + offset_);
    offset_ += 8;
    return std::bit_cast<double>(bits);
}

Point Reader::point()
{
    return {f32(), f32()};
}

Rect Reader::rect()
{
    return {f32(), f32(), f32(), f32()};
}

Color Reader::color()
{
    return {u8(), u8(), u8(), u8()};
}

Font Reader::font()
{
    Font result;
    result.direction = u8();
    result.encoding = u8();
    result.flags = u32();
    result.spacing = u8();
    result.shear = f32();
    result.rotation = f32();
    result.false_bold_width = f32();
    result.size = f32();
    result.face = u16();
    const auto family_and_style = u32();
    result.family = static_cast<std::uint16_t>(family_and_style >> 16);
    result.style = static_cast<std::uint16_t>(family_and_style & 0xffff);
    return result;
}

Transform Reader::transform()
{
    if (boolean())
        return {};
    return {f64(), f64(), f64(), f64(), f64(), f64()};
}

Gradient Reader::gradient()
{
    Gradient result;
    const auto raw_kind = u32();
    result.kind = raw_kind <= static_cast<std::uint32_t>(Gradient::Kind::none)
        ? static_cast<Gradient::Kind>(raw_kind)
        : Gradient::Kind::none;
    switch (result.kind) {
    case Gradient::Kind::linear:
        result.start = point();
        result.end = point();
        break;
    case Gradient::Kind::radial:
        result.center = point();
        result.radius = f32();
        break;
    case Gradient::Kind::radial_focus:
        result.center = point();
        result.focal = point();
        result.radius = f32();
        break;
    case Gradient::Kind::diamond:
        result.center = point();
        break;
    case Gradient::Kind::conic:
        result.center = point();
        result.angle = f32();
        break;
    case Gradient::Kind::none:
        break;
    }
    const auto count = i32();
    if (count < 0 || count > (1 << 16))
        throw ProtocolError("invalid gradient stop count");
    result.stops.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i)
        result.stops.push_back({color(), f32()});
    return result;
}

std::vector<Rect> Reader::region()
{
    const auto count = i32();
    if (count < 0 || count > (1 << 20))
        throw ProtocolError("invalid region rectangle count");
    std::vector<Rect> result;
    result.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i)
        result.push_back(rect());
    return result;
}

std::string Reader::string()
{
    const auto count = u32();
    const auto bytes = raw(count);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::vector<std::uint8_t> Reader::raw(std::size_t count)
{
    require(count);
    std::vector<std::uint8_t> result(
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
    offset_ += count;
    return result;
}

Writer::Writer(Op op)
{
    bytes_.reserve(64);
    u16(static_cast<std::uint16_t>(op));
    u32(0);
}

void Writer::u8(std::uint8_t value)
{
    bytes_.push_back(value);
}

void Writer::boolean(bool value)
{
    u8(value ? 1 : 0);
}

void Writer::u16(std::uint16_t value)
{
    bytes_.push_back(static_cast<std::uint8_t>(value));
    bytes_.push_back(static_cast<std::uint8_t>(value >> 8));
}

void Writer::u32(std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        bytes_.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

void Writer::i32(std::int32_t value)
{
    u32(std::bit_cast<std::uint32_t>(value));
}

void Writer::f32(float value)
{
    u32(std::bit_cast<std::uint32_t>(value));
}

void Writer::point(Point value)
{
    f32(value.x);
    f32(value.y);
}

void Writer::string(std::string_view value)
{
    u32(static_cast<std::uint32_t>(value.size()));
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
    raw(std::span(bytes, value.size()));
}

void Writer::raw(std::span<const std::uint8_t> value)
{
    bytes_.insert(bytes_.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> Writer::finish()
{
    const auto size = static_cast<std::uint32_t>(bytes_.size());
    for (int i = 0; i < 4; ++i)
        bytes_[2 + i] = static_cast<std::uint8_t>(size >> (i * 8));
    return bytes_;
}

std::vector<Message> Framer::feed(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > max_message_size - std::min(buffer_.size(), max_message_size))
        throw ProtocolError("pending protocol data exceeds the 64 MiB safety limit");
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    std::vector<Message> messages;
    std::size_t offset = 0;
    while (buffer_.size() - offset >= message_header_size) {
        const auto op = static_cast<Op>(read_u16_le(buffer_.data() + offset));
        const auto total = static_cast<std::size_t>(
            read_u32_le(buffer_.data() + offset + 2));
        if (total < message_header_size)
            throw ProtocolError("message length is smaller than its header");
        if (total > max_message_size)
            throw ProtocolError("message exceeds the 64 MiB safety limit");
        if (buffer_.size() - offset < total)
            break;
        messages.push_back({
            op,
            std::vector<std::uint8_t>(
                buffer_.begin() + static_cast<std::ptrdiff_t>(offset + message_header_size),
                buffer_.begin() + static_cast<std::ptrdiff_t>(offset + total)),
        });
        offset += total;
    }
    if (offset != 0)
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
    return messages;
}

void Framer::reset()
{
    buffer_.clear();
}

bool is_session_level(Op op)
{
    switch (op) {
    case Op::init_connection:
    case Op::close_connection:
    case Op::hello_ack:
    case Op::get_system_palette_result:
    case Op::create_state:
    case Op::delete_state:
    case Op::invalidate_rect:
    case Op::invalidate_region:
    case Op::copy_rect_no_clipping:
    case Op::fill_region_color_no_clipping:
    case Op::set_cursor:
    case Op::set_cursor_visible:
    case Op::move_cursor_to:
        return true;
    default:
        return false;
    }
}

std::string_view op_name(Op op)
{
    switch (op) {
    case Op::init_connection: return "RP_INIT_CONNECTION";
    case Op::update_display_mode: return "RP_UPDATE_DISPLAY_MODE";
    case Op::close_connection: return "RP_CLOSE_CONNECTION";
    case Op::get_system_palette: return "RP_GET_SYSTEM_PALETTE";
    case Op::get_system_palette_result: return "RP_GET_SYSTEM_PALETTE_RESULT";
    case Op::hello: return "RP_HELLO";
    case Op::hello_ack: return "RP_HELLO_ACK";
    case Op::create_state: return "RP_CREATE_STATE";
    case Op::delete_state: return "RP_DELETE_STATE";
    case Op::fill_rect: return "RP_FILL_RECT";
    case Op::fill_rect_color: return "RP_FILL_RECT_COLOR";
    case Op::fill_region_color_no_clipping: return "RP_FILL_REGION_COLOR_NO_CLIPPING";
    case Op::draw_bitmap: return "RP_DRAW_BITMAP";
    case Op::draw_string: return "RP_DRAW_STRING";
    case Op::string_width: return "RP_STRING_WIDTH";
    case Op::fill_arc: return "RP_FILL_ARC";
    case Op::stroke_arc: return "RP_STROKE_ARC";
    case Op::stroke_rect: return "RP_STROKE_RECT";
    case Op::stroke_round_rect: return "RP_STROKE_ROUND_RECT";
    case Op::stroke_shape: return "RP_STROKE_SHAPE";
    case Op::stroke_triangle: return "RP_STROKE_TRIANGLE";
    case Op::stroke_line_array: return "RP_STROKE_LINE_ARRAY";
    case Op::fill_round_rect: return "RP_FILL_ROUND_RECT";
    case Op::fill_shape: return "RP_FILL_SHAPE";
    case Op::fill_triangle: return "RP_FILL_TRIANGLE";
    case Op::fill_rect_gradient: return "RP_FILL_RECT_GRADIENT";
    case Op::read_bitmap: return "RP_READ_BITMAP";
    default: return "RP_UNKNOWN";
    }
}

} // namespace haiku_remote
