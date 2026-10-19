#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/connect_screen.hpp"
#include "haiku_remote/desktop_session.hpp"
#include "haiku_remote/desktop_shell.hpp"
#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/launch_arguments.hpp"
#include "haiku_remote/library_screen.hpp"
#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/png_writer.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/profile_library.hpp"
#include "haiku_remote/reconnect.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/test_script.hpp"
#include "haiku_remote/transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <atomic>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace haiku_remote;

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    // == launch.transport once parsed; what the endpoint path connects with.
    TransportOptions transport;
    int width = 1280;
    int height = 800;
    bool size_explicit = false;
    // The shared launch arguments (launch_arguments.hpp) and what they select.
    // `library` -- nothing connection-selecting was given -- opens the
    // connection library; every other target connects straight away and
    // bypasses it, leaving the CLI launch path untouched.
    LaunchArguments launch;
    LaunchTarget target = LaunchTarget::library;
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
        if (parse_launch_argument(options.launch, argument, value)) {
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
                      << "\n  [--width PX] [--height PX]\n"
                      << launch_usage()
                      << "\n  [--profiles connections.json]   alone: open the"
                         " library from that file\n"
                         "  (no connection argument at all opens the"
                         " connection library)\n\n"
                      << exit_status_usage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }
    options.target = resolve_launch_target(options.launch);
    options.transport = options.launch.transport;
    return options;
}

// Closing a window and quitting the application are different requests
// (issue #1). Closing a session's window returns to the library. SIGTERM and
// SIGINT -- which SDL's own signal handlers turn into SDL_QUIT; on Windows
// the console controls, through on_console_control() below -- a session
// manager's logout, or Cmd-Q on macOS quit the application, tearing the
// session and its tunnel down on the way out; they used to land back in the
// library, because the desktop took every SDL_QUIT for a window close.
//
// They are told apart by origin. A window close is SDL_WINDOWEVENT_CLOSE.
// SDL_QUIT never is one: sdl_init() turns off SDL's habit of following the
// last window's close with an SDL_QUIT, and on an SDL too old for that hint
// (< 2.0.22) the SDL_QUIT echoing a close is recognised and dropped below.
// (Between two windows SDL's handlers are not installed, so a signal there
// takes its default action; the guard of any ssh child sees its lifeline
// close and stops it -- managed_transport.cpp, run_guard().)
bool& app_quit_requested()
{
    static bool requested = false;
    return requested;
}

enum class EndRequest { none, close_window, quit_app };

// What an event asks of the window it arrived in; one per window.
class EndRequests {
public:
    EndRequest classify(const SDL_Event& event)
    {
        if (event.type == SDL_WINDOWEVENT
            && event.window.event == SDL_WINDOWEVENT_CLOSE) {
            window_closed_ = true;
            return EndRequest::close_window;
        }
        if (event.type != SDL_QUIT || window_closed_)
            return EndRequest::none; // an old SDL echoing the window close
        if (!app_quit_requested())
            std::cerr << "quit requested (SIGTERM, SIGINT or the system):"
                         " ending the session and exiting\n";
        app_quit_requested() = true;
        return EndRequest::quit_app;
    }

private:
    bool window_closed_ = false;
};

#if defined(_WIN32)
// Windows has no SIGTERM to send. Ctrl+C does reach SDL, as the SIGINT the
// CRT raises for it, but only while a window is open; and Ctrl+Break and the
// console's close (logoff, shutdown) are no signal SDL handles, so they took
// the default action -- measured on Windows Server 2022: the GUI killed
// outright with its desktop up (exit 0xC000013A), the session never ended and
// the library never saved; only the kill-on-close job stopped ssh. This
// handler makes each of them the SDL_QUIT a signal is on POSIX, so the
// window's own loop ends the session and the app exits 0
// (tests/e2e/profile_e2e.py, the "quit by CTRL_*" cases).
//
// It runs on a thread the system creates. SDL_PushEvent is thread-safe, but
// fails between two windows (SDL is not initialised then), so the request is
// also latched and sdl_init() replays it into the next window.
std::atomic<bool> g_console_quit {false};
HANDLE g_main_done = nullptr;

