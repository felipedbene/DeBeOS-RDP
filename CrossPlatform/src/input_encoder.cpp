#include "haiku_remote/input_encoder.hpp"

#include <algorithm>

namespace haiku_remote {

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

} // namespace haiku_remote
