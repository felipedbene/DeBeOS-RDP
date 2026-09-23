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

void Framer::fail(std::string description)
{
    failure_ = std::move(description);
    // Nothing after the bad header can be trusted, so stop holding it: an
    // attacker must not be able to pin 64 MiB in a client that has already
    // given up on the stream.
    buffer_.clear();
    buffer_.shrink_to_fit();
    throw ProtocolError(failure_);
}

std::vector<Message> Framer::feed(std::span<const std::uint8_t> bytes)
{
    // A caller that keeps reading from the socket after the desync gets the
    // original diagnostic again, not a fresh guess at the same broken bytes.
    if (failed())
        throw ProtocolError(failure_);
    if (bytes.size() > max_message_size - std::min(buffer_.size(), max_message_size)) {
        fail("pending protocol data exceeds the "
            + std::to_string(max_message_size) + " byte safety limit ("
            + std::to_string(buffer_.size()) + " buffered, "
            + std::to_string(bytes.size()) + " more offered at stream offset "
            + std::to_string(stream_offset_) + ")");
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    std::vector<Message> messages;
    std::size_t offset = 0;
    while (buffer_.size() - offset >= message_header_size) {
        const auto op = static_cast<Op>(read_u16_le(buffer_.data() + offset));
        const auto total = static_cast<std::size_t>(
            read_u32_le(buffer_.data() + offset + 2));
        // Both bounds are checked before the payload is copied, so no
        // allocation is ever sized from an unvalidated declared length.
        if (total < message_header_size || total > max_message_size) {
            const auto where = stream_offset_ + offset;
            fail("framing desync in " + op_name(op) + ": declared frame size "
                + std::to_string(total) + " is "
                + (total < message_header_size
                    ? "smaller than the " + std::to_string(message_header_size)
                        + " byte frame header"
                    : "beyond the " + std::to_string(max_message_size)
                        + " byte limit")
                + " (at stream offset " + std::to_string(where)
                + "); a byte stream has no frame delimiter, so the next frame"
                  " boundary is unknowable and the session cannot continue");
        }
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
    if (offset != 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
        stream_offset_ += offset;
    }
    return messages;
}

bool is_session_level(Op op)
{
    switch (op) {
    case Op::init_connection:
    case Op::close_connection:
    case Op::hello_ack:
    case Op::resync:
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

std::string op_name(Op op)
{
    switch (op) {
#define HAIKU_REMOTE_OP_CASE(name, value, wire_name) \
    case Op::name: return wire_name;
    HAIKU_REMOTE_OP_TABLE(HAIKU_REMOTE_OP_CASE)
#undef HAIKU_REMOTE_OP_CASE
    }
    // Deliberately no default arm above: -Wswitch then makes an Op enumerator
    // without a name a build diagnostic rather than a lie in the log. Falling
    // out of the switch means a code the protocol does not define at all, which
    // is only ever useful with its number attached.
    return "RP_UNKNOWN(" + std::to_string(static_cast<std::uint16_t>(op)) + ")";
}

std::span<const Op> all_ops()
{
    static constexpr Op ops[] = {
#define HAIKU_REMOTE_OP_VALUE(name, value, wire_name) Op::name,
        HAIKU_REMOTE_OP_TABLE(HAIKU_REMOTE_OP_VALUE)
#undef HAIKU_REMOTE_OP_VALUE
    };
    return ops;
}

} // namespace haiku_remote
