#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/library_screen.hpp"
#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/profile_library.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace haiku_remote;

namespace {

struct Options {
    TransportOptions transport;
    int width = 1280;
    int height = 800;
    bool size_explicit = false;
    // True once any connection-selecting argument was seen. When false, the
    // client opens the connection library instead of connecting straight away,
    // leaving the CLI launch path untouched.
    bool target_specified = false;
};

int parse_integer(std::string_view value, std::string_view name,
                  int minimum, int maximum)
{
    std::size_t parsed = 0;
    const long long number = std::stoll(std::string(value), &parsed);
    if (parsed != value.size() || number < minimum || number > maximum)
        throw std::runtime_error("invalid " + std::string(name) + ": "
                                 + std::string(value));
    return static_cast<int>(number);
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto value = [&]() -> std::string {
            if (++i >= argc)
                throw std::runtime_error("missing value for " + argument);
            return argv[i];
        };
        if (parse_transport_argument(options.transport, argument, value)) {
            options.target_specified = true;
        } else if (argument == "--width") {
            options.width = parse_integer(
                value(), "width", 1, Surface::max_dimension);
            options.size_explicit = true;
        } else if (argument == "--height") {
            options.height = parse_integer(
                value(), "height", 1, Surface::max_dimension);
            options.size_explicit = true;
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: haiku-remote-gui" << transport_usage()
                      << "\n  [--width PX] [--height PX]\n\n"
                      << exit_status_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }
    return options;
}

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

// Describe how the session ended and say whether that ending was a success.
// Returns true for an orderly end (exit 0), false for a failure (exit non-zero).
//
// Every post-connect ending used to collapse onto "receive failed: ..." and
// exit 0, which made three very different outcomes indistinguishable: a session
// that ran and was shut down, a transport fault, and a session the server
// refused before sending a single byte of drawing. The third is the one that
// costs time -- it presents as "the desktop is black" while the process reports
// success -- so it gets its own message and its own exit status.
bool report_session_end(bool orderly, std::size_t messages,
                        std::string_view reason)
{
    if (!orderly) {
        std::cerr << "receive failed: " << reason << '\n';
        return false;
    }
    if (messages == 0) {
        std::cerr << "the server closed the connection before sending any"
                     " drawing data: the session was refused or never started\n";
        return false;
    }
    std::cerr << "session ended: the server closed the connection after "
              << messages << " messages\n";
    return true;
}

} // namespace

