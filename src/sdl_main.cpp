#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/connect_screen.hpp"
#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/library_screen.hpp"
#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/png_writer.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/profile_library.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// The software renderer's driver index, or -1 if this SDL has none. Picked by
// index, not by flag: with SDL_RENDER_DRIVER set, SDL_CreateRenderer(-1, ...)
// tries only the hinted driver, so a SOFTWARE-flag retry would fail the same
// way the accelerated attempt did.
int software_renderer_index()
{
    const int count = SDL_GetNumRenderDrivers();
    for (int i = 0; i < count; ++i) {
        SDL_RendererInfo info;
        if (SDL_GetRenderDriverInfo(i, &info) == 0 && info.name != nullptr
            && std::string_view(info.name) == "software")
            return i;
    }
    return -1;
}

// One line naming the renderer in use, so a log says which path ran.
void report_renderer(SDL_Renderer* renderer)
{
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) == 0 && info.name != nullptr)
        std::fprintf(stderr, "renderer: %s\n", info.name);
}

// A window and a renderer for it, accelerated when the host can and software
// when it cannot: a GPU-less VM, an RDP-ed Windows box, SDL's offscreen
// driver. The fallback was always meant to exist, but on SDL3 through
// sdl2-compat (what Homebrew's "sdl2" is) a failed accelerated attempt
// recreates the window for OpenGL or Metal and can leave it unusable, so the
// software retry on the *same* window failed too, reporting the stale "Could
// not initialize OpenGL / GLES library" (measured on a GPU-less macOS 26 VM).
// So the window is recreated before the software renderer is tried, and every
// failure names the call that failed.
void create_window_and_renderer(const char* title, int width, int height,
                                Uint32 window_flags, SDL_Window*& window,
                                SDL_Renderer*& renderer)
{
    const auto make_window = [&]() {
        window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED, width, height,
                                  window_flags);
        if (window == nullptr)
            throw std::runtime_error(std::string("SDL_CreateWindow: ")
                                     + SDL_GetError());
    };
    make_window();
    const char* software_only = std::getenv("HAIKU_REMOTE_SOFTWARE_RENDERER");
    if (software_only == nullptr || software_only[0] == '\0'
        || std::string_view(software_only) == "0") {
        renderer = SDL_CreateRenderer(
            window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (renderer != nullptr) {
            report_renderer(renderer);
            return;
        }
        std::fprintf(stderr,
                     "accelerated renderer unavailable (%s); using the software"
                     " renderer\n", SDL_GetError());
        SDL_DestroyWindow(window);
        make_window();
    }
    renderer = SDL_CreateRenderer(window, software_renderer_index(),
                                  SDL_RENDERER_SOFTWARE);
    if (renderer == nullptr)
        throw std::runtime_error(std::string("SDL_CreateRenderer (software): ")
                                 + SDL_GetError());
    report_renderer(renderer);
}

} // namespace

// Run one connected remote-desktop session for already-resolved options. This
// is the client's original SDL behaviour verbatim, lifted into a function so
// both the CLI path and a library pick can drive it. `options` is taken by
// value because the display-size defaulting below mutates it.
// Run one connected remote-desktop session over an ALREADY-connected transport.
// Part 3 split this out of run_session() so the library path can connect behind
// a visible progress screen first and hand the live transport straight here.
// `refused_no_data` is set when the session ended by an orderly close with no
// drawing ever received -- the refused/nothing-to-show case the library path
// turns into an actionable error rather than a black flash.
int run_session_loop(Options options, const std::unique_ptr<Transport>& transport,
                     bool& refused_no_data)
{
    refused_no_data = false;
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

        SDL_Window* window = nullptr;
        SDL_Renderer* renderer = nullptr;
        create_window_and_renderer(
            "Haiku Remote", options.width, options.height,
            SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE,
            window, renderer);
        SDL_RenderSetLogicalSize(renderer, options.width, options.height);
        SDL_Texture* texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            options.width, options.height);
        if (texture == nullptr)
            throw std::runtime_error(std::string("SDL_CreateTexture: ")
                                     + SDL_GetError());

        std::string socket_error;
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
                const bool orderly =
                    transport->peer_closed() || session.server_closed();
                if (orderly && session.message_count() == 0)
                    refused_no_data = true;
                session_failed = !report_session_end(
                    orderly, session.message_count(), socket_error);
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
                if (session.message_count() == 0)
                    refused_no_data = true;
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

