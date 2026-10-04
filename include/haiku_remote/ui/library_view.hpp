#pragma once

// The connection library screen (issue #1): the home screen the app opens on,
// instead of a black desktop. Lists saved profiles favorite-first, each a card
// with its route and last-connected/last-error detail, and per-card actions
// (Connect, Edit, Duplicate, Favorite, Delete-with-confirm). A "New connection"
// button opens the editor and a compact quick-connect field connects an
// ephemeral, never-saved host. It renders LibraryController and translates
// clicks into its methods; it holds no connection state of its own.
//
// GUI/SDL side only -- compiled into haiku-remote-gui, never haiku_remote_core.

#include "haiku_remote/library_controller.hpp"
#include "haiku_remote/text_field_model.hpp"
#include "haiku_remote/ui/widgets.hpp"

#include <cstddef>
#include <optional>

namespace haiku_remote::ui {

class LibraryView {
public:
    // Called by the shell when this screen becomes current, to reset transient
    // state (the pending delete confirmation, the quick-connect buffer).
    void on_enter();

    // Draw and handle one frame. Mutating actions go straight to `controller`.
    void render(Widgets& ui, LibraryController& controller, int width, int height);

private:
    TextFieldModel quick_host_;
    // Index (into the current ordering) whose Delete is awaiting confirmation.
    std::optional<std::size_t> confirm_delete_;
    int scroll_ = 0;
};

} // namespace haiku_remote::ui