void push_quit()
{
    SDL_Event event {};
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
}

BOOL WINAPI on_console_control(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        break;
    default:
        return FALSE;
    }
    g_console_quit = true;
    push_quit();
    // A close, logoff or shutdown ends the process when this returns: wait for
    // main() to finish, then let its own exit end the process (and this thread)
    // so the exit status is main()'s, not the system's termination.
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && g_main_done != nullptr
        && ::WaitForSingleObject(g_main_done, 4500) == WAIT_OBJECT_0)
        ::Sleep(INFINITE);
    return TRUE;
}

void install_console_quit()
{
    g_main_done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::SetConsoleCtrlHandler(on_console_control, TRUE);
}

struct MainDone {
    ~MainDone()
    {
        if (g_main_done != nullptr)
            ::SetEvent(g_main_done);
    }
};
#endif

void sdl_init()
{
    SDL_SetMainReady();
#ifdef SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        throw std::runtime_error(SDL_GetError());
#if defined(_WIN32)
    if (g_console_quit)
        push_quit(); // asked while no window was open: this one ends at once
#endif
}

// The window the test script's "close" acts on: the one open now.
Uint32& current_window_id()
{
    static Uint32 id = 0;
    return id;
}

// A user closing the window: SDL_WINDOWEVENT_CLOSE, never SDL_QUIT, which
// would quit the application (EndRequests).
void test_script_close_window_sdl()
{
    SDL_Event close {};
    close.type = SDL_WINDOWEVENT;
    close.window.event = SDL_WINDOWEVENT_CLOSE;
    close.window.windowID = current_window_id();
    SDL_PushEvent(&close);
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

bool is_text_key(SDL_Keycode key)
{
    return key >= 0x20 && key != SDLK_DELETE
        && (key & SDLK_SCANCODE_MASK) == 0;
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
    // SDL attaches an OpenGL context to every window on macOS when no graphics
    // API is asked for ("some platforms will assume usage of OpenGL",
    // SDL_HINT_VIDEO_EXTERNAL_CONTEXT). Under the offscreen and dummy drivers
    // there is no GL to load, so SDL_CreateWindow itself failed with "Could
    // not initialize OpenGL / GLES library" (measured on macOS 26 with
    // Homebrew's sdl2-compat) before any renderer fallback could run. Those
    // drivers have no display to accelerate, so tell SDL not to presume GL.
    if (const char* driver = SDL_GetCurrentVideoDriver(); driver != nullptr
        && (std::string_view(driver) == "offscreen"
            || std::string_view(driver) == "dummy"))
        SDL_SetHint(SDL_HINT_VIDEO_EXTERNAL_CONTEXT, "1");
    const auto make_window = [&]() {
        window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED, width, height,
                                  window_flags);
        if (window == nullptr)
            throw std::runtime_error(std::string("SDL_CreateWindow: ")
                                     + SDL_GetError());
    };
    make_window();
    current_window_id() = SDL_GetWindowID(window);
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
// The desktop's state machine is DesktopController (desktop_session.cpp),
// shared with the X11 front end; this is the SDL window around it: one window,
// a texture per layer (the session's surface, the bar, the statistics panel),
// and SDL's events translated into the controller's and the shell's input.

// The SDL front end's toolbar hotkey, Ctrl+Alt+Home -- the chord Microsoft's
// RDP client uses for its connection bar, so it is free of the remote's own
// shortcuts. SDL's Alt is Haiku's Command (modifier_mask()).
bool is_shell_hotkey(const SDL_Keysym& key)
{
    return key.sym == SDLK_HOME && (key.mod & KMOD_CTRL) != 0
        && (key.mod & KMOD_ALT) != 0;
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

// What the controller asks of the SDL window.
class SdlDesktopWindow final : public DesktopWindow {
public:
    SdlDesktopWindow(SDL_Window* window, SDL_Renderer* renderer)
        : window_(window), renderer_(renderer) {}

    void set_desktop_size(DisplaySize size) override
    {
        // The renderer's logical size follows, so input keeps mapping onto
        // desktop pixels.
        SDL_RenderSetLogicalSize(renderer_, size.width, size.height);
    }

    DisplaySize drawable_size() override
    {
        DisplaySize size;
        if (SDL_GetRendererOutputSize(renderer_, &size.width, &size.height) != 0)
            size = {};
        return size;
    }

    void fit_window_to(DisplaySize size) override
    {
        if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
            return;
        int points_w = 0;
        int points_h = 0;
        SDL_GetWindowSize(window_, &points_w, &points_h);
        const DisplaySize pixels = drawable_size();
        if (points_w <= 0 || points_h <= 0 || pixels.width <= 0 || pixels.height <= 0)
            return;
        // Drawable pixels per window point (2 on a HiDPI panel).
        const int w = size.width * points_w / pixels.width;
        const int h = size.height * points_h / pixels.height;
        SDL_Rect bounds;
        if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(window_), &bounds) != 0
            || w > bounds.w || h > bounds.h)
            return;
        SDL_SetWindowSize(window_, w, h);
    }

    void toggle_fullscreen() override
    {
        const bool full
            = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
        if (SDL_SetWindowFullscreen(window_, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP)
            != 0)
            std::cerr << "full screen: " << SDL_GetError() << '\n';
    }

    void reconnected() override
    {
        // HAIKU_REMOTE_TEST_SCRIPT: a "session:" step queued behind
        // reconnect:resume watches the reconnected session.
        if (!test_wait)
            test_wait = test_script_pixel_wait();
    }

    // HAIKU_REMOTE_TEST_SCRIPT's "session:" step, if any (a no-op otherwise):
    // checked against the session surface after each frame.
    std::optional<TestPixelWait> test_wait = test_script_pixel_wait();

private:
    SDL_Window* window_;
    SDL_Renderer* renderer_;
};