// The CLI path, unchanged in behaviour: resolve a transport from the options,
// connect it inline (reporting a connect failure exactly as before, with the
// same exit code), then run the session loop.
int run_session(Options options)
{
    std::string socket_error;
    auto transport = make_transport(options.transport, socket_error);
    if (transport == nullptr) {
        std::cerr << socket_error << '\n';
        return exit_status::failed;
    }
    if (!connect_with_broker_trust(*transport,
                                   terminal_trust_policy(options.transport),
                                   socket_error)) {
        std::cerr << "connect to " << transport->describe() << " failed: "
                  << socket_error << '\n';
        return connect_exit_status(*transport);
    }
    bool refused = false;
    return run_session_loop(std::move(options), transport, refused);
}

// The result of the connection-progress window: the connected transport (plus
// the managed substrate it needs kept alive) on success, or the user's choice
// on the error screen.
struct ConnectOutcome {
    ConnectResult result;
    ConnectScreen::Action action = ConnectScreen::Action::none; // only on failure
};

// Open a window and run connect_with_progress for `profile`, painting the
// progress checklist as the flow advances -- the replacement for the old silent
// black window. On success the window is torn down and the live transport
// returned. On failure the window stays up showing the actionable error until
// the user picks Back or Retry (or closes it, treated as Back).
ConnectOutcome run_connect(const ConnectionProfile& profile)
{
    ConnectOutcome out;
    const std::string title = profile.name.empty() ? profile.host : profile.name;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        throw std::runtime_error(SDL_GetError());

    int width = 900;
    int height = 560;
    SDL_Rect bounds;
    if (SDL_GetDisplayUsableBounds(0, &bounds) == 0 && bounds.w > 0
        && bounds.h > 0) {
        width = std::min(width, bounds.w);
        height = std::min(height, bounds.h);
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    create_window_and_renderer("DeBeOS Remote \xE2\x80\x94 Connecting", width, height,
                               SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN,
                               window, renderer);
    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width,
        height);
    if (texture == nullptr)
        throw std::runtime_error(std::string("SDL_CreateTexture: ")
                                 + SDL_GetError());

    ConnectFlow flow(plan_launch(profile));
    ConnectScreen ui(flow, title, width, height);
    bool closed = false;

    const auto blit = [&]() {
        const Surface& surface = ui.render();
        SDL_UpdateTexture(texture, nullptr, surface.pixels().data(),
                          surface.stride());
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    };
    const auto pump = [&]() {
        SDL_Event event {};
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT)
                closed = true;
            else if (event.type == SDL_WINDOWEVENT
                     && event.window.event == SDL_WINDOWEVENT_EXPOSED)
                ui.mark_dirty();
        }
    };
    // Called at every stage boundary by connect_with_progress: repaint the new
    // flow state and keep the window responsive while the blocking SSH/connect
    // calls run between ticks with the current stage label on screen.
    const auto on_progress = [&]() {
        ui.mark_dirty();
        blit();
        pump();
    };

    // Block on window events until the screen yields an action (a window
    // close counts as the refusing choice). Shared by the error screen and the
    // broker trust prompt.
    const auto wait_for_action = [&]() -> ConnectScreen::Action {
        ui.mark_dirty();
        blit();
        ConnectScreen::Action action = ConnectScreen::Action::none;
        while (!closed && action == ConnectScreen::Action::none) {
            if (ui.dirty())
                blit();
            SDL_Event event {};
            if (SDL_WaitEvent(&event) == 0) {
                closed = true;
                break;
            }
            do {
                switch (event.type) {
                case SDL_QUIT:
                    closed = true;
                    break;
                case SDL_MOUSEMOTION:
                    ui.pointer_move(event.motion.x, event.motion.y);
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (event.button.button == SDL_BUTTON_LEFT)
                        ui.pointer_press(event.button.x, event.button.y);
                    break;
                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_RETURN
                        || event.key.keysym.sym == SDLK_KP_ENTER)
                        ui.key(ConnectScreen::Key::enter);
                    else if (event.key.keysym.sym == SDLK_ESCAPE)
                        ui.key(ConnectScreen::Key::escape);
                    break;
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_EXPOSED)
                        ui.mark_dirty();
                    break;
                default:
                    break;
                }
            } while (!closed && SDL_PollEvent(&event) != 0);
            action = ui.take_action();
        }
        return action;
    };

    // Trust on first use: an unknown or changed broker certificate is put to
    // the user in this window. Closing it is a refusal.
    const TrustPrompt ask_trust = [&](const BrokerCheck& check) -> TrustChoice {
        ui.show_trust_prompt(check);
        const ConnectScreen::Action action = wait_for_action();
        ui.clear_trust_prompt();
        if (closed)
            return TrustChoice::reject;
        if (action == ConnectScreen::Action::trust)
            return TrustChoice::trust;
        if (action == ConnectScreen::Action::replace)
            return TrustChoice::replace;
        return TrustChoice::reject;
    };

    blit();
    out.result = connect_with_progress(profile, flow, on_progress, nullptr,
                                       ask_trust);

    if (!out.result.ok) {
        const ConnectScreen::Action action = closed ? ConnectScreen::Action::back
                                                    : wait_for_action();
        out.action = closed ? ConnectScreen::Action::back : action;
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return out;
}

