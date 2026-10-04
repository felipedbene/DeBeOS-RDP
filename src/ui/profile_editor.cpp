#include "haiku_remote/ui/profile_editor.hpp"

#include <charconv>
#include <string>

namespace haiku_remote::ui {

namespace {

enum FieldId {
    kName = 10,
    kHost,
    kRemotePort,
    kSshUser,
    kSshPort,
    kIdentity,
    kLocalPort,
    kWidth,
    kHeight,
    kKnownHosts,
    kCookieSource,
};

// Parse a required integer field. On failure, set ok=false and name the field
// (once), and return `fallback` so the route summary can still render.
int parse_int(const TextFieldModel& model, const char* field, int fallback,
              bool& ok, std::string& error)
{
    const std::string& text = model.text();
    int value = 0;
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        if (ok) {
            ok = false;
            error = std::string(field) + ": not a number";
        }
        return fallback;
    }
    return value;
}

} // namespace

void ProfileEditor::load(const ConnectionProfile& profile)
{
    id_ = profile.id;
    name_.set_text(profile.name);
    host_.set_text(profile.host);
    remote_port_.set_text(std::to_string(profile.remote_port));
    ssh_user_.set_text(profile.ssh_user);
    ssh_port_.set_text(std::to_string(profile.ssh_port));
    identity_.set_text(profile.identity_file);
    local_port_.set_text(profile.local_port ? std::to_string(*profile.local_port)
                                             : std::string{});
    width_.set_text(std::to_string(profile.width));
    height_.set_text(std::to_string(profile.height));
    known_hosts_.set_text(profile.known_hosts_file);
    cookie_source_.set_text(profile.cookie_source);
    favorite_ = profile.favorite;
    ssh_mode_ = profile.mode == ConnectionMode::ssh;
    auto_reconnect_ = profile.auto_reconnect;
    last_connected_at_ = profile.last_connected_at;
    status_.clear();
}

ConnectionProfile ProfileEditor::build(bool& ok, std::string& error) const
{
    ok = true;
    error.clear();
    ConnectionProfile profile;
    profile.id = id_;
    profile.name = name_.text();
    profile.favorite = favorite_;
    profile.mode = ssh_mode_ ? ConnectionMode::ssh : ConnectionMode::direct;
    profile.host = host_.text();
    profile.remote_port = parse_int(remote_port_, "remotePort", 10900, ok, error);
    profile.ssh_user = ssh_user_.text();
    profile.ssh_port = parse_int(ssh_port_, "sshPort", 22, ok, error);
    profile.identity_file = identity_.text();
    if (!local_port_.empty())
        profile.local_port = parse_int(local_port_, "localPort", 0, ok, error);
    profile.width = parse_int(width_, "width", 1280, ok, error);
    profile.height = parse_int(height_, "height", 800, ok, error);
    profile.known_hosts_file = known_hosts_.text();
    profile.cookie_source = cookie_source_.text();
    profile.auto_reconnect = auto_reconnect_;
    profile.last_connected_at = last_connected_at_;
    return profile;
}

void ProfileEditor::render(Widgets& ui, LibraryController& controller, int width,
                           int height)
{
    (void)height;
    const Theme& t = ui.theme();
    const int margin = 16;
    const int label_w = 150;
    const int field_x = margin + label_w;
    const int field_w = width - field_x - margin;
    const int row_h = 28;
    const int gap = 8;

    ui.label(margin, margin + t.font_size,
             id_.empty() ? "New connection" : "Edit connection", t.text);

    int y = margin + ui.line_height() + 12;
    const auto form_row = [&](int id, const char* caption, TextFieldModel& model,
                              const char* placeholder = "") {
        ui.label(margin, ui.center_baseline(Box{margin, y, label_w, row_h}), caption,
                 t.text_dim);
        ui.field(id, Box{field_x, y, field_w, row_h}, model, placeholder);
        y += row_h + gap;
    };

    form_row(kName, "Name", name_, "friendly name");

    // Mode / favorite / auto-reconnect toggles on one line.
    ui.label(margin, ui.center_baseline(Box{margin, y, label_w, row_h}), "Mode",
             t.text_dim);
    if (ui.toggle(Box{field_x, y, 110, row_h}, ssh_mode_ ? "SSH tunnel" : "Direct TCP",
                  ssh_mode_))
        ssh_mode_ = !ssh_mode_;
    if (ui.toggle(Box{field_x + 120, y, 110, row_h}, "Favorite", favorite_))
        favorite_ = !favorite_;
    if (ui.toggle(Box{field_x + 240, y, 130, row_h}, "Auto reconnect", auto_reconnect_))
        auto_reconnect_ = !auto_reconnect_;
    y += row_h + gap;

    form_row(kHost, ssh_mode_ ? "Haiku host (SSH)" : "Host", host_, "hostname or IP");
    form_row(kRemotePort, "Remote port", remote_port_);
    if (ssh_mode_) {
        form_row(kSshUser, "SSH user", ssh_user_);
        form_row(kSshPort, "SSH port", ssh_port_);
        form_row(kIdentity, "Identity file", identity_, "optional; ssh-agent if empty");
        form_row(kLocalPort, "Local port", local_port_, "auto when empty");
        form_row(kKnownHosts, "Known hosts", known_hosts_, "optional per-profile file");
    } else {
        form_row(kCookieSource, "Cookie source", cookie_source_,
                 "path or source tag; never the cookie");
    }
    form_row(kWidth, "Width", width_);
    form_row(kHeight, "Height", height_);

    // Continuous route summary.
    bool ok = true;
    std::string error;
    const ConnectionProfile preview = build(ok, error);
    ui.label(margin, y + t.font_size, "Route:", t.text_dim);
    ui.label_clipped(margin + ui.measure("Route: "), y + t.font_size,
                     preview.route_summary(), t.text, width - 2 * margin);
    y += ui.line_height() + 6;

    if (!status_.empty())
        ui.label(margin, y + t.font_size, status_, t.danger);
    y += ui.line_height() + 10;

    // Actions.
    const Box save_btn{margin, y, 110, row_h + 4};
    const Box connect_btn{margin + 120, y, 150, row_h + 4};
    const Box cancel_btn{margin + 280, y, 110, row_h + 4};

    if (ui.button_accent(save_btn, "Save")) {
        if (!ok) {
            status_ = error;
        } else {
            const SaveResult result = controller.commit_profile(preview);
            if (!result.ok)
                status_ = result.invalid_field.field + ": "
                    + result.invalid_field.message;
            // On success commit_profile() returned to the library.
        }
        return;
    }
    if (ui.button(connect_btn, "Save & Connect")) {
        if (!ok) {
            status_ = error;
            return;
        }
        const SaveResult result = controller.commit_profile(preview);
        if (!result.ok) {
            status_ = result.invalid_field.field + ": "
                + result.invalid_field.message;
            return;
        }
        if (controller.selected())
            controller.connect(*controller.selected());
        return;
    }
    if (ui.button(cancel_btn, "Cancel") || ui.input().escape) {
        controller.close_editor();
        return;
    }
}

} // namespace haiku_remote::ui