// Run one connected remote-desktop session for already-resolved options. This
// is the client's original SDL behaviour verbatim, lifted into a function so
// both the CLI path and a library pick can drive it. `options` is taken by
// value because the display-size defaulting below mutates it.
int run_session(Options options)
{
    try {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
            throw std::runtime_error(SDL_GetError());
        // Default the remote desktop to the local display size so the window
        // does not come up larger than the screen (e.g. a 1280x800 default on a
        // 1024x768 panel). --width/--height still override.
        if (!options.size_explicit) {
            SDL_Rect bounds;
            if (SDL_GetDisplayUsableBounds(0, &bounds) == 0
                && bounds.w > 0 && bounds.h > 0) {
                options.width = std::min(bounds.w, Surface::max_dimension);
                options.height = std::min(bounds.h, Surface::max_dimension);
            }
        }

        SDL_Window* window = SDL_CreateWindow(
            "Haiku Remote", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            options.width, options.height,
            SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (window == nullptr)
            throw std::runtime_error(SDL_GetError());
        SDL_Renderer* renderer = SDL_CreateRenderer(
            window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (renderer == nullptr)
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (renderer == nullptr)
            throw std::runtime_error(SDL_GetError());
        SDL_RenderSetLogicalSize(renderer, options.width, options.height);
        SDL_Texture* texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            options.width, options.height);
        if (texture == nullptr)
            throw std::runtime_error(SDL_GetError());

        std::string socket_error;
        const auto transport = make_transport(options.transport, socket_error);
        if (transport == nullptr)
            throw std::runtime_error(socket_error);
        if (!transport->connect(socket_error)) {
            // Reported here rather than thrown: the catch below exits 2, which
            // is this client's code for bad arguments, so every connect failure
            // in this front end used to be indistinguishable from a typo -- and
            // from each other. See exit_status_usage().
            std::cerr << "connect to " << transport->describe() << " failed: "
                      << socket_error << '\n';
            const int status = connect_exit_status(*transport);
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return status;
        }
        Session session(
            options.width, options.height,
            [&](std::span<const std::uint8_t> bytes) {
                return transport->send_all(bytes, socket_error);
            },
            [](std::string_view line) { std::cerr << line << '\n'; });
        // Session::start() returns void and ignores whether the handshake went
        // out, so a connection that dies between connect() and the first write
        // otherwise looks like a session that simply received nothing. The send
        // callback only assigns socket_error on failure, so an empty string
        // after start() means both handshake messages were written.
        socket_error.clear();
        session.start();
        if (!socket_error.empty())
            throw std::runtime_error("handshake send to " + transport->describe()
                                     + " failed: " + socket_error);

        bool running = true;
        bool dirty = true;
        bool session_failed = false;
        std::uint32_t last_modifiers = 0;
        std::array<std::uint8_t, 256 * 1024> buffer {};
        const auto send = [&](std::vector<std::uint8_t> message) {
            if (!session.send_client_message(message)) {
                std::cerr << "send failed: " << socket_error << '\n';
                session_failed = true;
                running = false;
            }
        };
        const auto update_modifiers = [&]() {
            const auto current = modifier_mask(SDL_GetModState());
            if (current != last_modifiers) {
                last_modifiers = current;
                send(InputEncoder::modifiers_changed(current));
            }
        };

        SDL_StartTextInput();
        while (running) {
            const int received = transport->receive(buffer, 2, socket_error);
            if (received < 0) {
                session_failed = !report_session_end(
                    transport->peer_closed() || session.server_closed(),
                    session.message_count(), socket_error);
                break;
            }
            if (received > 0) {
                session.ingest(
                    std::span(buffer.data(), static_cast<std::size_t>(received)));
                dirty = true;
            }
            // RP_CLOSE_CONNECTION arrives before the EOF does. Stop on it
            // rather than idling against a socket that will never say anything
            // again; clearing `running` rather than breaking lets this last
            // iteration present the frame the close arrived with.
            if (session.server_closed() && running) {
                session_failed = !report_session_end(
                    true, session.message_count(), socket_error);
                running = false;
            }

            SDL_Event event {};
            while (SDL_PollEvent(&event) != 0) {
                switch (event.type) {
                case SDL_QUIT:
                    running = false;
                    break;
                case SDL_MOUSEMOTION:
                    send(InputEncoder::mouse_moved(
                        static_cast<float>(event.motion.x),
                        static_cast<float>(event.motion.y)));
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP: {
                    int x = 0;
                    int y = 0;
                    const auto state = SDL_GetMouseState(&x, &y);
                    if (event.type == SDL_MOUSEBUTTONDOWN) {
                        send(InputEncoder::mouse_down(
                            static_cast<float>(x), static_cast<float>(y),
                            button_mask(state),
                            std::max<int>(1, event.button.clicks)));
                    } else {
                        send(InputEncoder::mouse_up(
                            static_cast<float>(x), static_cast<float>(y),
                            button_mask(state)));
                    }
                    break;
                }
                case SDL_MOUSEWHEEL:
                    send(InputEncoder::mouse_wheel(
                        -static_cast<float>(event.wheel.preciseX),
                        -static_cast<float>(event.wheel.preciseY)));
                    break;
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    update_modifiers();
                    const bool down = event.type == SDL_KEYDOWN;
                    const auto special = special_text(event.key.keysym.sym);
                    if (!special.empty()) {
                        send(InputEncoder::key(
                            down, special, first_utf8_scalar(special),
                            haiku_key(event.key.keysym.scancode)));
                    } else if (!is_text_key(event.key.keysym.sym)) {
                        send(InputEncoder::key(
                            down, {}, 0, haiku_key(event.key.keysym.scancode)));
                    }
                    break;
                }
                case SDL_TEXTINPUT: {
                    const std::string_view text(event.text.text);
                    const auto raw = first_utf8_scalar(text);
                    send(InputEncoder::key(true, text, raw, 0));
                    send(InputEncoder::key(false, text, raw, 0));
                    break;
                }
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_EXPOSED)
                        dirty = true;
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        last_modifiers = 0;
                        send(InputEncoder::modifiers_changed(0));
                    }
                    break;
                default:
                    break;
                }
            }

            if (dirty) {
                const auto pixels = session.surface().pixels();
                if (SDL_UpdateTexture(
                        texture, nullptr, pixels.data(),
                        session.surface().stride()) != 0) {
                    throw std::runtime_error(SDL_GetError());
                }
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, nullptr, nullptr);
                SDL_RenderPresent(renderer);
                dirty = false;
            }
        }
        SDL_StopTextInput();
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        // A user-initiated quit (SDL_QUIT) leaves session_failed false and is
        // still a success; only the error and refused-session paths are not.
        return session_failed ? exit_status::failed : exit_status::ok;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        SDL_Quit();
        return exit_status::usage;
    }
}

