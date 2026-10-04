#pragma once

// The connected desktop view: the remote framebuffer and its input loop, lifted
// from the old sdl_main.cpp. The SDL keymap, modifier mask, button mask and
// UTF-8 helpers move with it (into desktop_view.cpp). It owns its own streaming
// texture sized to the remote resolution; the shell hands it SDL events while
// the session is up and asks it to present each frame. The UI thread never
// touches the Session or its Surface -- it copies the frame the coordinator
// worker hands out, exactly as before.
//
// GUI/SDL side only -- compiled into haiku-remote-gui, never haiku_remote_core.

#include <SDL.h>

#include "haiku_remote/connection_coordinator.hpp"
#include "haiku_remote/surface.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace haiku_remote::ui {

class DesktopView {
public:
    DesktopView() = default;
    ~DesktopView();
    DesktopView(const DesktopView&) = delete;
    DesktopView& operator=(const DesktopView&) = delete;

    // (Re)create the texture at the coordinator's resolution. Called by the
    // shell on entering the connected view. Returns false and sets `error` when
    // the texture could not be created.
    bool open(SDL_Renderer* renderer, ConnectionCoordinator& coordinator,
              std::string& error);
    void close();
    [[nodiscard]] bool is_open() const { return texture_ != nullptr; }

    // Translate one SDL event into coordinator input (mouse, keyboard, text).
    void handle_event(const SDL_Event& event, ConnectionCoordinator& coordinator);

    // Copy the newest frame (only on a generation change) and present it, with
    // the renderer's logical size pinned to the remote resolution.
    void render(SDL_Renderer* renderer, ConnectionCoordinator& coordinator);

    // Drop modifier state (on focus loss / on open) so a stuck modifier cannot
    // carry across a focus change.
    void reset_modifiers(ConnectionCoordinator& coordinator);

private:
    SDL_Texture* texture_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    std::unique_ptr<Surface> frame_;
    std::uint64_t shown_generation_ = 0;
    std::uint32_t last_modifiers_ = 0;
};

} // namespace haiku_remote::ui