// The library screen's key for an SDL key press, with its modifiers. Text is
// not handled here -- it arrives as SDL_TEXTINPUT (and SDL_TEXTEDITING while
// an input method composes), so any script and any IME work. The platform
// conventions: Cmd is the command key and Option the word modifier on macOS
// (Cmd-Left/Right go to the ends); Ctrl is both elsewhere. The classic
// Shift-Insert / Ctrl-Insert / Shift-Delete clipboard keys work too.
std::pair<LibraryScreen::Key, unsigned> library_key(const SDL_Keysym& keysym)
{
    using Key = LibraryScreen::Key;
    const auto mod = static_cast<unsigned>(keysym.mod);
#ifdef __APPLE__
    const bool command = (mod & KMOD_GUI) != 0;
    const bool word = (mod & KMOD_ALT) != 0;
#else
    const bool command = (mod & KMOD_CTRL) != 0;
    const bool word = (mod & KMOD_CTRL) != 0;
#endif
    const bool shift = (mod & KMOD_SHIFT) != 0;
    unsigned mods = shift ? LibraryScreen::mod_shift : LibraryScreen::mod_none;
    if (word)
        mods |= LibraryScreen::mod_word;
    Key key = Key::none;
    switch (keysym.sym) {
    case SDLK_BACKSPACE: key = Key::backspace; break;
    case SDLK_DELETE: key = shift && !command ? Key::cut : Key::del; break;
    case SDLK_INSERT:
        key = shift ? Key::paste : (mod & KMOD_CTRL) != 0 ? Key::copy : Key::none;
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: key = Key::enter; break;
    case SDLK_TAB: key = shift ? Key::back_tab : Key::tab; break;
    case SDLK_UP: key = Key::up; break;
    case SDLK_DOWN: key = Key::down; break;
    case SDLK_LEFT: key = Key::left; break;
    case SDLK_RIGHT: key = Key::right; break;
    case SDLK_HOME: key = Key::home; break;
    case SDLK_END: key = Key::end; break;
    case SDLK_PAGEUP: key = Key::page_up; break;
    case SDLK_PAGEDOWN: key = Key::page_down; break;
    case SDLK_ESCAPE: key = Key::escape; break;
    case SDLK_a: key = command ? Key::select_all : Key::none; break;
    case SDLK_c: key = command ? Key::copy : Key::none; break;
    case SDLK_x: key = command ? Key::cut : Key::none; break;
    case SDLK_v: key = command ? Key::paste : Key::none; break;
    default: break;
    }
#ifdef __APPLE__
    if (command && (key == Key::left || key == Key::right)) {
        key = key == Key::left ? Key::home : Key::end;
        mods &= ~static_cast<unsigned>(LibraryScreen::mod_word);
    }
#endif
    return {key, mods};
}