// Run a connected desktop over `link`, whose transport is already connected.
// Auto-reconnect follows `reconnect`; a manual Reconnect from the toolbar works
// either way.
DesktopEnd run_desktop(DesktopSize size, SessionLink& link, const std::string& name,
                       const ReconnectConfig& reconnect)
{
    sdl_init();
    // The Session the connect's handshake ran on (it has ingested the server's
    // first messages) is continued at its own size; a link without one gets a
    // fresh Session.
    std::unique_ptr<Session> prestarted = link.take_session();
    // Default the remote desktop to the local display size so the window does
    // not come up larger than the screen (e.g. a 1280x800 default on a 1024x768
    // panel). --width/--height still override.
    if (prestarted != nullptr) {
        size.width = prestarted->surface().width();
        size.height = prestarted->surface().height();
    } else if (!size.explicit_size) {
        SDL_Rect bounds;
        if (SDL_GetDisplayUsableBounds(0, &bounds) == 0 && bounds.w > 0
            && bounds.h > 0) {
            size.width = std::min(bounds.w, Surface::max_dimension);
            size.height = std::min(bounds.h, Surface::max_dimension);
        }
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    create_window_and_renderer(
        "Haiku Remote", size.width, size.height,
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE,
        window, renderer);
    SDL_RenderSetLogicalSize(renderer, size.width, size.height);
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
        // The worker wakes the event loop with this (SDL_PushEvent is
        // thread-safe), so a reconnect's progress needs no timer.
        const Uint32 wake_event = SDL_RegisterEvents(1);
        const auto wake = [wake_event] {
            if (wake_event == static_cast<Uint32>(-1))
                return;
            SDL_Event event {};
            event.type = wake_event;
            SDL_PushEvent(&event);
        };
        SdlDesktopWindow host(window, renderer);
        DesktopController desktop(link, name, size.width, size.height, reconnect,
                                  host, std::move(prestarted), wake);
        Session& session = desktop.session();
        DesktopShell& shell = desktop.shell();
        bool quit_requested = false; // the window closed, or the app quit
        EndRequests ends;

        // Present the frame: the session's surface, with the bar and the stats
        // panel composited over it as separate textures. Neither is ever drawn
        // into the session's surface.
        const auto present = [&]() {
            // While a reconnect runs, its worker owns the session (it resets
            // it and ingests the handshake): the last frame stays on screen.
            if (desktop.surface_changed() || texture == nullptr)
                upload(renderer, texture, session.surface());
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            const bool bar_shown = shell.visible();
            const bool stats_shown = shell.stats_visible();
            if (bar_shown && (shell.dirty() || bar_texture == nullptr))
                upload(renderer, bar_texture, shell.render_bar());
            if (stats_shown && (shell.dirty() || stats_texture == nullptr))
                upload(renderer, stats_texture, shell.render_stats());
            if (stats_shown) {
                const SDL_Rect r = to_sdl(shell.stats_rect());
                SDL_RenderCopy(renderer, stats_texture, nullptr, &r);
            }
            if (bar_shown) {
                const SDL_Rect r = to_sdl(shell.bar_rect());
                SDL_RenderCopy(renderer, bar_texture, nullptr, &r);
            }
            SDL_RenderPresent(renderer);
            desktop.presented();
            if (host.test_wait && !desktop.connecting()
                && test_script_check_frame(*host.test_wait, session))
                host.test_wait.reset();
        };

        desktop.start();
        SDL_StartTextInput();
        while (desktop.running()) {
            auto now = Clock::now();
            if (quit_requested) {
                desktop.user_quit();
                break;
            }
            if (desktop.advance(now))
                continue;

            SDL_Event event {};
            bool have_event = false;
            if (desktop.live()) {
                if (desktop.receive(2))
                    continue;
            } else {
                // No transport: block on the window until the next event or
                // the reconnect deadline -- no busy loop, and no sleep the user
                // cannot interrupt.
                const int timeout = desktop.idle_wait_ms(now);
                if (desktop.needs_present())
                    present();
                have_event = SDL_WaitEventTimeout(&event, timeout) != 0;
            }

            now = Clock::now();
            while (have_event || SDL_PollEvent(&event) != 0) {
                have_event = false;
                switch (event.type) {
                case SDL_QUIT:
                    if (ends.classify(event) == EndRequest::quit_app)
                        quit_requested = true;
                    break;
                case SDL_MOUSEMOTION:
                    if (!shell.pointer_move(event.motion.x, event.motion.y, now))
                        desktop.send(InputEncoder::mouse_moved(
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
                        desktop.send(InputEncoder::mouse_down(
                            static_cast<float>(x), static_cast<float>(y),
                            button_mask(state), std::max<int>(1, event.button.clicks)));
                    } else {
                        if (event.button.button == SDL_BUTTON_LEFT
                            && shell.pointer_release())
                            break;
                        desktop.send(InputEncoder::mouse_up(
                            static_cast<float>(x), static_cast<float>(y),
                            button_mask(state)));
                    }
                    break;
                }
                case SDL_MOUSEWHEEL:
                    desktop.send(InputEncoder::mouse_wheel(
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
                    desktop.set_modifiers(modifier_mask(SDL_GetModState()));
                    const bool down = event.type == SDL_KEYDOWN;
                    const auto special = special_text(event.key.keysym.sym);
                    if (!special.empty()) {
                        desktop.send(InputEncoder::key(
                            down, special, first_utf8_scalar(special),
                            haiku_key(event.key.keysym.scancode)));
                    } else if (!is_text_key(event.key.keysym.sym)) {
                        desktop.send(InputEncoder::key(
                            down, {}, 0, haiku_key(event.key.keysym.scancode)));
                    }
                    break;
                }
                case SDL_TEXTINPUT: {
                    const std::string_view text(event.text.text);
                    const auto raw = first_utf8_scalar(text);
                    desktop.send(InputEncoder::key(true, text, raw, 0));
                    desktop.send(InputEncoder::key(false, text, raw, 0));
                    break;
                }
                case SDL_WINDOWEVENT:
                    if (ends.classify(event) == EndRequest::close_window)
                        quit_requested = true; // back to the library
                    if (event.window.event == SDL_WINDOWEVENT_EXPOSED)
                        desktop.mark_dirty();
                    if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                        desktop.window_resized(now);
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                        desktop.focus_lost();
                    break;
                default:
                    break;
                }
            }

            desktop.after_events(Clock::now());
            if (desktop.needs_present())
                present();
        }
        DesktopEnd end = desktop.finish();
        release();
        return end;
    } catch (...) {
        link.teardown();
        release();
        throw;
    }
}

// Open a window and run the connect for `profile` on a worker thread
// (AsyncConnect), painting the progress checklist, the elapsed time and a
// Cancel button while ssh and the sockets block. Cancel (the button, Escape,
// or closing the window) stops the attempt: its ssh child is killed and
// reaped, its socket shut down, and nothing is left running when this
// returns. On success the window is torn down and the live transport returned.
// On failure the window stays up showing the actionable error and the
// diagnostic log until the user picks Back or Retry (or closes it, treated as
// Back); Copy diagnostics puts the whole log on the clipboard.
ConnectOutcome run_connect(const ConnectionProfile& profile)
{
    ConnectOutcome out;
    const std::string title = profile.name.empty() ? profile.host : profile.name;

    sdl_init();

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

    // The screen draws `shown`, the UI thread's copy of the worker's flow.
    ConnectFlow shown(plan_launch(profile));
    ConnectScreen ui(shown, title, width, height);
    bool closed = false;
    EndRequests ends;

    const auto blit = [&]() {
        const Surface& surface = ui.render();
        SDL_UpdateTexture(texture, nullptr, surface.pixels().data(),
                          surface.stride());
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    };
    // One translation of SDL input into ConnectScreen input, for every phase.
    const auto handle = [&](const SDL_Event& event) {
        if (ends.classify(event) != EndRequest::none)
            closed = true;
        switch (event.type) {
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
            else if (event.key.keysym.sym == SDLK_TAB)
                ui.key((event.key.keysym.mod & KMOD_SHIFT) != 0
                           ? ConnectScreen::Key::back_tab
                           : ConnectScreen::Key::tab);
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_EXPOSED)
                ui.mark_dirty();
            break;
        default:
            break;
        }
    };

    // The worker wakes this loop with a user event at every stage boundary,
    // trust question and at the end; SDL_PushEvent is thread-safe.
    const Uint32 wake_event = SDL_RegisterEvents(1);
    ConnectOptions options;
    options.session_log = [](std::string_view line) { write_stderr_line(line); };
    AsyncConnect attempt(profile, options, [wake_event] {
        if (wake_event == static_cast<Uint32>(-1))
            return;
        SDL_Event event {};
        event.type = wake_event;
        SDL_PushEvent(&event);
    });

    bool prompt_showing = false;
    bool refresh = true; // the worker published, or the clock ticked
    std::size_t stages_reported = 0;
    while (!attempt.finished()) {
        if (refresh) {
            shown = attempt.flow();
            ui.mark_dirty(); // `shown` changed under the screen
            refresh = false;
            test_script_report_stages(attempt.log(), stages_reported);
        }
        ui.set_elapsed(attempt.elapsed());
        ui.set_cancelling(attempt.cancel_requested());
        const auto question = attempt.trust_question();
        if (question && !prompt_showing) {
            ui.show_trust_prompt(*question);
            prompt_showing = true;
        } else if (!question && prompt_showing) {
            ui.clear_trust_prompt();
            prompt_showing = false;
        }
        if (ui.dirty())
            blit();

        // Wake for input, for the worker, or once a quarter second for the
        // elapsed-time clock.
        SDL_Event event {};
        if (SDL_WaitEventTimeout(&event, 250) != 0) {
            do {
                if (event.type == wake_event)
                    refresh = true;
                else
                    handle(event);
            } while (SDL_PollEvent(&event) != 0);
        }
        if (closed && !attempt.cancel_requested())
            attempt.cancel();
        switch (ui.take_action()) {
        case ConnectScreen::Action::cancel:
            attempt.cancel();
            break;
        case ConnectScreen::Action::trust:
            attempt.answer_trust(TrustChoice::trust);
            break;
        case ConnectScreen::Action::replace:
            attempt.answer_trust(TrustChoice::replace);
            break;
        case ConnectScreen::Action::reject:
            attempt.answer_trust(TrustChoice::reject);
            break;
        default:
            break;
        }
    }

    out.result = attempt.take_result();
    shown = attempt.flow();
    test_script_report_stages(attempt.log(), stages_reported);
    // A cancel that raced a successful connect still wins: the user said stop.
    if (out.result.cancelled || attempt.cancel_requested()) {
        out.result = ConnectResult {};
        out.cancelled = true;
        out.action = ConnectScreen::Action::back;
    } else if (!out.result.ok) {
        if (test_script_take("connect-error") == std::optional<std::string>("back")) {
            std::cerr << "e2e: connect failed, leaving the error screen by Back\n";
            closed = true;
        }
        ui.clear_trust_prompt();
        ui.set_diagnostics(attempt.log().lines());
        ui.mark_dirty();
        if (test_script_take("connect-error") == std::optional<std::string>("back")) {
            std::cerr << "e2e: connect failed, leaving the error screen by Back\n";
            closed = true;
        }
        while (!closed && out.action == ConnectScreen::Action::none) {
            if (ui.dirty())
                blit();
            SDL_Event event {};
            if (SDL_WaitEvent(&event) == 0) {
                closed = true;
                break;
            }
            do {
                handle(event);
            } while (!closed && SDL_PollEvent(&event) != 0);
            const ConnectScreen::Action action = ui.take_action();
            if (action == ConnectScreen::Action::copy_log) {
                const std::string text = attempt.log().text();
                if (SDL_SetClipboardText(text.c_str()) == 0)
                    ui.note_copied();
                else
                    std::cerr << "clipboard: " << SDL_GetError() << '\n';
            } else if (action == ConnectScreen::Action::back
                       || action == ConnectScreen::Action::retry) {
                out.action = action;
            }
        }
        if (closed)
            out.action = ConnectScreen::Action::back;
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
    sdl_init();

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
    EndRequests ends;
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

    if (test_script_active())
        std::cerr << "e2e: library shown with " << library.size() << " profiles\n";
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
            test_script_library_step(ui, library, [&](int x, int y) {
                SDL_Event press {};
                press.type = SDL_MOUSEBUTTONDOWN;
                press.button.button = SDL_BUTTON_LEFT;
                press.button.state = SDL_PRESSED;
                press.button.clicks = 1;
                press.button.x = static_cast<int>(static_cast<float>(x) / pointer_scale_x);
                press.button.y = static_cast<int>(static_cast<float>(y) / pointer_scale_y);
                SDL_PushEvent(&press);
            });
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
            case SDL_WINDOWEVENT:
                // Closing the library and quitting both end the run.
                if (ends.classify(event) != EndRequest::none) {
                    running = false;
                    closed = true;
                }
                if (event.type == SDL_QUIT)
                    break;
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

// The SDL windows of the shared app flow (desktop_session.hpp).
class SdlFrontEnd final : public FrontEnd {
public:
    std::optional<ConnectionProfile> run_library(ConnectionLibrary& library) override
    {
        return ::run_library(library);
    }
    ConnectOutcome run_connect(const ConnectionProfile& profile) override
    {
        return ::run_connect(profile);
    }
    DesktopEnd run_desktop(DesktopSize size, SessionLink& link, const std::string& name,
                           const ReconnectConfig& reconnect) override
    {
        return ::run_desktop(size, link, name, reconnect);
    }
    bool quit_requested() const override { return app_quit_requested(); }
};

int main(int argc, char** argv)
{
#if defined(_WIN32)
    install_console_quit();
    const MainDone main_done;
#endif
    try {
        Options options = parse_options(argc, argv);
        set_test_script_close_window(test_script_close_window_sdl);
        SdlFrontEnd front_end;
        const DesktopSize size {options.width, options.height, options.size_explicit};
        // A connection named on the command line connects straight away: the
        // automation/debugging path is unchanged.
        if (options.target == LaunchTarget::endpoint)
            return run_endpoint(front_end, options.transport, size);
        // Otherwise the library, or the one profile --profile / --ssh-host
        // names; a pick runs its progress window and desktop, then the library
        // reopens until it is closed.
        return run_profile_flow(front_end, options.launch, options.target, size);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        SDL_Quit();
        return exit_status::usage;
    }
}
