#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/connect_screen.hpp"
#include "haiku_remote/desktop_shell.hpp"
#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/library_screen.hpp"
#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/profile_library.hpp"
#include "haiku_remote/reconnect.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
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

// --- The connected desktop -------------------------------------------------
//
// One window, one Session and one DesktopShell for the life of a desktop, over
// however many connections it takes: the surface (the only copy of the remote
// screen) and the session identity carry across a reconnect, the transport and
// whatever route it rode on do not. Everything that decides *whether* to come
// back is ReconnectSchedule's (src/reconnect.cpp); everything about *how* a
// connection is built is the SessionLink's, so this loop never names a route.

// What builds the desktop's connections. The current transport, if any, is
// owned here together with the substrate it needs (an SSH tunnel), and
// teardown() drops both -- always before connect() builds the next, so a
// reconnect never has two forwards up at once.
class SessionLink {
public:
    virtual ~SessionLink() = default;
    [[nodiscard]] virtual Transport* transport() = 0;
    virtual void teardown() = 0;
    // Build and connect a fresh transport. `progress` is called with a short
    // label whenever the attempt moves to a new stage. On failure `outcome` is
    // the reconnect policy's reading of it and `error` the user-facing reason.
    virtual bool connect(const std::function<void(std::string_view)>& progress,
                         ConnectionOutcome& outcome, std::string& error) = 0;
    // The connection connect() built (or the one handed over at construction)
    // carried a session: the server sent something back.
    virtual void established() {}
    [[nodiscard]] virtual std::string route() const { return {}; }
    // The transport is the local end of an SSH -L forward (see
    // ConnectionResult::through_forward).
    [[nodiscard]] virtual bool through_forward() const { return false; }
};

// The command-line path: the transport options from argv, connected again on a
// manual Reconnect exactly the way run_session() connected them the first time.
class OptionsLink final : public SessionLink {
public:
    OptionsLink(TransportOptions options, std::unique_ptr<Transport> connected)
        : options_(std::move(options)), transport_(std::move(connected)) {}

    Transport* transport() override { return transport_.get(); }
    void teardown() override { transport_.reset(); }

    bool connect(const std::function<void(std::string_view)>& progress,
                 ConnectionOutcome& outcome, std::string& error) override
    {
        auto transport = make_transport(options_, error);
        if (transport == nullptr) {
            outcome = ConnectionOutcome::refused; // a misinvocation: never retry
            return false;
        }
        progress("connecting to " + transport->describe());
        if (!connect_with_broker_trust(*transport, terminal_trust_policy(options_),
                                       error)) {
            ConnectionResult result;
            result.connect_failure = transport->connect_failure();
            outcome = classify_connection(result);
            if (result.connect_failure == ConnectFailure::broker_unknown
                || result.connect_failure == ConnectFailure::broker_changed)
                outcome = ConnectionOutcome::refused; // a decision, not a blip
            error = "connect to " + transport->describe() + " failed: " + error;
            return false;
        }
        transport_ = std::move(transport);
        return true;
    }

    std::string route() const override
    {
        return transport_ ? transport_->describe() : std::string();
    }

private:
    TransportOptions options_;
    std::unique_ptr<Transport> transport_;
};

// The library path: a profile, connected through plan_launch and
// connect_with_progress -- the same managed route walk the progress screen ran
// -- with the result (transport plus ManagedConnection, which owns the ssh
// child) held here. A first successful exchange stamps the profile's
// last-connected time, whether or not the session later drops.
class ProfileLink final : public SessionLink {
public:
    ProfileLink(ConnectionProfile profile, ConnectionLibrary& library,
                ConnectResult connected)
        : profile_(std::move(profile)), library_(library),
          result_(std::move(connected)) {}

    Transport* transport() override { return result_.transport.get(); }

    void teardown() override
    {
        // The transport first, so its socket closes before the forward it runs
        // through; then the substrate, whose destructor terminates and reaps
        // the ssh child (ManagedProcess).
        result_.transport.reset();
        result_.connection = ManagedConnection {};
        result_.ok = false;
    }