// HAIKU_REMOTE_LIBRARY_EVENTS: a verification hook. A ';'-separated script
// of synthetic SDL events, each run only once the queue has drained, so they
// go through exactly the translation above, in order:
//   text:<utf8>  compose:<utf8>  key:[shift+][ctrl+][alt+][gui+]<name>
//   down:<x>,<y>[,<clicks>]  up:<x>,<y>  move:<x>,<y>  wheel:<notches>
//   size:<w>x<h>  clip:<utf8>  snap:<file.png>  quit
// Key names: left right home end up down tab return escape backspace delete
// pageup pagedown insert, or a single letter. Coordinates are window units.
std::vector<std::string> split_script(const char* script)
{
    std::vector<std::string> steps;
    if (script == nullptr)
        return steps;
    std::string step;
    for (const char* c = script;; ++c) {
        if (*c == ';' || *c == '\0') {
            if (!step.empty())
                steps.push_back(step);
            step.clear();
            if (*c == '\0')
                break;
        } else {
            step.push_back(*c);
        }
    }
    return steps;
}

bool push_script_event(const std::string& step, SDL_Window* window,
                       std::string& snapshot_path)
{
    const auto colon = step.find(':');
    const std::string verb = step.substr(0, colon);
    const std::string arg = colon == std::string::npos ? "" : step.substr(colon + 1);
    SDL_Event event {};
    const auto window_id = SDL_GetWindowID(window);
    const auto two_ints = [&](int& a, int& b, int& c) {
        c = 1;
        return std::sscanf(arg.c_str(), "%d,%d,%d", &a, &b, &c) >= 2;
    };
    if (verb == "text" || verb == "compose") {
        event.type = verb == "text" ? SDL_TEXTINPUT : SDL_TEXTEDITING;
        event.text.windowID = window_id;
        std::snprintf(verb == "text" ? event.text.text : event.edit.text,
                      SDL_TEXTINPUTEVENT_TEXT_SIZE, "%s", arg.c_str());
        if (verb == "compose") {
            event.edit.start = -1;
            event.edit.length = 0;
        }
    } else if (verb == "key") {
        std::string name = arg;
        Uint16 mod = KMOD_NONE;
        for (const auto& [prefix, bit] :
             {std::pair<std::string, Uint16> {"shift+", KMOD_LSHIFT},
              {"ctrl+", KMOD_LCTRL}, {"alt+", KMOD_LALT}, {"gui+", KMOD_LGUI}}) {
            while (name.rfind(prefix, 0) == 0) {
                mod = static_cast<Uint16>(mod | bit);
                name.erase(0, prefix.size());
            }
        }
        SDL_Keycode sym = SDL_GetKeyFromName(name.c_str());
        if (sym == SDLK_UNKNOWN)
            return false;
        event.type = SDL_KEYDOWN;
        event.key.windowID = window_id;
        event.key.state = SDL_PRESSED;
        event.key.keysym.sym = sym;
        event.key.keysym.scancode = SDL_GetScancodeFromKey(sym);
        event.key.keysym.mod = mod;
    } else if (verb == "down" || verb == "up") {
        int x = 0, y = 0, clicks = 1;
        if (!two_ints(x, y, clicks))
            return false;
        event.type = verb == "down" ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        event.button.windowID = window_id;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.state = verb == "down" ? SDL_PRESSED : SDL_RELEASED;
        event.button.clicks = static_cast<Uint8>(clicks);
        event.button.x = x;
        event.button.y = y;
    } else if (verb == "move") {
        int x = 0, y = 0, unused = 0;
        if (!two_ints(x, y, unused))
            return false;
        event.type = SDL_MOUSEMOTION;
        event.motion.windowID = window_id;
        event.motion.x = x;
        event.motion.y = y;
    } else if (verb == "wheel") {
        event.type = SDL_MOUSEWHEEL;
        event.wheel.windowID = window_id;
        event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        event.wheel.y = std::atoi(arg.c_str());
#if SDL_VERSION_ATLEAST(2, 0, 18)
        event.wheel.preciseY = static_cast<float>(event.wheel.y);
#endif
    } else if (verb == "size") {
        int w = 0, h = 0;
        if (std::sscanf(arg.c_str(), "%dx%d", &w, &h) != 2)
            return false;
        SDL_SetWindowSize(window, w, h); // clamped to the minimum size
        return true;
    } else if (verb == "clip") {
        // Put text on the system clipboard, as another application would.
        return SDL_SetClipboardText(arg.c_str()) == 0;
    } else if (verb == "snap") {
        snapshot_path = arg;
        return true;
    } else if (verb == "quit") {
        event.type = SDL_QUIT;
    } else {
        return false;
    }
    return SDL_PushEvent(&event) == 1;
}

