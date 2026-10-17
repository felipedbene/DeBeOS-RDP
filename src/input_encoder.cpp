#include "haiku_remote/input_encoder.hpp"

#include <algorithm>

namespace haiku_remote {

std::int32_t first_utf8_scalar(std::string_view text)
{
    if (text.empty())
        return 0;
    const auto first = static_cast<unsigned char>(text[0]);
    if (first < 0x80)
        return first;
    int count = 0;
    std::uint32_t value = 0;
    if ((first & 0xe0) == 0xc0) {
        count = 2;
        value = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        count = 3;
        value = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        count = 4;
        value = first & 0x07;
    } else {
        return 0;
    }
    if (text.size() < static_cast<std::size_t>(count))
        return 0;
    for (int i = 1; i < count; ++i) {
        const auto byte = static_cast<unsigned char>(text[static_cast<std::size_t>(i)]);
        if ((byte & 0xc0) != 0x80)
            return 0;
        value = (value << 6) | (byte & 0x3f);
    }
    return static_cast<std::int32_t>(value);
}

std::vector<std::uint8_t> InputEncoder::mouse_moved(float x, float y)
{
    Writer writer(Op::mouse_moved);
    writer.f32(x);
    writer.f32(y);
    return writer.finish();
}

std::vector<std::uint8_t> InputEncoder::mouse_down(
    float x, float y, std::uint32_t pressed_buttons, std::int32_t clicks)
{
    Writer writer(Op::mouse_down);
    writer.f32(x);
    writer.f32(y);
    writer.i32(static_cast<std::int32_t>(pressed_buttons));
    writer.i32(std::max<std::int32_t>(1, clicks));
    return writer.finish();
}

std::vector<std::uint8_t> InputEncoder::mouse_up(
    float x, float y, std::uint32_t pressed_buttons)
{
    Writer writer(Op::mouse_up);
    writer.f32(x);
    writer.f32(y);
    writer.i32(static_cast<std::int32_t>(pressed_buttons));
    return writer.finish();
}

std::vector<std::uint8_t> InputEncoder::mouse_wheel(
    float delta_x, float delta_y)
{
    Writer writer(Op::mouse_wheel_changed);
    writer.f32(delta_x);
    writer.f32(delta_y);
    return writer.finish();
}

std::vector<std::uint8_t> InputEncoder::key(
    bool down, std::string_view composed_utf8, std::int32_t raw_character,
    std::int32_t haiku_key)
{
    Writer writer(down ? Op::key_down : Op::key_up);
    writer.string(composed_utf8);
    writer.i32(raw_character);
    writer.i32(haiku_key);
    return writer.finish();
}

std::vector<std::uint8_t> InputEncoder::modifiers_changed(std::uint32_t value)
{
    Writer writer(Op::modifiers_changed);
    writer.u32(value);
    return writer.finish();
}

std::vector<std::uint8_t> InputEncoder::secure_attention()
{
    return Writer(Op::secure_attention).finish();
}

} // namespace haiku_remote