    bool connect(const std::function<void(std::string_view)>& progress,
                 ConnectionOutcome& outcome, std::string& error) override
    {
        teardown();
        ConnectFlow flow(plan_launch(profile_));
        std::optional<std::size_t> shown;
        const auto on_progress = [&]() {
            const auto active = flow.active_stage();
            if (active.has_value() && active != shown) {
                shown = active;
                progress(flow.stages()[*active].label);
            }
        };
        // No trust prompt here: a broker certificate that became unknown or
        // changed since the connection was first made is not something to
        // accept from inside a reconnect loop. It fails the attempt, and the
        // policy does not retry it (outcome_for_connect_error).
        result_ = connect_with_progress(profile_, flow, on_progress);
        if (!result_.ok || result_.transport == nullptr) {
            outcome = outcome_for_connect_error(result_.error);
            error = result_.error.summary();
            teardown();
            return false;
        }
        std::cerr << "route: " << result_.route_note << '\n';
        return true;
    }

    void established() override
    {
        library_.mark_connected(profile_.id);
        const SaveResult saved = library_.save();
        if (!saved.ok)
            std::cerr << "connection library: could not record the connection: "
                      << saved.message << '\n';
    }

    std::string route() const override { return result_.route_note; }
    bool through_forward() const override
    {
        return result_.connection.tunnel != nullptr;
    }

private:
    ConnectionProfile profile_;
    ConnectionLibrary& library_;
    ConnectResult result_;
};

// How a desktop ended, for the caller's exit status and the library card.
struct DesktopEnd {
    int status = exit_status::ok;
    // The user closed the window or pressed Disconnect.
    bool user_ended = false;
    // The last connection closed in an orderly way without sending anything.
    bool refused_no_data = false;
    // The last connection's ending, and why the desktop gave up on it.
    ConnectionOutcome outcome = ConnectionOutcome::completed;
    std::string reason;
};

// The SDL front end's toolbar hotkey, Ctrl+Alt+Home -- the chord Microsoft's
// RDP client uses for its connection bar, so it is free of the remote's own
// shortcuts. SDL's Alt is Haiku's Command (modifier_mask()).
bool is_shell_hotkey(const SDL_Keysym& key)
{
    return key.sym == SDLK_HOME && (key.mod & KMOD_CTRL) != 0
        && (key.mod & KMOD_ALT) != 0;
}

std::string format_bytes(std::uint64_t bytes)
{
    char text[32];
    if (bytes >= 10ull * 1024 * 1024)
        std::snprintf(text, sizeof text, "%.1f MiB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        std::snprintf(text, sizeof text, "%.1f KiB",
                      static_cast<double>(bytes) / 1024.0);
    return text;
}

// Upload `surface` into `texture`, (re)creating it when the size changed.
void upload(SDL_Renderer* renderer, SDL_Texture*& texture, const Surface& surface)
{
    int w = 0;
    int h = 0;
    if (texture != nullptr
        && (SDL_QueryTexture(texture, nullptr, nullptr, &w, &h) != 0
            || w != surface.width() || h != surface.height())) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    if (texture == nullptr) {
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, surface.width(),
                                    surface.height());
        if (texture == nullptr)
            throw std::runtime_error(std::string("SDL_CreateTexture: ")
                                     + SDL_GetError());
    }
    if (SDL_UpdateTexture(texture, nullptr, surface.pixels().data(),
                          surface.stride()) != 0)
        throw std::runtime_error(SDL_GetError());
}

SDL_Rect to_sdl(const IntRect& r)
{
    return SDL_Rect {r.left, r.top, r.right - r.left + 1, r.bottom - r.top + 1};
}

