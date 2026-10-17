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
// in the frontend, where it already lives. A quick-connect request is an
// ordinary profile with an EMPTY id: it is in no library, so mark_connected and
// set_last_error on it are no-ops and nothing is saved.
//
// Coordinates. The surface is in device pixels, and so are pointer events and
// hit regions. Layout is computed in logical units and multiplied by scale()
// (set_scale), which a frontend sets from its renderer-output-size to
// window-size ratio, so a Retina/high-DPI window gets a sharp, correctly sized
// UI rather than a blurry upscale.
//
// Keyboard. Every button, toggle and field is reachable with Tab/Shift-Tab, is
// activated with Enter (or Space on a button or toggle), and shows a visible
// focus ring once the keyboard has been used.
//
// Text fields are real single-line editors: a caret that moves by code point
// (Left/Right), by word (with mod_word) and to either end (Home/End); a
// selection made with Shift plus any of those, with the mouse (press and drag,
// Shift-click, double-click for a word, triple-click for all) or Select All;
// Copy/Cut/Paste through a clipboard the frontend supplies (set_clipboard);
// and IME composition (text_editing) shown inline, underlined, until the
// input method commits it through text_input(). The caret blinks: the screen
// never runs a timer itself, it tells the frontend how long it may sleep
// (next_wake_ms) and the frontend calls tick() when that time is up, so an
// idle window costs one wake-up per blink phase and nothing in between.
//
// Scrolling. The editor's form scrolls between a fixed header (title, status)
// and a fixed action bar (Save / Cancel), by wheel(), Page Up/Down, and by
// itself whenever keyboard focus moves to a control outside the visible band,
// so it works at any height down to min_window_height. The list scrolls its
// cards with the wheel too.

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/surface.hpp"
#include "haiku_remote/text_engine.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

class ConnectionLibrary;

class LibraryScreen {
public:
    // list: saved connections plus quick-connect. editor: the profile editor.
    // transfer: the import/export path prompt.
    enum class View { list, editor, transfer };

