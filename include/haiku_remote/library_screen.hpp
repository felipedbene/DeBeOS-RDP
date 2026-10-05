#pragma once

// The connection-library screen: the view the client shows when it opens with
// no connection named on the command line. It is a self-contained, immediate-
// mode UI drawn with the very toolkit the rest of the client already uses --
// the software-rendered Surface plus TextEngine -- and nothing else. It takes
// abstract pointer/keyboard events (a frontend translates SDL or X11 events
// into them), paints into a Surface the frontend blits, and drives a
// ConnectionLibrary for every data change.
//
// It depends on no windowing library, so it builds in haiku_remote_core and is
// shared verbatim by the SDL and X11 frontends; the dependency arrow points one
// way (frontend -> screen -> library), never back. The protocol, the session,
// the transport and the renderer know nothing about it, which is the separation
// DeBeOS-RDP issue #1 asks for: application/library UI on one side, reusable
// protocol core on the other.
//
// What it does NOT do: open a socket, start a session, or run a tunnel.
// "Connect" only records which profile the user chose; the frontend reads that
// with take_connect_request(), stamps it via ConnectionLibrary::mark_connected,
// turns it into TransportOptions with plan_launch() (profile_launch.hpp), and
// runs the existing session code path. That keeps the one network entry point
// in the frontend, where it already lives.

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/surface.hpp"
#include "haiku_remote/text_engine.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

class ConnectionLibrary;

class LibraryScreen {
public:
    enum class View { list, editor };

    // The frontend maps its native non-text keys onto these; printable text is
    // delivered separately through text_input().
    enum class Key {
        none,
        backspace,
        del,
        enter,
        tab,
        back_tab,
        up,
        down,
        escape,
    };

    // A rectangular hit target produced by the last render(). pointer_press
    // scans these; tests use them to click a control by role without hard-
    // coding pixel coordinates.
    struct Region {
        enum class Kind {
            none,
            new_connection,
            search,
            connect,
            edit,
            duplicate,
            remove,
            favorite_card,     // the star on a list card
            editor_field,      // a text field, identified by `field`
            mode_cycle,
            favorite_toggle,   // the editor's favorite checkbox
            autoreconnect_toggle,
            save,
            cancel,
        };
        Kind kind = Kind::none;
        std::string profile_id; // set for per-card actions
        int field = -1;         // set for editor_field (an EditorField index)
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;

        [[nodiscard]] bool hit(int px, int py) const
        {
            return px >= x && px < x + w && py >= y && py < y + h;
        }
    };

    // The editor's text fields, in tab order. Kept in the header so a frontend
    // or test can refer to them by name.
    enum EditorField {
        field_name,
        field_host,
        field_ssh_user,
        field_remote_port,
        field_ssh_port,
        field_identity_file,
        field_known_hosts_file,
        field_cookie_source,
        field_width,
        field_height,
        editor_field_count,
    };

    LibraryScreen(ConnectionLibrary& library, int width, int height);

    void resize(int width, int height);
    [[nodiscard]] int width() const { return surface_.width(); }
    [[nodiscard]] int height() const { return surface_.height(); }

    // Input. Each mutates state and marks the screen dirty when something
    // visible changed.
    void pointer_move(int x, int y);
    void pointer_press(int x, int y);
    void text_input(std::string_view utf8);
    void key(Key k);

    // Paint current state into the surface and return it. Also rebuilds the hit
    // regions. Clears the dirty flag.
    const Surface& render();
    [[nodiscard]] const Surface& surface() const { return surface_; }
    [[nodiscard]] bool dirty() const { return dirty_; }
    void mark_dirty() { dirty_ = true; }

    // The frontend polls this after feeding events: a value means the user hit
    // Connect and the frontend should start a session for that profile. Reading
    // it clears it.
    [[nodiscard]] std::optional<ConnectionProfile> take_connect_request();

    [[nodiscard]] View view() const { return view_; }
    [[nodiscard]] std::string_view status() const { return status_; }
    [[nodiscard]] const std::string& search_text() const { return search_; }
    [[nodiscard]] const std::vector<Region>& regions() const { return regions_; }

private:
    struct EditorState {
        std::string id; // empty => a new profile
        bool is_new = true;
        std::array<std::string, editor_field_count> text;
        ConnectionMode mode = ConnectionMode::ssh;
        bool favorite = false;
        bool auto_reconnect = true;
        int focus = field_name;
        std::string error;       // human-readable, shown under the form
        std::string error_field; // validate() field name of the bad field
    };

    // --- list view ---
    void list_pointer_press(const Region& region);
    void begin_new();
    void begin_edit(const ConnectionProfile& profile);
    void connect_profile(const std::string& id);
    void ensure_selection_visible(std::size_t visible_count);

    // --- editor view ---
    void editor_pointer_press(const Region& region);
    void editor_text(std::string_view utf8);
    void editor_key(Key k);
    [[nodiscard]] bool field_is_numeric(int field) const;
    [[nodiscard]] ConnectionProfile build_editor_profile() const;
    void commit_editor();

    // --- rendering ---
    void render_list();
    void render_editor();
    void add_region(Region region);
    // Draw a label; returns the advance width painted.
    float draw_text(std::string_view text, int x, int baseline, Color color,
                    float size);
    void draw_button(const Region& region, std::string_view label, bool primary);
    void draw_field(int field, int x, int y, int w, int h, std::string_view label,
                    std::string_view value, bool focused, bool bad);

    ConnectionLibrary& library_;
    TextEngine text_;
    Surface surface_;

    View view_ = View::list;
    std::string status_;
    std::string search_;
    std::size_t selected_ = 0;    // index into the visible (filtered) list
    std::size_t scroll_ = 0;      // first visible card index
    EditorState editor_;

    std::vector<Region> regions_;
    std::optional<ConnectionProfile> connect_request_;
    int pointer_x_ = -1;
    int pointer_y_ = -1;
    bool dirty_ = true;
};

} // namespace haiku_remote
