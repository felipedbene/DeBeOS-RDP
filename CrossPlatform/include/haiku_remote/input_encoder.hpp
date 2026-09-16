#pragma once

#include "haiku_remote/protocol.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace haiku_remote {

namespace modifiers {
constexpr std::uint32_t shift = 0x00000001;
constexpr std::uint32_t command = 0x00000002;
constexpr std::uint32_t control = 0x00000004;
constexpr std::uint32_t caps_lock = 0x00000008;
constexpr std::uint32_t scroll_lock = 0x00000010;
constexpr std::uint32_t num_lock = 0x00000020;
constexpr std::uint32_t option = 0x00000040;
constexpr std::uint32_t menu = 0x00000080;
constexpr std::uint32_t left_shift = 0x00000100;
constexpr std::uint32_t right_shift = 0x00000200;
constexpr std::uint32_t left_command = 0x00000400;
constexpr std::uint32_t right_command = 0x00000800;
constexpr std::uint32_t left_control = 0x00001000;
constexpr std::uint32_t right_control = 0x00002000;
constexpr std::uint32_t left_option = 0x00004000;
constexpr std::uint32_t right_option = 0x00008000;
} // namespace modifiers

namespace buttons {
constexpr std::uint32_t primary = 1;
constexpr std::uint32_t secondary = 2;
constexpr std::uint32_t tertiary = 4;
} // namespace buttons

class InputEncoder {
public:
    static std::vector<std::uint8_t> mouse_moved(float x, float y);
    static std::vector<std::uint8_t> mouse_down(
        float x, float y, std::uint32_t pressed_buttons, std::int32_t clicks);
    static std::vector<std::uint8_t> mouse_up(
        float x, float y, std::uint32_t pressed_buttons);
    static std::vector<std::uint8_t> mouse_wheel(float delta_x, float delta_y);
    static std::vector<std::uint8_t> key(
        bool down, std::string_view composed_utf8, std::int32_t raw_character,
        std::int32_t haiku_key);
    static std::vector<std::uint8_t> modifiers_changed(std::uint32_t value);
};

} // namespace haiku_remote