    // The frontend maps its native non-text keys onto these; printable text is
    // delivered separately through text_input() (a lone " " on a focused button
    // or toggle activates it, so no Space key is needed here).
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
        // Text editing and scrolling (appended so earlier values do not move).
        left,
        right,
        home,
        end,
        select_all,
        copy,
        cut,
        paste,
        page_up,
        page_down,
    };

    // Modifiers for key() and pointer_press(). mod_shift extends the
    // selection; mod_word makes Left/Right/Backspace/Delete act on a word
    // (Ctrl on Windows/Linux, Option on macOS -- the frontend decides).
    enum Mod : unsigned {
        mod_none = 0,
        mod_shift = 1u << 0,
        mod_word = 1u << 1,
    };

    // A rectangular hit target produced by the last render(), in pixels.
    // pointer_press scans these; tests use them to click a control by role
    // without hard-coding pixel coordinates.
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
            // Added for issue #1 sections 1-2 (kept after the originals so
            // existing values do not move).
            card,              // a card's body: click selects it
            confirm_delete,    // "Delete" on a card awaiting confirmation
            cancel_delete,     // "Keep" on a card awaiting confirmation
            quick_host,        // the quick-connect host field
            quick_mode,        // the quick-connect mode chip
            quick_connect,     // quick-connect "Connect"
            quick_options,     // quick-connect "More options..." -> editor
            import_profiles,
            export_profiles,
            save_and_connect,  // editor: save, then connect
            connect_now,       // editor (quick mode): connect without saving
            transfer_path,     // import/export: the path field
            transfer_confirm,
            transfer_cancel,
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

    // What has keyboard focus: a control identified by role (and card / field),
    // never by pixel position, so it survives re-layout and re-ordering.
    struct Focus {
        Region::Kind kind = Region::Kind::none;
        std::string profile_id;
        int field = -1;

        friend bool operator==(const Focus&, const Focus&) = default;
    };

    // The editor's text fields. Values index EditorState::text and are stable
    // (field_local_port was appended); the on-screen and Tab order is
    // editor_tab_order below.
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
        field_local_port, // empty = pick a free port automatically
        editor_field_count,
    };

    static constexpr std::array<int, editor_field_count> editor_tab_order = {
        field_name,          field_host,          field_ssh_user,
        field_ssh_port,      field_remote_port,   field_local_port,
        field_identity_file, field_known_hosts_file, field_cookie_source,
        field_width,         field_height,
    };

    // The smallest surface the layout is designed for, in logical units.
    static constexpr int min_logical_width = 480;
    static constexpr int min_logical_height = 320;
    // The smallest window a frontend should allow, in window (logical) units:
    // the list keeps its header, quick connect, search and two whole cards,
    // and the editor keeps its header and action bar with a scrolling form
    // between them. Width is what the list header and quick-connect row need.
    static constexpr int min_window_width = 800;
    static constexpr int min_window_height = 480;

    // One full caret blink is two phases of this length (on, then off).
    static constexpr int caret_blink_ms = 530;
    // A wheel notch scrolls the editor by this many logical units.
    static constexpr int wheel_step = 48;

    LibraryScreen(ConnectionLibrary& library, int width, int height);

    // Size in device pixels. Surfaces smaller than the minimum are clamped.
    void resize(int width, int height);
    [[nodiscard]] int width() const { return surface_.width(); }
    [[nodiscard]] int height() const { return surface_.height(); }

    // Device pixels per logical unit (1 on a normal display, 2 on Retina).
    // Clamped to [1, 4]. Marks the screen dirty when it changes.
    void set_scale(float scale);
    [[nodiscard]] float scale() const { return scale_; }

    // The clock the cards' "Last connected ..." text is measured against.
    // Defaults to ConnectionLibrary::now_seconds; tests pin it.
    void set_clock(std::function<std::int64_t()> clock);

    // Input. Each mutates state and marks the screen dirty when something
    // visible changed. Pointer coordinates are device pixels.
    void pointer_move(int x, int y);
    // `clicks` is the click count (2 selects a word in a text field, 3 all of
    // it); `mods` & mod_shift extends the selection to the pointer.
    void pointer_press(int x, int y, int clicks = 1, unsigned mods = mod_none);
    // Ends a selection drag started by pointer_press in a text field.
    void pointer_release(int x, int y);
    // Committed text (SDL_TEXTINPUT): inserted at the caret, replacing any
    // selection. Control characters are dropped; any other UTF-8 is kept.
    void text_input(std::string_view utf8);
    // An IME's uncommitted composition (SDL_TEXTEDITING): shown at the caret,
    // never stored. `cursor` is the input method's cursor within it, in code
    // points. An empty string ends the composition.
    void text_editing(std::string_view composition, int cursor = -1);
    void key(Key k, unsigned mods = mod_none);
    // Mouse wheel / trackpad, in notches; positive scrolls up (SDL's sign).
    void wheel(float notches);

    // Where copy/cut write and paste reads. Unset, the screen keeps its own
    // in-process clipboard, so the editing commands still work everywhere.
    void set_clipboard(std::function<std::string()> read,
                       std::function<void(const std::string&)> write);

    // Caret blink. The clock is milliseconds on any monotonic base; it
    // defaults to std::chrono::steady_clock, and tests pin it.
    void set_millis_clock(std::function<std::int64_t()> clock);
    // How long the frontend may sleep before the caret must be redrawn, or -1
    // when nothing on screen animates (no text field focused, or the window
    // inactive). Always >= 1 otherwise.
    [[nodiscard]] int next_wake_ms() const;
    // Call when next_wake_ms() has elapsed: marks the screen dirty if the
    // caret's blink phase changed since the last render.
    void tick();
    // An inactive window shows no caret (and so needs no wake-ups) and a
    // muted selection.
    void set_window_active(bool active);

    // Paint current state into the surface and return it. Also rebuilds the hit
    // regions. Clears the dirty flag.
    const Surface& render();
    [[nodiscard]] const Surface& surface() const { return surface_; }
    [[nodiscard]] bool dirty() const { return dirty_; }
    void mark_dirty() { dirty_ = true; }

    // The frontend polls this after feeding events: a value means the user hit
    // Connect and the frontend should start a session for that profile. Reading
    // it clears it. A quick-connect request has an empty id.
    [[nodiscard]] std::optional<ConnectionProfile> take_connect_request();

    [[nodiscard]] View view() const { return view_; }
    [[nodiscard]] std::string_view status() const { return status_; }
    [[nodiscard]] bool status_is_error() const { return status_error_; }
    [[nodiscard]] const std::string& search_text() const { return search_; }
    [[nodiscard]] const std::string& quick_host_text() const { return quick_host_; }
    [[nodiscard]] const std::vector<Region>& regions() const { return regions_; }
    [[nodiscard]] const Focus& focus() const { return focus_; }
    // True once the keyboard moved focus; a pointer press clears it. The focus
    // ring is drawn only while this holds (the ":focus-visible" convention).
    [[nodiscard]] bool focus_visible() const { return focus_visible_; }
    // The id of the card currently asking "Delete this connection?", if any.
    [[nodiscard]] const std::string& pending_delete() const { return pending_delete_; }
    // Every validation error the editor is showing, in field (tab) order.
    [[nodiscard]] const std::vector<FieldError>& editor_errors() const
    {
        return editor_.errors;
    }
    [[nodiscard]] const std::string& editor_text(int field) const
    {
        return editor_.text[static_cast<std::size_t>(field)];
    }
    [[nodiscard]] bool editor_is_quick() const { return editor_.quick; }

    // The most a text field holds, in bytes of UTF-8 -- each field's natural
    // limit, so a stray paste can never make one hold a megabyte:
    //   host 253 (a DNS name's maximum, RFC 1035 2.3.4 / RFC 1123 2.1);
    //   ports 5 and width/height 5 (65535; Surface::max_dimension 16384);
    //   SSH user and name 255 (POSIX LOGIN_NAME_MAX on Linux is 256 with the
    //   NUL; a name is one line on a card); quick connect's "user@host" 509
    //   (255 + "@" + 253); search 255 (it matches name, host and user, so a
    //   longer query can match nothing); identity, known-hosts and cookie
    //   source, and the import/export path 4095 (PATH_MAX 4096 with the NUL).
    // 0 for something that is not a text field.
    [[nodiscard]] static std::size_t max_field_bytes(Region::Kind kind, int field = -1);
    // Typing or a paste that would take a field past max_field_bytes inserts
    // nothing and says so here, against the field, until that field is next
    // edited or another field gets focus: a visible refusal, never a silent
    // truncation. Empty when there is none.
    [[nodiscard]] const std::string& input_error() const { return input_error_.message; }
    [[nodiscard]] const Focus& input_error_field() const { return input_error_.field; }
    // The text engine's running operation count (TextEngine::Stats), so a test
    // can pin what a render or an edit costs as an exact number of code points
    // laid out rather than as wall-clock time.
    [[nodiscard]] const TextEngine::Stats& text_stats() const { return text_.stats(); }

    // The focused text field's caret and selection anchor, as byte offsets
    // into its text (always on a code-point boundary). Equal when nothing is
    // selected. Both 0 when no text field has focus.
    [[nodiscard]] std::size_t caret() const { return edit_.caret; }
    [[nodiscard]] std::size_t selection_anchor() const { return edit_.anchor; }
    [[nodiscard]] std::string selected_text() const;
    [[nodiscard]] const std::string& composition() const { return edit_.composition; }
    // The caret's rectangle from the last render, in device pixels (for
    // SDL_SetTextInputRect, so an IME's candidate window sits by the caret).
    // w == 0 when no caret was drawn.
    [[nodiscard]] const Region& caret_rect() const { return caret_rect_; }
    // How far the editor form is scrolled, in logical units, and the most it
    // can scroll at the current size (both as of the last render).
    [[nodiscard]] int editor_scroll() const { return editor_scroll_; }
    [[nodiscard]] int editor_max_scroll() const { return editor_max_scroll_; }

    // The card's "Last connected ..." text for a Unix time `when`, measured
    // from `now`: "Never connected" for 0, relative ("just now", "12 min ago",
    // "3 h ago", "2 days ago") within a week, else the local calendar date
    // ("Last connected on 2026-03-01").
    [[nodiscard]] static std::string describe_last_connected(std::int64_t when,
                                                             std::int64_t now);

    // A card's third line: "<mode>  ·  <width> × <height>  ·  <last connected>",
    // e.g. "SSH tunnel  ·  1280 × 800  ·  Last connected 5 min ago".
    [[nodiscard]] static std::string card_meta(const ConnectionProfile& profile,
                                               std::int64_t now);

    // The validate() field name an editor field reports under ("remotePort"...).
    [[nodiscard]] static std::string_view validate_name(int field);

