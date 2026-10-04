#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "haiku_remote/launch_options.hpp"
#include "haiku_remote/library_controller.hpp"
#include "haiku_remote/profile_store.hpp"
#include "haiku_remote/ssh_tunnel.hpp"
#include "haiku_remote/surface.hpp"
#include "haiku_remote/transport.hpp"
#include "haiku_remote/ui/connecting_view.hpp"
#include "haiku_remote/ui/desktop_view.hpp"
#include "haiku_remote/ui/library_view.hpp"
#include "haiku_remote/ui/profile_editor.hpp"
#include "haiku_remote/ui/widgets.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

using namespace haiku_remote;

namespace {

// Resolve a --profile reference (id first, then name) to an index in the loaded
// library. nullopt when nothing matches.
std::optional<std::size_t> find_profile(const LibraryController& controller,
                                        const std::string& reference)
{
    const auto& profiles = controller.profiles();
    for (std::size_t i = 0; i < profiles.size(); ++i)
        if (profiles[i].id == reference)
            return i;
    for (std::size_t i = 0; i < profiles.size(); ++i)
        if (profiles[i].name == reference)
            return i;
    return std::nullopt;
}

// Build this frame's chrome input from the current SDL state plus the events
// that arrived. Mouse-held state persists across frames in `held`.
ui::Input gather_chrome_input(bool& held, int& wheel_accum,
                              const std::string& text_accum, bool clicked,
                              const ui::Input& keys)
{
    ui::Input input = keys;
    int x = 0;
    int y = 0;
    SDL_GetMouseState(&x, &y);
    input.mouse_x = x;
    input.mouse_y = y;
    input.mouse_down = held;
    input.clicked = clicked;
    input.wheel = wheel_accum;
    input.text = text_accum;
    return input;
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
    // at launch (the tunnel also reaps before each connect). An unverifiable pid
    // is never signalled -- only its stale pidfile is unlinked -- so a recycled
    // pid can never make us kill a stranger.
    reap_orphans(SshTunnel::default_pidfile_dir());

    LibraryController controller;
    controller.reload();

    // Launch-time connection flags bypass the library and connect immediately,
    // preserving the automation/debug workflow (--host/--ssh-host/--url/--profile).
    if (!launch.options.open_library) {
        if (!launch.options.profile_ref.empty()) {
            const auto index = find_profile(controller, launch.options.profile_ref);
            if (!index) {
                std::cerr << "no saved profile matches --profile "
                          << launch.options.profile_ref << '\n';
                return exit_status::usage;
            }
            controller.connect(*index, launch.options.transport.cookie);
        } else {
            controller.connect_config(coordinator_config_from(launch.options));
        }
    }

    try {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
            throw std::runtime_error(SDL_GetError());

        SDL_Window* window = SDL_CreateWindow(
            "DeBeOS Remote", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            launch.options.width, launch.options.height,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (window == nullptr)
            throw std::runtime_error(SDL_GetError());
        SDL_Renderer* renderer = SDL_CreateRenderer(
            window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (renderer == nullptr)
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (renderer == nullptr)
            throw std::runtime_error(SDL_GetError());

        ui::Widgets widgets;
        ui::LibraryView library_view;
        ui::ProfileEditor profile_editor;
        ui::ConnectingView connecting_view;
        ui::DesktopView desktop_view;

        // The chrome texture/surface, (re)created to match the window size.
        SDL_Texture* chrome_texture = nullptr;
        std::unique_ptr<Surface> chrome_surface;
        int chrome_w = 0;
        int chrome_h = 0;
        const auto ensure_chrome = [&](int w, int h) {
            if (chrome_texture != nullptr && w == chrome_w && h == chrome_h)
                return;
            if (chrome_texture != nullptr)
                SDL_DestroyTexture(chrome_texture);
            chrome_w = w;
            chrome_h = h;
            chrome_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                               SDL_TEXTUREACCESS_STREAMING, w, h);
            chrome_surface = std::make_unique<Surface>(w, h);
        };

        AppMode previous = AppMode::Connecting; // force a first on-enter below
        bool running = true;
        bool mouse_held = false; // left button state carried across frames
        SDL_StartTextInput();

        while (running) {
            controller.poll();
            const AppMode mode = controller.mode();

            // View transitions.
            if (mode != previous) {
                if (previous == AppMode::Connected)
                    desktop_view.close();
                switch (mode) {
                case AppMode::Library:
                    library_view.on_enter();
                    break;
                case AppMode::Editor:
                    profile_editor.load(controller.editing());
                    break;
                case AppMode::Connected: {
                    std::string error;
                    if (controller.coordinator() == nullptr
                        || !desktop_view.open(renderer, *controller.coordinator(),
                                              error)) {
                        std::cerr << "could not open desktop view: " << error << '\n';
                        controller.cancel();
                    }
                    break;
                }
                default:
                    break;
                }
                previous = mode;
            }

            // ---- Event pump. ----
            bool clicked = false;
            int wheel_accum = 0;
            std::string text_accum;
            ui::Input keys; // the per-frame key edits for chrome

            SDL_Event event {};
            while (SDL_PollEvent(&event) != 0) {
                if (event.type == SDL_QUIT) {
                    if (mode == AppMode::Connected || mode == AppMode::Connecting)
                        controller.cancel(); // leave the session, not the app
                    else
                        running = false;
                    continue;
                }
                if (mode == AppMode::Connected) {
                    if (controller.coordinator() != nullptr)
                        desktop_view.handle_event(event, *controller.coordinator());
                    continue;
                }
                // Chrome event translation.
                switch (event.type) {
                case SDL_MOUSEBUTTONDOWN:
                    if (event.button.button == SDL_BUTTON_LEFT)
                        mouse_held = true;
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        mouse_held = false;
                        clicked = true;
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    wheel_accum += event.wheel.y;
                    break;
                case SDL_TEXTINPUT:
                    text_accum += event.text.text;
                    break;
                case SDL_KEYDOWN:
                    switch (event.key.keysym.sym) {
                    case SDLK_BACKSPACE: keys.backspace = true; break;
                    case SDLK_DELETE: keys.del = true; break;
                    case SDLK_LEFT: keys.left = true; break;
                    case SDLK_RIGHT: keys.right = true; break;
                    case SDLK_HOME: keys.home = true; break;
                    case SDLK_END: keys.end = true; break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER: keys.enter = true; break;
                    case SDLK_ESCAPE: keys.escape = true; break;
                    case SDLK_TAB:
                        if ((SDL_GetModState() & KMOD_SHIFT) != 0)
                            keys.back_tab = true;
                        else
                            keys.tab = true;
                        break;
                    default:
                        break;
                    }
                    break;
                default:
                    break;
                }
            }

            if (mode == AppMode::Connected) {
                if (controller.coordinator() != nullptr)
                    desktop_view.render(renderer, *controller.coordinator());
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }

            // ---- Chrome frame. ----
            int win_w = 0;
            int win_h = 0;
            SDL_GetWindowSize(window, &win_w, &win_h);
            if (win_w <= 0 || win_h <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }
            ensure_chrome(win_w, win_h);

            const ui::Input input =
                gather_chrome_input(mouse_held, wheel_accum, text_accum, clicked, keys);
            widgets.begin(*chrome_surface, input);
            switch (mode) {
            case AppMode::Library:
                library_view.render(widgets, controller, win_w, win_h);
                break;
            case AppMode::Editor:
                profile_editor.render(widgets, controller, win_w, win_h);
                break;
            case AppMode::Connecting:
            case AppMode::Failed:
                connecting_view.render(widgets, controller, win_w, win_h);
                break;
            default:
                break;
            }
            widgets.end();

            SDL_RenderSetLogicalSize(renderer, 0, 0); // chrome is 1:1 with the window
            SDL_UpdateTexture(chrome_texture, nullptr,
                              chrome_surface->pixels().data(),
                              chrome_surface->stride());
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, chrome_texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        SDL_StopTextInput();
        // Tear the connection down before exit: the coordinator's destructor
        // joins its worker, which stops the owned ssh child. The launch-time
        // reaper is the backstop for anything a crash left behind.
        controller.cancel();
        desktop_view.close();
        if (chrome_texture != nullptr)
            SDL_DestroyTexture(chrome_texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return exit_status::ok;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        SDL_Quit();
        return exit_status::usage;
    }
}
