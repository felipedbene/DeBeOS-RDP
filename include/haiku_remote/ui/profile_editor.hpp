#pragma once

// The profile editor screen (issue #1): create or edit a saved connection. Each
// field is a focusable text field or a toggle; the resulting route is
// summarized continuously, and Save validates through the store before writing
// so an invalid port or dimension is reported against its field rather than
// silently coerced. Save and Connect act on LibraryController; Cancel returns to
// the library without writing.
//
// GUI/SDL side only -- compiled into haiku-remote-gui, never haiku_remote_core.

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/library_controller.hpp"
#include "haiku_remote/text_field_model.hpp"
#include "haiku_remote/ui/widgets.hpp"

#include <string>

namespace haiku_remote::ui {

class ProfileEditor {
public:
    // Seed the form from the controller's working copy. The shell calls this on
    // entering the editor so a fresh "New" starts blank and an "Edit" starts
    // populated.
    void load(const ConnectionProfile& profile);

    void render(Widgets& ui, LibraryController& controller, int width, int height);

private:
    // Assemble a ConnectionProfile from the current field values. `ok` is false
    // when an integer field did not parse; `error` names it.
    ConnectionProfile build(bool& ok, std::string& error) const;

    std::string id_; // empty => creating a new profile
    TextFieldModel name_;
    TextFieldModel host_;
    TextFieldModel remote_port_;
    TextFieldModel ssh_user_;
    TextFieldModel ssh_port_;
    TextFieldModel identity_;
    TextFieldModel local_port_;
    TextFieldModel width_;
    TextFieldModel height_;
    TextFieldModel known_hosts_;
    TextFieldModel cookie_source_;
    bool favorite_ = false;
    bool ssh_mode_ = true;
    bool auto_reconnect_ = true;
    std::int64_t last_connected_at_ = 0;
    std::string status_; // validation / parse error shown under the form
};

} // namespace haiku_remote::ui