// Read the renderer's current frame back (not the UI surface), so a headless
// check sees what actually reached the window, texture upload included.
void write_snapshot(SDL_Renderer* renderer, const std::string& path)
{
    int out_w = 0;
    int out_h = 0;
    SDL_GetRendererOutputSize(renderer, &out_w, &out_h);
    Surface shot(std::max(out_w, 1), std::max(out_h, 1));
    std::string error;
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                             shot.pixels().data(), shot.stride())
        != 0)
        error = SDL_GetError();
    else
        (void)write_png(shot, path, error);
    std::cerr << "library snapshot " << out_w << 'x' << out_h << ": "
              << (error.empty() ? path : error) << '\n';
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

    // 1024x700 is the size issue #1 requires the library to be usable at; the
    // window is resizable from there. Sizes here are window (logical) units.
    int width = 1024;
    int height = 700;
    SDL_Rect bounds;
    if (SDL_GetDisplayUsableBounds(0, &bounds) == 0 && bounds.w > 0
        && bounds.h > 0) {
        width = std::min(width, bounds.w);
        height = std::min(height, bounds.h);
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    create_window_and_renderer("DeBeOS Remote \xE2\x80\x94 Connections", width, height,
                               SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN
                                   | SDL_WINDOW_RESIZABLE,
                               window, renderer);
    // The editor scrolls, so the window can shrink well below its 700-row
    // starting height, to the smallest size the list still lays out at.
    SDL_SetWindowMinimumSize(window, std::min(width, LibraryScreen::min_window_width),
                             std::min(height, LibraryScreen::min_window_height));

    // High-DPI: the UI renders at the renderer's OUTPUT size (device pixels)
    // with a scale of output/window, so a Retina window is sharp rather than an
    // upscaled 1x frame. Pointer events arrive in window units and are scaled
    // to device pixels the same way. The texture is always the UI surface's
    // size, so an upload can never disagree with it.
    LibraryScreen ui(library, width, height);
    // Copy / cut / paste go through the system clipboard.
    ui.set_clipboard(
        [] {
            std::string text;
            if (char* clip = SDL_GetClipboardText()) {
                text = clip;
                SDL_free(clip);
            }
            return text;
        },
        [](const std::string& text) { (void)SDL_SetClipboardText(text.c_str()); });
    SDL_Texture* texture = nullptr;
    float pointer_scale_x = 1.0f;
    float pointer_scale_y = 1.0f;
    const auto sync_size = [&] {
        int window_w = 0;
        int window_h = 0;
        int output_w = 0;
        int output_h = 0;
        SDL_GetWindowSize(window, &window_w, &window_h);
        if (SDL_GetRendererOutputSize(renderer, &output_w, &output_h) != 0
            || output_w <= 0 || output_h <= 0) {
            output_w = window_w;
            output_h = window_h;
        }
        pointer_scale_x = window_w > 0 ? static_cast<float>(output_w) / window_w : 1.0f;
        pointer_scale_y = window_h > 0 ? static_cast<float>(output_h) / window_h : 1.0f;
        ui.set_scale(pointer_scale_x);
        ui.resize(output_w, output_h);
        int texture_w = 0;
        int texture_h = 0;
        if (texture != nullptr)
            SDL_QueryTexture(texture, nullptr, nullptr, &texture_w, &texture_h);
        if (texture == nullptr || texture_w != ui.width() || texture_h != ui.height()) {
            if (texture != nullptr)
                SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, ui.width(),
                                        ui.height());
            if (texture == nullptr)
                throw std::runtime_error(std::string("SDL_CreateTexture: ")
                                         + SDL_GetError());
        }
        ui.mark_dirty();
    };
    sync_size();
    const auto device_x = [&](int x) {
        return static_cast<int>(std::lround(static_cast<float>(x) * pointer_scale_x));
    };
    const auto device_y = [&](int y) {
        return static_cast<int>(std::lround(static_cast<float>(y) * pointer_scale_y));
    };
    std::optional<ConnectionProfile> chosen;
    bool running = true;
    bool closed = false;
    // Verification hooks: HAIKU_REMOTE_LIBRARY_SNAPSHOT=<file.png> reads the
    // first presented frame back; HAIKU_REMOTE_LIBRARY_EVENTS (above) drives
    // the window with synthetic events and takes snapshots along the way. Both
    // work headless, with SDL_VIDEODRIVER=offscreen.
    std::string snapshot_path;
    if (const char* first = std::getenv("HAIKU_REMOTE_LIBRARY_SNAPSHOT"))
        snapshot_path = first;
    const std::vector<std::string> script =
        split_script(std::getenv("HAIKU_REMOTE_LIBRARY_EVENTS"));
    std::size_t script_next = 0;
    SDL_Rect input_rect {0, 0, 0, 0};

    SDL_StartTextInput();
    while (running) {
        if (ui.dirty()) {
            const Surface& surface = ui.render();
            SDL_UpdateTexture(texture, nullptr, surface.pixels().data(),
                              surface.stride());
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            if (!snapshot_path.empty()) {
                write_snapshot(renderer, snapshot_path);
                snapshot_path.clear();
            }
            SDL_RenderPresent(renderer);
            // Keep an input method's candidate window next to the caret.
            const LibraryScreen::Region& caret = ui.caret_rect();
            if (caret.w > 0 && pointer_scale_x > 0 && pointer_scale_y > 0) {
                const SDL_Rect rect {
                    static_cast<int>(static_cast<float>(caret.x) / pointer_scale_x),
                    static_cast<int>(static_cast<float>(caret.y) / pointer_scale_y),
                    std::max(1, static_cast<int>(static_cast<float>(caret.w) / pointer_scale_x)),
                    static_cast<int>(static_cast<float>(caret.h) / pointer_scale_y)};
                if (rect.x != input_rect.x || rect.y != input_rect.y
                    || rect.h != input_rect.h) {
                    input_rect = rect;
                    SDL_SetTextInputRect(&input_rect);
                }
            }
        }

        SDL_Event event {};
        if (script_next < script.size()) {
            // Scripted: the next step runs only once everything before it has
            // been handled (and rendered, by the top of the loop).
            if (SDL_PollEvent(&event) == 0) {
                const std::string& step = script[script_next++];
                if (!push_script_event(step, window, snapshot_path)) {
                    std::cerr << "HAIKU_REMOTE_LIBRARY_EVENTS: bad step '" << step
                              << "'\n";
                    running = false;
                    closed = true;
                }
                if (!snapshot_path.empty())
                    ui.mark_dirty(); // a snap step: render and read back now
                continue;
            }
        } else {
            // Sleep until an event arrives or the caret's blink phase is up:
            // no polling, and no wake-ups at all when nothing blinks.
            const int wait_ms = ui.next_wake_ms();
            const int got = wait_ms < 0 ? SDL_WaitEvent(&event)
                                        : SDL_WaitEventTimeout(&event, wait_ms);
            if (got == 0) {
                if (wait_ms < 0)
                    break; // SDL_WaitEvent failed
                ui.tick();
                continue;
            }
        }
        do {
            switch (event.type) {
            case SDL_QUIT:
                running = false;
                closed = true;
                break;
            case SDL_MOUSEMOTION:
                ui.pointer_move(device_x(event.motion.x), device_y(event.motion.y));
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT)
                    ui.pointer_press(device_x(event.button.x),
                                     device_y(event.button.y),
                                     std::max<int>(1, event.button.clicks),
                                     (SDL_GetModState() & KMOD_SHIFT) != 0
                                         ? LibraryScreen::mod_shift
                                         : LibraryScreen::mod_none);
                break;
            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT)
                    ui.pointer_release(device_x(event.button.x),
                                       device_y(event.button.y));
                break;
            case SDL_MOUSEWHEEL: {
#if SDL_VERSION_ATLEAST(2, 0, 18)
                float notches = event.wheel.preciseY;
#else
                float notches = static_cast<float>(event.wheel.y);
#endif
                if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                    notches = -notches;
                ui.wheel(notches);
                break;
            }
            case SDL_TEXTINPUT:
                ui.text_input(event.text.text);
                break;
            case SDL_TEXTEDITING:
                // SDL_TEXTEDITING's `start` is the IME cursor, in code points.
                ui.text_editing(event.edit.text, event.edit.start);
                break;
            case SDL_KEYDOWN: {
                const auto [key, mods] = library_key(event.key.keysym);
                if (key != LibraryScreen::Key::none)
                    ui.key(key, mods);
                break;
            }
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_EXPOSED)
                    ui.mark_dirty();
                else if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                    sync_size();
                else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                    ui.set_window_active(true);
                else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    ui.set_window_active(false);