// Run a connected desktop over `link`, whose transport is already connected.
// Auto-reconnect follows `reconnect`; a manual Reconnect from the toolbar works
// either way.
DesktopEnd run_desktop(Options options, SessionLink& link, const std::string& name,
                       const ReconnectConfig& reconnect)
{
    DesktopEnd end;
    using Clock = std::chrono::steady_clock;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        throw std::runtime_error(SDL_GetError());
    // Default the remote desktop to the local display size so the window
    // does not come up larger than the screen (e.g. a 1280x800 default on a
    // 1024x768 panel). --width/--height still override.
    if (!options.size_explicit) {
        SDL_Rect bounds;
        if (SDL_GetDisplayUsableBounds(0, &bounds) == 0 && bounds.w > 0
            && bounds.h > 0) {
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
    SDL_Texture* texture = nullptr;
    SDL_Texture* bar_texture = nullptr;
    SDL_Texture* stats_texture = nullptr;
    const auto release = [&]() {
        SDL_StopTextInput();
        if (stats_texture != nullptr)
            SDL_DestroyTexture(stats_texture);
        if (bar_texture != nullptr)
            SDL_DestroyTexture(bar_texture);
        if (texture != nullptr)
            SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    };

    try {
        std::string socket_error;
        Session session(
            options.width, options.height,
            [](std::span<const std::uint8_t>) { return false; },
            [](std::string_view line) { std::cerr << line << '\n'; });
        DesktopShell shell(name, options.width, options.height);
        ReconnectSchedule schedule(reconnect);
        ResizeSettler settler;

        bool live = false;        // a transport is connected and started
        bool established = false; // this connection has carried a session
        bool running = true;
        bool dirty = true;
        bool quit_requested = false;
        bool shell_was_visible = false;
        bool stats_were_visible = false;
        int reconnects = 0;
        std::uint32_t last_modifiers = 0;
        std::uint64_t bytes_received = 0;
        std::size_t frames = 0;
        auto stats_since = Clock::now();
        double frame_rate = 0;
        std::array<std::uint8_t, 256 * 1024> buffer {};

        // Present the frame: the session's surface, with the bar and the stats
        // panel composited over it as separate textures. Neither is ever drawn
        // into the session's surface.
        const auto present = [&]() {
            if (dirty) {
                upload(renderer, texture, session.surface());
                dirty = false;
            }
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            const bool bar_shown = shell.visible();
            const bool stats_shown = shell.stats_visible();
            if (bar_shown && (shell.dirty() || bar_texture == nullptr))
                upload(renderer, bar_texture, shell.render_bar());
            if (stats_shown && (shell.dirty() || stats_texture == nullptr))
                upload(renderer, stats_texture, shell.render_stats());
            if (bar_shown || stats_shown)
                shell.mark_clean();
            if (stats_shown) {
                const SDL_Rect r = to_sdl(shell.stats_rect());
                SDL_RenderCopy(renderer, stats_texture, nullptr, &r);
            }
            if (bar_shown) {
                const SDL_Rect r = to_sdl(shell.bar_rect());
                SDL_RenderCopy(renderer, bar_texture, nullptr, &r);
            }
            SDL_RenderPresent(renderer);
            shell_was_visible = bar_shown;
            stats_were_visible = stats_shown;
            ++frames;
        };

        const auto send = [&](const std::vector<std::uint8_t>& message) {
            if (!live)
                return;
            if (!session.send_client_message(message))
                std::cerr << "send failed: " << socket_error << '\n';
            // The receive path observes the dead transport and ends the
            // connection through the one place that classifies endings.
        };

        // The window's drawable size in device pixels: what "Window" and
        // fit-to-window ask the remote for, so the desktop is shown 1:1 rather
        // than scaled.
        const auto drawable_size = [&]() {
            DisplaySize size;
            if (SDL_GetRendererOutputSize(renderer, &size.width, &size.height) != 0)
                size = {};
            return size;
        };

        // A live resolution change. The session reallocates its canvas and,
        // on a live connection, sends RP_UPDATE_DISPLAY_MODE; app_server then
        // repaints the whole new frame. Without a connection only the local
        // size changes, and the next handshake asks for it. The renderer's
        // logical size follows, so input keeps mapping onto desktop pixels.
        const auto apply_display_size = [&](DisplaySize size) {
            if (!valid_display_size(size.width, size.height)) {
                std::cerr << "display size " << size.width << 'x' << size.height
                          << " refused: each side must be 1.."
                          << Surface::max_dimension << '\n';
                return false;
            }
            if (size.width == session.display_width()
                && size.height == session.display_height())
                return false;
            if (live) {
                const auto change
                    = session.change_display_mode(size.width, size.height);
                std::cerr << "display mode " << size.width << 'x' << size.height
                          << (change == Session::DisplayModeChange::sent
                                  ? " requested (RP_UPDATE_DISPLAY_MODE)"
                                  : " adopted locally; the send failed")
                          << '\n';
            } else {
                session.set_display_size(size.width, size.height);
                std::cerr << "display size " << size.width << 'x' << size.height
                          << " adopted; the next connection asks for it\n";
            }
            SDL_RenderSetLogicalSize(renderer, size.width, size.height);
            shell.set_desktop_size(size.width, size.height);
            dirty = true;
            return true;
        };

        // A fixed size: also bring the window to it, so it is shown 1:1, when
        // it fits on the display and the window is not full screen.
        const auto fit_window_to = [&](DisplaySize size) {
            if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
                return;
            int points_w = 0;
            int points_h = 0;
            SDL_GetWindowSize(window, &points_w, &points_h);
            const DisplaySize pixels = drawable_size();
            if (points_w <= 0 || points_h <= 0 || pixels.width <= 0
                || pixels.height <= 0)
                return;
            // Drawable pixels per window point (2 on a HiDPI panel).
            const int w = size.width * points_w / pixels.width;
            const int h = size.height * points_h / pixels.height;
            SDL_Rect bounds;
            if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(window),
                                           &bounds) != 0
                || w > bounds.w || h > bounds.h)
                return;
            SDL_SetWindowSize(window, w, h);
        };

        // Record how the current connection ended and decide what happens next.
        const auto connection_ended = [&](ConnectionOutcome outcome,
                                          std::string reason) {
            live = false;
            established = false;
            link.teardown();
            end.outcome = outcome;
            end.reason = reason;
            last_modifiers = 0;
            const auto now = Clock::now();
            if (schedule.connection_ended(outcome, now)) {
                const auto delay = std::chrono::duration_cast<
                    std::chrono::milliseconds>(schedule.due_at() - now);
                std::cerr << "connection ended (" << reason << "); reconnecting in "
                          << delay.count() << " ms (attempt "
                          << schedule.attempts_made() + 1 << " of "
                          << reconnect.max_attempts << ")\n";
                return;
            }
            if (schedule.exhausted())
                end.reason += " -- reconnect gave up after "
                    + std::to_string(schedule.attempts_made())
                    + (schedule.attempts_made() == 1 ? " attempt" : " attempts");
            running = false;
        };

        // Start the handshake on the link's freshly connected transport.
        const auto start_connection = [&](bool first) {
            Transport* transport = link.transport();
            if (!first)
                session.reset();
            session.set_sender([&, transport](std::span<const std::uint8_t> bytes) {
                return transport->send_all(bytes, socket_error);
            });
            // Session::start() returns void and ignores whether the handshake
            // went out; the sender only assigns socket_error on failure, so an
            // empty string afterwards means every handshake message was written.
            socket_error.clear();
            session.start();
            if (!socket_error.empty()) {
                const std::string reason = "handshake send to "
                    + transport->describe() + " failed: " + socket_error;
                if (first)
                    throw std::runtime_error(reason);
                std::cerr << reason << '\n';
                ConnectionResult result;
                result.connected = true;
                result.peer_closed = transport->peer_closed();
                result.connection_reset = transport->connection_reset();
                result.through_forward = link.through_forward();
                connection_ended(classify_connection(result), reason);
                return;
            }
            live = true;
            established = false;
            end.status = exit_status::ok;
            end.refused_no_data = false;
            end.reason.clear();
            shell.set_link(DesktopShell::Link::connecting, link.route());
        };

        // The connection-ending path for a live transport: gather what the
        // transport and the session observed and classify it once.
        const auto observe_end = [&](bool receive_failed) {
            Transport* transport = link.transport();
            ConnectionResult result;
            result.connected = true;
            result.message_count = session.message_count();
            result.server_closed = session.server_closed();
            result.peer_closed = transport->peer_closed();
            result.connection_reset = transport->connection_reset();
            result.through_forward = link.through_forward();
            const bool orderly = result.peer_closed || result.server_closed;
            end.refused_no_data = orderly && result.message_count == 0;
            const bool ok
                = report_session_end(orderly, result.message_count, socket_error);
            end.status = ok ? exit_status::ok : exit_status::failed;
            const ConnectionOutcome outcome = classify_connection(result);
            std::string reason(outcome_name(outcome));
            if (receive_failed && !orderly && !socket_error.empty())
                reason += ": " + socket_error;
            connection_ended(outcome, reason);
        };

        // Events that arrive while a reconnect is being built: the attempt
        // blocks this thread between stage boundaries, so only a close request
        // is honoured -- input has no transport to go to.
        const auto drain_during_connect = [&]() {
            SDL_Event event {};
            while (SDL_PollEvent(&event) != 0) {
                if (event.type == SDL_QUIT)
                    quit_requested = true;
                else if (event.type == SDL_WINDOWEVENT
                         && event.window.event == SDL_WINDOWEVENT_EXPOSED)
                    dirty = true;
            }
        };

        const auto attempt_reconnect = [&]() {
            schedule.attempt_started();
            ++reconnects;
            const std::string attempt = "attempt "
                + std::to_string(schedule.attempts_made());
            shell.set_link(DesktopShell::Link::connecting, attempt);
            present();
            ConnectionOutcome failure = ConnectionOutcome::connect_failed;
            std::string error;
            const bool ok = link.connect(
                [&](std::string_view stage) {
                    shell.set_link(DesktopShell::Link::connecting,
                                   attempt + ": " + std::string(stage));
                    present();
                    drain_during_connect();
                },
                failure, error);
            drain_during_connect();
            if (!ok) {
                std::cerr << "reconnect failed: " << error << '\n';
                end.status = exit_status::failed;
                connection_ended(failure, error);
                return;
            }
            start_connection(false);
        };

        start_connection(true);
        SDL_StartTextInput();
        while (running) {
            auto now = Clock::now();
            if (quit_requested) {
                end.user_ended = true;
                break;
            }
            if (schedule.due(now)) {
                attempt_reconnect();
                continue;
            }

            SDL_Event event {};
            bool have_event = false;
            if (live) {
                Transport* transport = link.transport();
                const int received = transport->receive(buffer, 2, socket_error);
                if (received < 0) {
                    observe_end(true);
                    dirty = true;
                    continue;
                }
                if (received > 0) {
                    bytes_received += static_cast<std::size_t>(received);
                    session.ingest(
                        std::span(buffer.data(), static_cast<std::size_t>(received)));
                    dirty = true;
                }
                // The handshake succeeded once the server sent anything back:
                // a refused candidate never gets a single message
                // (classify_connection's refused case). This is the moment a
                // connection counts as made -- not when it later ends cleanly,
                // which a dropped session never does.
                if (!established && session.message_count() > 0) {
                    established = true;
                    schedule.established();
                    link.established();
                    shell.set_link(DesktopShell::Link::connected, link.route());
                }
                // RP_CLOSE_CONNECTION arrives before the EOF does. Stop on it
                // rather than idling against a socket that will never say
                // anything again.
                if (session.server_closed()) {
                    observe_end(false);
                    dirty = true;
                    continue;
                }
            } else {
                // No transport: block on the window until the next event or
                // the reconnect deadline -- no busy loop, and no sleep the user
                // cannot interrupt. The wait is capped at a second so the
                // countdown on the bar stays current.
                int timeout = 1000;
                if (schedule.pending()) {
                    const auto delay = std::chrono::duration_cast<
                        std::chrono::milliseconds>(schedule.due_at() - now);
                    // Wake on the next whole second of the countdown, so the
                    // number on the bar ticks on time, or on the deadline.
                    const long long to_tick = delay.count() % 1000;
                    timeout = static_cast<int>(std::clamp<long long>(
                        to_tick == 0 ? 1000 : to_tick, 1, 1000));
                    shell.set_link(DesktopShell::Link::waiting,
                                   reconnect_countdown(schedule, now));
                } else {
                    shell.set_link(DesktopShell::Link::lost, end.reason);
                }
                if (const auto settles = settler.deadline()) {
                    const auto delay = std::chrono::duration_cast<
                        std::chrono::milliseconds>(*settles - now);
                    timeout = std::min(timeout, static_cast<int>(std::clamp<long long>(
                                                    delay.count(), 1, 1000)));
                }
                if (dirty || shell.dirty())
                    present();
                have_event = SDL_WaitEventTimeout(&event, timeout) != 0;
            }

            const auto modifiers_now = [&]() {
                return modifier_mask(SDL_GetModState());
            };
            const auto update_modifiers = [&]() {
                const auto current = modifiers_now();
                if (current != last_modifiers) {
                    last_modifiers = current;
                    send(InputEncoder::modifiers_changed(current));
                }
            };

            now = Clock::now();
            while (have_event || SDL_PollEvent(&event) != 0) {
                have_event = false;
                switch (event.type) {
                case SDL_QUIT:
                    quit_requested = true;
                    break;
                case SDL_MOUSEMOTION:
                    if (!shell.pointer_move(event.motion.x, event.motion.y, now))
                        send(InputEncoder::mouse_moved(
                            static_cast<float>(event.motion.x),
                            static_cast<float>(event.motion.y)));
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP: {
                    // The event's own coordinates, which SDL has already mapped
                    // through the logical size; SDL_GetMouseState() reports
                    // window pixels, which differ once the window is scaled
                    // (full screen).
                    const int x = event.button.x;
                    const int y = event.button.y;
                    const auto state = SDL_GetMouseState(nullptr, nullptr);
                    if (event.type == SDL_MOUSEBUTTONDOWN) {
                        if (event.button.button == SDL_BUTTON_LEFT
                            && shell.pointer_press(x, y, now))
                            break;
                        send(InputEncoder::mouse_down(
                            static_cast<float>(x), static_cast<float>(y),
                            button_mask(state),
                            std::max<int>(1, event.button.clicks)));
                    } else {
                        if (event.button.button == SDL_BUTTON_LEFT
                            && shell.pointer_release())
                            break;
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
                    if (is_shell_hotkey(event.key.keysym)) {
                        if (event.type == SDL_KEYDOWN && event.key.repeat == 0)
                            shell.toggle();
                        break;
                    }
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
                    if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        dirty = true;
                        if (shell.fit_to_window())
                            settler.window_resized(drawable_size(), now);
                    }
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        last_modifiers = 0;
                        send(InputEncoder::modifiers_changed(0));
                    }
                    break;
                default:
                    break;
                }
            }

            switch (shell.take_action()) {
            case DesktopShell::Action::none:
            case DesktopShell::Action::stats:
                break;
            case DesktopShell::Action::disconnect:
                end.user_ended = true;
                running = false;
                break;
            case DesktopShell::Action::reconnect:
                std::cerr << "reconnect requested\n";
                if (live) {
                    live = false;
                    established = false;
                    link.teardown();
                }
                schedule.request_now(Clock::now());
                break;
            case DesktopShell::Action::repaint:
                if (live) {
                    const RepaintLever lever = request_repaint(session);
                    std::cerr << "full repaint requested ("
                              << (lever == RepaintLever::resync ? "RP_RESYNC"
                                  : lever == RepaintLever::display_mode
                                      ? "RP_UPDATE_DISPLAY_MODE"
                                      : "send failed")
                              << ")\n";
                }
                break;
            case DesktopShell::Action::fullscreen: {
                const bool full = (SDL_GetWindowFlags(window)
                                   & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                if (SDL_SetWindowFullscreen(
                        window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
                    std::cerr << "full screen: " << SDL_GetError() << '\n';
                dirty = true;
                break;
            }
            case DesktopShell::Action::shortcut:
                if (const RemoteShortcut* chord = shell.take_shortcut()) {
                    for (const auto& message :
                         encode_shortcut(*chord, last_modifiers))
                        send(message);
                }
                break;
            case DesktopShell::Action::display_size:
                if (const auto choice = shell.take_display_size()) {
                    using Kind = DisplaySizeChoice::Kind;
                    switch (choice->kind) {
                    case Kind::preset:
                        settler.cancel();
                        if (apply_display_size(choice->size))
                            fit_window_to(choice->size);
                        break;
                    case Kind::window:
                        settler.cancel();
                        apply_display_size(drawable_size());
                        break;
                    case Kind::fit:
                        if (shell.fit_to_window())
                            apply_display_size(drawable_size());
                        else
                            settler.cancel();
                        break;
                    }
                }
                break;
            }

            now = Clock::now();
            if (shell.fit_to_window()) {
                if (const auto settled = settler.take(shell.desktop_size(), now))
                    apply_display_size(*settled);
            }

            now = Clock::now();
            shell.tick(now);
            if (now - stats_since >= std::chrono::milliseconds(500)) {
                const double seconds
                    = std::chrono::duration<double>(now - stats_since).count();
                frame_rate = static_cast<double>(frames) / seconds;
                frames = 0;
                stats_since = now;
                if (shell.stats_visible()) {
                    char line[96];
                    std::vector<std::string> lines;
                    lines.push_back("messages: "
                                    + std::to_string(session.message_count()));
                    lines.push_back("received: " + format_bytes(bytes_received));
                    if (session.compressed()) {
                        const auto wire = session.compressed_wire_bytes();
                        const auto plain = session.compressed_plain_bytes();
                        std::snprintf(line, sizeof line, "zstd: %s -> %s (%.1fx)",
                                      format_bytes(wire).c_str(),
                                      format_bytes(plain).c_str(),
                                      wire == 0 ? 0.0
                                                : static_cast<double>(plain)
                                                    / static_cast<double>(wire));
                        lines.emplace_back(line);
                    } else {
                        lines.emplace_back("compression: off");
                    }
                    lines.push_back("blocking replies: "
                                    + std::to_string(session.sync_replies()));
                    std::snprintf(line, sizeof line, "frames/s: %.1f", frame_rate);
                    lines.emplace_back(line);
                    std::snprintf(line, sizeof line,
                                  "session %08x  generation %u  reconnects %d",
                                  session.session_id(), session.generation(),
                                  reconnects);
                    lines.emplace_back(line);
                    shell.set_stats_lines(std::move(lines));
                }
            }

            const bool overlay_changed = shell.dirty()
                && (shell.visible() || shell.stats_visible());
            if (dirty || overlay_changed || shell.visible() != shell_was_visible
                || shell.stats_visible() != stats_were_visible)
                present();
        }
        link.teardown();
        release();
        return end;
    } catch (...) {
        link.teardown();
        release();
        throw;
    }
}

// The CLI path: resolve a transport from the options, connect it inline
// (reporting a connect failure exactly as before, with the same exit code),
// then run the desktop. Automatic reconnect is off here -- the command line has
// no flag for it -- but the toolbar's Reconnect rebuilds from the same options.
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
    const std::string name = transport->describe();
    OptionsLink link(options.transport, std::move(transport));
    try {
        // A user-initiated close keeps the status of the connection it closed:
        // ok, unless that connection had already failed.
        return run_desktop(std::move(options), link, name, {}).status;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        SDL_Quit();
        return exit_status::usage;
    }
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

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    create_window_and_renderer("DeBeOS Remote \xE2\x80\x94 Connections", width, height,
                               SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN,
                               window, renderer);
    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width,
        height);
    if (texture == nullptr)
        throw std::runtime_error(std::string("SDL_CreateTexture: ")
                                 + SDL_GetError());

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

            // Connect with a visible progress screen, then run the desktop. The
            // ManagedConnection the connect produced owns any ssh child (the
            // tunnel route); it moves into the ProfileLink with the transport,
            // which tears both down when a connection ends or the desktop
            // closes, before it builds a replacement -- so the ssh child is
            // never orphaned and two forwards are never up at once. A connect
            // failure shows an actionable error with Back/Retry instead of a
            // black window; Retry re-runs the connect for the same profile,
            // Back returns to the library. Either way the failure is recorded on
            // the library card too.
            for (;;) {
                ConnectOutcome outcome = run_connect(*picked);
                if (outcome.result.ok) {
                    std::cerr << "route: " << outcome.result.route_note << '\n';
                    Options session_options = options;
                    session_options.width = picked->width;
                    session_options.height = picked->height;
                    session_options.size_explicit = true;
                    const std::string name
                        = picked->name.empty() ? picked->host : picked->name;
                    // The link stamps lastConnectedAt the moment the server
                    // answers the handshake, so a session that later drops still
                    // records its success.
                    ProfileLink link(*picked, library, std::move(outcome.result));
                    std::optional<DesktopEnd> end;
                    try {
                        end = run_desktop(session_options, link, name,
                                          reconnect_config_for(*picked));
                    } catch (const std::exception& error) {
                        std::cerr << error.what() << '\n';
                        library.set_last_error(picked->id, error.what());
                    }
                    if (end && end->refused_no_data) {
                        const ConnectError e = classify_connect_failure(
                            ConnectFailPoint::session_refused, ConnectFailure::none,
                            "the server closed the connection before sending any"
                            " drawing");
                        library.set_last_error(picked->id, e.summary());
                        std::cerr << "session for '" << picked->name
                                  << "' was refused: " << e.summary() << '\n';
                    } else if (end && end->status != exit_status::ok
                               && !end->reason.empty()) {
                        library.set_last_error(picked->id,
                                               "Connection lost: " + end->reason);
                        std::cerr << "session for '" << picked->name
                                  << "' ended: " << end->reason << '\n';
                    }
                    const SaveResult saved = library.save();
                    if (!saved.ok)
                        std::cerr << "connection library: could not save: "
                                  << saved.message << '\n';
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
