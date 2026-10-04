#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "haiku_remote/application_state.hpp"
#include "haiku_remote/connection_coordinator.hpp"
#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/launch_options.hpp"
#include "haiku_remote/profile_store.hpp"
#include "haiku_remote/ssh_tunnel.hpp"
#include "haiku_remote/surface.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace haiku_remote;

namespace {

std::uint32_t modifier_mask(SDL_Keymod state)
{
    std::uint32_t result = 0;
    if ((state & KMOD_LSHIFT) != 0)
        result |= modifiers::shift | modifiers::left_shift;
    if ((state & KMOD_RSHIFT) != 0)
        result |= modifiers::shift | modifiers::right_shift;
    if ((state & KMOD_LCTRL) != 0)
        result |= modifiers::control | modifiers::left_control;
    if ((state & KMOD_RCTRL) != 0)
        result |= modifiers::control | modifiers::right_control;
    if ((state & KMOD_LALT) != 0)
        result |= modifiers::command | modifiers::left_command;
    if ((state & KMOD_RALT) != 0)
        result |= modifiers::command | modifiers::right_command;
    if ((state & KMOD_LGUI) != 0)
        result |= modifiers::option | modifiers::left_option;
    if ((state & KMOD_RGUI) != 0)
        result |= modifiers::option | modifiers::right_option;
    if ((state & KMOD_CAPS) != 0)
        result |= modifiers::caps_lock;
    if ((state & KMOD_NUM) != 0)
        result |= modifiers::num_lock;
    return result;
}

std::uint32_t button_mask(std::uint32_t state)
{
    std::uint32_t result = 0;
    if ((state & SDL_BUTTON_LMASK) != 0) result |= buttons::primary;
    if ((state & SDL_BUTTON_RMASK) != 0) result |= buttons::secondary;
    if ((state & SDL_BUTTON_MMASK) != 0) result |= buttons::tertiary;
    return result;
}

std::int32_t haiku_key(SDL_Scancode key)
{
    switch (key) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_F1: return 0x02;
    case SDL_SCANCODE_F2: return 0x03;
    case SDL_SCANCODE_F3: return 0x04;
    case SDL_SCANCODE_F4: return 0x05;
    case SDL_SCANCODE_F5: return 0x06;
    case SDL_SCANCODE_F6: return 0x07;
    case SDL_SCANCODE_F7: return 0x08;
    case SDL_SCANCODE_F8: return 0x09;
    case SDL_SCANCODE_F9: return 0x0a;
    case SDL_SCANCODE_F10: return 0x0b;
    case SDL_SCANCODE_F11: return 0x0c;
    case SDL_SCANCODE_F12: return 0x0d;
    case SDL_SCANCODE_BACKSPACE: return 0x1e;
    case SDL_SCANCODE_INSERT: return 0x1f;
    case SDL_SCANCODE_HOME: return 0x20;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_TAB: return 0x26;
    case SDL_SCANCODE_DELETE: return 0x34;
    case SDL_SCANCODE_END: return 0x35;
    case SDL_SCANCODE_PAGEDOWN: return 0x36;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return 0x47;
    case SDL_SCANCODE_UP: return 0x57;
    case SDL_SCANCODE_SPACE: return 0x5e;
    case SDL_SCANCODE_LEFT: return 0x61;
    case SDL_SCANCODE_DOWN: return 0x62;
    case SDL_SCANCODE_RIGHT: return 0x63;
    default: return 0;
    }
}

std::string_view special_text(SDL_Keycode key)
{
    switch (key) {
    case SDLK_HOME: return std::string_view("\x01", 1);
    case SDLK_END: return std::string_view("\x04", 1);
    case SDLK_INSERT: return std::string_view("\x05", 1);
    case SDLK_BACKSPACE: return std::string_view("\x08", 1);
    case SDLK_TAB: return std::string_view("\x09", 1);
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return std::string_view("\x0a", 1);
    case SDLK_PAGEUP: return std::string_view("\x0b", 1);
    case SDLK_PAGEDOWN: return std::string_view("\x0c", 1);
    case SDLK_ESCAPE: return std::string_view("\x1b", 1);
    case SDLK_LEFT: return std::string_view("\x1c", 1);
    case SDLK_RIGHT: return std::string_view("\x1d", 1);
    case SDLK_UP: return std::string_view("\x1e", 1);
    case SDLK_DOWN: return std::string_view("\x1f", 1);
    case SDLK_DELETE: return std::string_view("\x7f", 1);
    default: return {};
    }
}

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
        const auto byte = static_cast<unsigned char>(
            text[static_cast<std::size_t>(i)]);
        if ((byte & 0xc0) != 0x80)
            return 0;
        value = (value << 6) | (byte & 0x3f);
    }
    return static_cast<std::int32_t>(value);
}