#if SDL_VERSION_ATLEAST(2, 0, 18)
                else if (event.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)
                    sync_size(); // moved to a display with a different scale
#endif
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

            // Connect with a visible progress screen, then run the session. The
            // ManagedConnection the connect produced owns any ssh child
            // (the tunnel route); it lives inside `outcome.result` and is kept in scope
            // for the whole run_session_loop() call, which keeps the tunnel up --
            // dropping `outcome` at the end tears it down, so the ssh child is
            // never orphaned. A connect failure shows an actionable error with
            // Back/Retry instead of a black window; Retry re-runs the connect for
            // the same profile, Back returns to the library. Either way the
            // failure is recorded on the library card too.
            for (;;) {
                ConnectOutcome outcome = run_connect(*picked);
                if (outcome.result.ok) {
                    std::cerr << "route: " << outcome.result.route_note << '\n';
                    Options session_options = options;
                    session_options.width = picked->width;
                    session_options.height = picked->height;
                    session_options.size_explicit = true;
                    bool refused = false;
                    const int status = run_session_loop(
                        session_options, outcome.result.transport, refused);
                    if (status == exit_status::ok) {
                        library.mark_connected(picked->id);
                    } else if (refused) {
                        const ConnectError e = classify_connect_failure(
                            ConnectFailPoint::session_refused, ConnectFailure::none,
                            "the server closed the connection before sending any"
                            " drawing");
                        library.set_last_error(picked->id, e.summary());
                        std::cerr << "session for '" << picked->name
                                  << "' was refused: " << e.summary() << '\n';
                    } else {
                        library.set_last_error(
                            picked->id, "Last attempt failed (exit "
                                            + std::to_string(status) + ").");
                        std::cerr << "session for '" << picked->name
                                  << "' ended with status " << status << '\n';
                    }
                    (void)library.save();
                    break;
                }

                library.set_last_error(picked->id, outcome.result.error.summary());
                std::cerr << "could not connect '" << picked->name
                          << "': " << outcome.result.error.raw << '\n';
                (void)library.save();
                if (outcome.action == ConnectScreen::Action::retry)
                    continue;
                break;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        SDL_Quit();
        return exit_status::usage;
    }
}
