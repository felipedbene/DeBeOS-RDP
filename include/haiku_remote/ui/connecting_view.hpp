#pragma once

// The connecting / failed screen (issue #1): an explicit progress state instead
// of a silent black canvas. While connecting it shows the profile, the current
// lifecycle stage, elapsed time, and a Cancel action. On failure it shows the
// actionable reason, the human detail, and the bounded diagnostic ring, with a
// "Copy diagnostics" action (to the SDL clipboard) and "Back to library". It
// reads LibraryController's snapshot and drives cancel()/dismiss_failure().
//
// GUI/SDL side only -- compiled into haiku-remote-gui, never haiku_remote_core.

#include "haiku_remote/library_controller.hpp"
#include "haiku_remote/ui/widgets.hpp"

namespace haiku_remote::ui {

class ConnectingView {
public:
    void render(Widgets& ui, LibraryController& controller, int width, int height);

private:
    int diag_scroll_ = 0;
};

} // namespace haiku_remote::ui