private:
    struct EditorState {
        std::string id; // empty => a new (or quick) profile
        bool is_new = true;
        bool quick = false; // quick connect: Connect without saving
        std::array<std::string, editor_field_count> text;
        ConnectionMode mode = ConnectionMode::ssh;
        bool favorite = false;
        bool auto_reconnect = true;
        // Once the user has tried to save or connect, errors are recomputed on
        // every edit so each one disappears the moment it is fixed.
        bool show_errors = false;
        std::vector<FieldError> errors;
    };

    struct TransferState {
        bool is_export = true;
        std::string path;
        std::string error;
    };

    enum class Commit { save, save_and_connect, connect_only };

    struct TextEdit {
        std::size_t caret = 0;
        std::size_t anchor = 0;
        std::string composition;
        int composition_cursor = -1; // code points into composition; -1 = end
        int hscroll = 0;             // device px the focused field is scrolled
        bool dragging = false;
    };

    // The editor's geometry in logical units, before scrolling: fields and
    // toggles in form coordinates, the fixed bands in window coordinates.
    struct Box {
        int x = 0, y = 0, w = 0, h = 0;
    };
    struct EditorLayout {
        int left = 0, right = 0, content_w = 0;
        std::array<Box, editor_field_count> fields {};
        Box mode, favorite, reconnect;
        int route_y = 0;
        int view_top = 0;      // the scrolling band, window coordinates
        int view_bottom = 0;
        int content_bottom = 0; // form coordinates (unscrolled)
        int actions_y = 0;
    };

    // --- text editing ---
    void reset_edit();
    void clamp_edit();
    [[nodiscard]] bool has_selection() const { return edit_.caret != edit_.anchor; }
    void touch_caret(); // restart the blink so the caret is solid after input
    void text_changed();
    void insert_text(std::string_view utf8);
    void delete_selection(std::string& text);
    [[nodiscard]] bool edit_key(Key k, unsigned mods);
    [[nodiscard]] int text_pad(Region::Kind kind) const;
    [[nodiscard]] std::size_t offset_at(std::string_view text, int rel_px);
    // The caret stops of the field text `text` at the field font, laid out once
    // and reused for as long as the text is unchanged: every render, caret
    // move, click and drag in between costs no layout at all.
    [[nodiscard]] const TextEngine::Stops& field_stops(std::string_view text);
    // `text` cut to `max_width` device px at `font` with a trailing ellipsis,
    // or unchanged when it fits; costs a layout of what is shown, not of all
    // of `text`.
    [[nodiscard]] std::string elide(std::string_view text, const Font& font,
                                    float max_width);
    void set_input_error(std::string message);
    void clear_input_error();
    [[nodiscard]] bool caret_on() const;
    void draw_text_value(const Region& region, std::string_view value,
                         std::string_view placeholder, bool focused);

    // --- editor scrolling ---
    [[nodiscard]] EditorLayout editor_layout() const;
    void set_editor_scroll(int scroll, const EditorLayout& layout);
    void reveal_editor_focus(const EditorLayout& layout);

    // --- focus ---
    [[nodiscard]] std::vector<Focus> focus_order() const;
    void move_focus(int delta, bool wrap);
    void set_focus(Focus focus);
    void activate(const Focus& focus);
    [[nodiscard]] bool focus_is_text() const;
    [[nodiscard]] std::string* focused_text();
    [[nodiscard]] bool is_focused(const Region& region) const;
    void sync_selection_to_focus();

    // --- list view ---
    void list_pointer_press(const Region& region);
    void list_key(Key k);
    void begin_new();
    void begin_quick();
    void begin_edit(const ConnectionProfile& profile);
    void begin_transfer(bool is_export);
    void connect_profile(const std::string& id);
    void quick_connect();
    void return_to_list(std::string status, bool is_error = false);
    void ensure_selection_visible(std::size_t visible_count);
    void scroll_list(int cards); // by whole cards; positive = later cards
    [[nodiscard]] std::size_t cards_per_page() const;

    // --- editor view ---
    void editor_pointer_press(const Region& region);
    void editor_key(Key k);
    void editor_changed();
    // Strictly parses every numeric field and validates the result. Returns the
    // profile when there are no errors; fills `errors` either way.
    [[nodiscard]] std::optional<ConnectionProfile>
    build_editor_profile(std::vector<FieldError>& errors) const;
    // The profile the route preview shows: whatever parses, defaults elsewhere.
    [[nodiscard]] ConnectionProfile preview_profile() const;
    void commit_editor(Commit how);

    // --- transfer view ---
    void transfer_pointer_press(const Region& region);
    void transfer_key(Key k);
    void commit_transfer();

    // --- rendering ---
    void render_list();
    void render_card(const ConnectionProfile& profile, std::size_t index, int x,
                     int y, int w);
    void render_editor();
    void render_transfer();
    void add_region(Region region);
    [[nodiscard]] int px(int logical) const;
    [[nodiscard]] Region make_region(Region::Kind kind, int lx, int ly, int lw,
                                     int lh, std::string profile_id = {},
                                     int field = -1) const;
    // Draw a label at logical coordinates; returns the logical advance width.
    float draw_text(std::string_view text, int lx, int lbaseline, Color color,
                    float size, bool bold = false, int lmax_width = 0);
    [[nodiscard]] float text_width(std::string_view text, float size, bool bold);
    enum class ButtonStyle { primary, secondary, danger, chip };
    void draw_button(const Region& region, std::string_view label,
                     ButtonStyle style);
    void draw_toggle(const Region& region, std::string_view label, bool on);
    void draw_field(const Region& region, std::string_view label,
                    std::string_view value, std::string_view placeholder,
                    bool focused, std::string_view error);
    void draw_focus_ring(const Region& region);
    void draw_route(const ConnectionProfile& profile, int lx, int ly, int lw,
                    bool compact = false);

    ConnectionLibrary& library_;
    TextEngine text_;
    Surface surface_;
    float scale_ = 1.0f;
    std::function<std::int64_t()> clock_;

    View view_ = View::list;
    std::string status_;
    bool status_error_ = false;
    std::string search_;
    std::string quick_host_;
    ConnectionMode quick_mode_ = ConnectionMode::ssh;
    std::size_t selected_ = 0;    // index into the visible (filtered) list
    std::size_t scroll_ = 0;      // first visible card index
    std::string pending_delete_;  // card awaiting delete confirmation
    EditorState editor_;
    TransferState transfer_;
    Focus focus_ {Region::Kind::search, {}, -1};
    Focus list_focus_; // where focus returns when the editor closes
    bool focus_visible_ = false;

    TextEdit edit_;
    struct InputError {
        Focus field;
        std::string message;
    };
    InputError input_error_;
    struct StopsCache {
        bool valid = false;
        std::string text;
        float size = 0;
        TextEngine::Stops stops;
    };
    StopsCache stops_cache_;
    std::string own_clipboard_;
    std::function<std::string()> clipboard_read_;
    std::function<void(const std::string&)> clipboard_write_;
    std::function<std::int64_t()> millis_;
    std::int64_t blink_epoch_ = 0;
    bool window_active_ = true;
    int rendered_caret_phase_ = -1; // blink phase the last render drew
    Region caret_rect_;
    int editor_scroll_ = 0;
    int editor_max_scroll_ = 0;
    bool reveal_pending_ = false;
    float wheel_rest_ = 0; // fractional notches not yet applied to the list

    std::vector<Region> regions_;
    std::optional<ConnectionProfile> connect_request_;
    int pointer_x_ = -1;
    int pointer_y_ = -1;
    int hovered_ = -1; // index into regions_ under the pointer
    bool dirty_ = true;
};

} // namespace haiku_remote