bool is_text_key(SDL_Keycode key)
{
    return key >= 0x20 && key != SDLK_DELETE
        && (key & SDLK_SCANCODE_MASK) == 0;
}

// Resolve a saved profile named by --profile (id first, then name) out of the
// library. Throws when nothing matches, so a typo is a usage error rather than
// a silent connect to the wrong machine.
ConnectionProfile resolve_saved_profile(const std::string& reference)
{
    const ProfileStore store;
    const LoadResult library = store.load();
    for (const auto& profile : library.profiles)
        if (profile.id == reference)
            return profile;
    for (const auto& profile : library.profiles)
        if (profile.name == reference)
            return profile;
    throw std::runtime_error("no saved profile matches --profile " + reference);
}

// PR3 drives ONE profile straight into the desktop view -- there is no library
// UI yet. With no connection flag we default to the local app_server port, which
// is also where tools/rp_mock_server.py listens, so `haiku-remote-gui` with no
// arguments connects to a local mock the same way the old client did.
CoordinatorConfig config_for(const LaunchOptions& options)
{
    LaunchOptions resolved = options;
    if (!resolved.profile_ref.empty()) {
        resolved.profile = resolve_saved_profile(resolved.profile_ref);
    } else if (resolved.open_library) {
        ConnectionProfile fallback;
        fallback.name = "localhost";
        fallback.mode = ConnectionMode::direct;
        fallback.host = "127.0.0.1";
        fallback.remote_port = 10900;
        fallback.width = resolved.width;
        fallback.height = resolved.height;
        resolved.profile = fallback;
    }
    // Reconnect follows the profile's own preference unless a flag forced it.
    if (!resolved.reconnect.enabled && resolved.profile.auto_reconnect)
        resolved.reconnect.enabled = true;
    return coordinator_config_from(resolved);
}

} // namespace