// Show the connection library in its own window and block until the user picks a
// connection or closes the window. Returns the chosen profile, or nullopt on
// close. The screen is drawn with the same Surface + TextEngine renderer the
// session uses; this code only translates SDL events and uploads the frame,
// mirroring the session loop's SDL_UpdateTexture/RenderCopy path.
std::optional<ConnectionProfile> run_library(ConnectionLibrary& library)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        throw std::runtime_error(SDL_GetError());

    int width = 1000;
    int height = 720;
    SDL_Rect bounds;
    if (SDL_GetDisplayUsableBounds(0, &bounds) == 0 && bounds.w > 0
        && bounds.h > 0) {
        width = std::min(width, bounds.w);
        height = std::min(height, bounds.h);
    }

    SDL_Window* window = SDL_CreateWindow(
        "DeBeOS Remote \xE2\x80\x94 Connections", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, width, height,
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN);
    if (window == nullptr)
        throw std::runtime_error(SDL_GetError());
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == nullptr)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (renderer == nullptr)
        throw std::runtime_error(SDL_GetError());
    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width,
        height);
    if (texture == nullptr)
        throw std::runtime_error(SDL_GetError());

    LibraryScreen ui(library, width, height);
    std::optional<ConnectionProfile> chosen;
    bool running = true;
    bool closed = false;

    SDL_StartTextInput();
    while (running) {
        if (ui.dirty()) {
            const Surface& surface = ui.render();
            SDL_UpdateTexture(texture, nullptr, surface.pixels().data(),
                              surface.stride());
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }

        SDL_Event event {};
        if (SDL_WaitEvent(&event) == 0)
            break;
        do {
            switch (event.type) {
            case SDL_QUIT:
                running = false;
                closed = true;
                break;
            case SDL_MOUSEMOTION:
                ui.pointer_move(event.motion.x, event.motion.y);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT)
                    ui.pointer_press(event.button.x, event.button.y);
                break;
            case SDL_TEXTINPUT:
                ui.text_input(event.text.text);
                break;
            case SDL_KEYDOWN: {
                LibraryScreen::Key key = LibraryScreen::Key::none;
                switch (event.key.keysym.sym) {
                case SDLK_BACKSPACE: key = LibraryScreen::Key::backspace; break;
                case SDLK_DELETE: key = LibraryScreen::Key::del; break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER: key = LibraryScreen::Key::enter; break;
                case SDLK_TAB:
                    key = (SDL_GetModState() & KMOD_SHIFT) != 0
                              ? LibraryScreen::Key::back_tab
                              : LibraryScreen::Key::tab;
                    break;
                case SDLK_UP: key = LibraryScreen::Key::up; break;
                case SDLK_DOWN: key = LibraryScreen::Key::down; break;
                case SDLK_ESCAPE: key = LibraryScreen::Key::escape; break;
                default: break;
                }
                if (key != LibraryScreen::Key::none)
                    ui.key(key);
                break;
            }
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_EXPOSED)
                    ui.mark_dirty();
                break;
            default:
                break;
            }
        } while (running && SDL_PollEvent(&event) != 0);

        if (auto request = ui.take_connect_request()) {
            chosen = std::move(request);
            running = false;
        }
    }
    SDL_StopTextInput();

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return closed ? std::nullopt : chosen;
}

int main(int argc, char** argv)
{
    try {
        const Options options = parse_options(argc, argv);
        // A connection named on the command line connects straight away: the
        // automation/debugging path is unchanged.
        if (options.target_specified)
            return run_session(options);

        // No CLI target: open the connection library. Picking a connection runs
        // a session, after which the library reopens; closing it exits.
        ConnectionLibrary library;
        const LoadResult load = library.load();
        if (load.status == LoadStatus::recovered)
            std::cerr << "connection library: " << load.message << '\n';

        for (;;) {
            auto picked = run_library(library);
            if (!picked)
                return exit_status::ok;

            // Stand up the route the profile asks for: the broker (wss), an
            // owned SSH -L tunnel, or -- for the auto/direct mode -- the broker
            // first and the tunnel as a fallback. The ManagedConnection owns any
            // ssh child and temp certificate; keeping it in scope for the whole
            // run_session() call is what keeps the tunnel up, and dropping it at
            // the end of this iteration is what tears the tunnel down. The ssh
            // child is never orphaned.
            ManagedConnection conn = open_connection(*picked);
            if (!conn.ok) {
                library.set_last_error(picked->id, conn.error);
                std::cerr << "could not connect '" << picked->name
                          << "': " << conn.error << '\n';
                (void)library.save();
                continue;
            }
            std::cerr << "route: " << conn.note << '\n';
            Options session_options = options;
            session_options.transport = conn.transport;
            session_options.width = picked->width;
            session_options.height = picked->height;
            session_options.size_explicit = true;

            const int status = run_session(session_options);
            if (status == exit_status::ok) {
                library.mark_connected(picked->id);
            } else {
                library.set_last_error(
                    picked->id, "Last attempt failed (exit "
                                    + std::to_string(status) + ").");
                std::cerr << "session for '" << picked->name
                          << "' ended with status " << status << '\n';
            }
            (void)library.save();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        SDL_Quit();
        return exit_status::usage;
    }
}