int main(int argc, char** argv)
{
    LaunchResult launch = parse_launch_options(argc, argv);
    if (launch.show_help) {
        std::cout << "Usage: haiku-remote-gui" << launch_usage() << "\n\n"
                  << exit_status_usage();
        return exit_status::ok;
    }
    if (!launch.ok) {
        std::cerr << launch.error << '\n';
        return launch.exit_code;
    }

    // Reap any verified-ours orphaned ssh tunnel left by an earlier crashed run,
    // at launch and before any connect (the tunnel also reaps before each
    // connect). An unverifiable pid is never signalled -- only its stale pidfile
    // is unlinked -- so a recycled pid can never make us kill a stranger.
    reap_orphans(SshTunnel::default_pidfile_dir());

    CoordinatorConfig config;
    try {
        config = config_for(launch.options);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return exit_status::usage;
    }

    try {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
            throw std::runtime_error(SDL_GetError());

        ConnectionCoordinator coordinator(config);
        const int width = coordinator.width();
        const int height = coordinator.height();

        SDL_Window* window = SDL_CreateWindow(
            "Haiku Remote", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            width, height,
            SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (window == nullptr)
            throw std::runtime_error(SDL_GetError());
        SDL_Renderer* renderer = SDL_CreateRenderer(
            window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (renderer == nullptr)
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (renderer == nullptr)
            throw std::runtime_error(SDL_GetError());
        SDL_RenderSetLogicalSize(renderer, width, height);
        SDL_Texture* texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            width, height);
        if (texture == nullptr)
            throw std::runtime_error(SDL_GetError());

        coordinator.connect();

        // The UI thread never touches the Session or its Surface: it reads the
        // mutex-guarded Snapshot and copies out the frame the worker hands it.
        Surface frame(width, height);
        std::uint64_t shown_generation = 0;
        std::uint32_t last_modifiers = 0;
        CoordinatorState last_state = CoordinatorState::idle;
        bool running = true;
        bool user_quit = false;

        const auto post = [&](std::vector<std::uint8_t> message) {
            coordinator.post_input(std::move(message));
        };
        const auto update_modifiers = [&]() {
            const auto current = modifier_mask(SDL_GetModState());
            if (current != last_modifiers) {
                last_modifiers = current;
                post(InputEncoder::modifiers_changed(current));
            }
        };

        SDL_StartTextInput();
        while (running) {
            const Snapshot snap = coordinator.snapshot();
            if (snap.state != last_state) {
                last_state = snap.state;
                std::string title = "Haiku Remote - ";
                title += coordinator_state_name(snap.state);
                if (snap.reason != ConnectionReason::none) {
                    title += " (";
                    title += reason_code(snap.reason);
                    title += ")";
                }
                SDL_SetWindowTitle(window, title.c_str());
            }
            // The worker finished: a clean disconnect exits ok, a failure prints
            // its actionable reason and exits non-zero.
            if (snap.finished) {
                if (snap.state == CoordinatorState::failed) {
                    std::cerr << "connection failed (" << reason_code(snap.reason)
                              << "): " << snap.message << '\n';
                }
                break;
            }

            SDL_Event event {};
            while (SDL_PollEvent(&event) != 0) {
                switch (event.type) {
                case SDL_QUIT:
                    user_quit = true;
                    running = false;
                    break;
                case SDL_MOUSEMOTION:
                    post(InputEncoder::mouse_moved(
                        static_cast<float>(event.motion.x),
                        static_cast<float>(event.motion.y)));
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP: {
                    int x = 0;
                    int y = 0;
                    const auto state = SDL_GetMouseState(&x, &y);
                    if (event.type == SDL_MOUSEBUTTONDOWN) {
                        post(InputEncoder::mouse_down(
                            static_cast<float>(x), static_cast<float>(y),
                            button_mask(state),
                            std::max<int>(1, event.button.clicks)));
                    } else {
                        post(InputEncoder::mouse_up(
                            static_cast<float>(x), static_cast<float>(y),
                            button_mask(state)));
                    }
                    break;
                }
                case SDL_MOUSEWHEEL:
                    post(InputEncoder::mouse_wheel(
                        -static_cast<float>(event.wheel.preciseX),
                        -static_cast<float>(event.wheel.preciseY)));
                    break;
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    update_modifiers();
                    const bool down = event.type == SDL_KEYDOWN;
                    const auto special = special_text(event.key.keysym.sym);
                    if (!special.empty()) {
                        post(InputEncoder::key(
                            down, special, first_utf8_scalar(special),
                            haiku_key(event.key.keysym.scancode)));
                    } else if (!is_text_key(event.key.keysym.sym)) {
                        post(InputEncoder::key(
                            down, {}, 0, haiku_key(event.key.keysym.scancode)));
                    }
                    break;
                }
                case SDL_TEXTINPUT: {
                    const std::string_view text(event.text.text);
                    const auto raw = first_utf8_scalar(text);
                    post(InputEncoder::key(true, text, raw, 0));
                    post(InputEncoder::key(false, text, raw, 0));
                    break;
                }
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_EXPOSED)
                        shown_generation = 0; // force a re-upload
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        last_modifiers = 0;
                        post(InputEncoder::modifiers_changed(0));
                    }
                    break;
                default:
                    break;
                }
            }

            // Re-upload only when the worker produced a newer frame.
            if (snap.surface_generation != shown_generation) {
                const std::uint64_t copied = coordinator.copy_frame(frame);
                if (copied != 0) {
                    shown_generation = copied;
                    if (SDL_UpdateTexture(texture, nullptr,
                                          frame.pixels().data(),
                                          frame.stride()) != 0) {
                        throw std::runtime_error(SDL_GetError());
                    }
                }
            }
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        SDL_StopTextInput();

        coordinator.disconnect();
        const Snapshot final_snap = coordinator.snapshot();

        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();

        // A user-initiated quit is a success; a connection that failed on its
        // own is not.
        if (!user_quit && final_snap.state == CoordinatorState::failed)
            return exit_status::failed;
        return exit_status::ok;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        SDL_Quit();
        return exit_status::usage;
    }
}
