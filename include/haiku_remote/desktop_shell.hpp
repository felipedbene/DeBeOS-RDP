#pragma once

// DeBeOS-RDP issue #1, "Desktop shell": the in-session toolbar of a connected
// desktop, and the small pieces of glue between it and the session.
//
// DesktopShell is an immediate-mode overlay drawn with the same toolkit as
// LibraryScreen and ConnectScreen (a software Surface plus TextEngine). It is
// deliberately *not* drawn into the session's Surface: that surface is the only
// copy of the remote screen and is what RP_READ_BITMAP answers are computed
// from, so the bar renders into its own Surface and the front end composites
// it on top at present time. Hidden, it costs the desktop no pixels at all.
//
// It reveals when the pointer touches the top edge over the bar's span, or on a
// hotkey the front end maps to toggle(), and hides itself again a moment after
// the pointer leaves. While the link is not up (connecting, waiting to
// reconnect, lost) it stays visible, because then the status *is* the content.
//
// It opens no socket and knows nothing about SDL; the front end feeds it
// pointer events in desktop coordinates and acts on take_action().

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/reconnect.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/surface.hpp"
#include "haiku_remote/text_engine.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

// A key chord the local host is likely to swallow before it reaches the client
// window, sent to the remote as RP_MODIFIERS_CHANGED + RP_KEY_DOWN + RP_KEY_UP.
//
// Only chords app_server itself acts on are offered. Ctrl+Alt+Del is not one of
// them: Haiku's Team Monitor is raised by the *local* keyboard driver from raw
// key state (add-ons/input_server/devices/keyboard/KeyboardInputDevice.cpp:324-337),
// and remote input enters app_server's EventDispatcher directly
// (drawing/interface/remote/RemoteEventStream.cpp:101-219), never passing
// through that driver -- so sending it would arrive as an ordinary keystroke.
struct RemoteShortcut {
    std::string_view label;
    std::uint32_t modifiers; // the Haiku modifier mask held for the chord
    std::int32_t key;        // Haiku key code
    std::string_view bytes;  // the key's composed bytes
    std::int32_t raw_char;
};

// The offered chords. Workspace 1-4 is Command+F1..F4 and Previous workspace is
// Command+` (Desktop.cpp:200-224, KeyboardFilter::Filter); both are the chords a
// host window manager typically takes for itself (Alt+F1..F4 on Linux and
// Windows, where the client maps Alt to Command).
[[nodiscard]] std::span<const RemoteShortcut> remote_shortcuts();

// The four messages that send `shortcut`, ending by restoring `held_modifiers`
// (the modifiers really held locally) so the remote's idea of the keyboard is
// not left with a phantom Command key down.
[[nodiscard]] std::vector<std::vector<std::uint8_t>>
encode_shortcut(const RemoteShortcut& shortcut, std::uint32_t held_modifiers);

// Which repaint lever "Request full repaint" pulled.
enum class RepaintLever {
    resync,       // RP_RESYNC: barrier, full state replay and repaint (§4.4).
    display_mode, // RP_UPDATE_DISPLAY_MODE at the current size (§9.2).
    failed,       // the send failed.
};

// Ask the server to repaint everything, with the best lever this connection
// negotiated: RP_RESYNC when RP_CAP_RESYNC was acknowledged, otherwise the
// same-size RP_UPDATE_DISPLAY_MODE every server honours (PROTOCOL.md §9.2).
RepaintLever request_repaint(Session& session);

// The reconnect configuration for a profile: the GUI's reboot-sized backoff
// (below), switched on by the profile's `auto_reconnect` flag.
[[nodiscard]] ReconnectConfig reconnect_config_for(const ConnectionProfile& profile);

// The GUI's reconnect budget, sized to outlast a guest reboot. Measured on the
// canonical AMI (ami-0de9d45ed8c03cad1, c7g.large, us-west-2, 2026-10-17), from
// a guest-initiated `shutdown -r` to app_server publishing its new session
// cookie and listening on 10900: 6, 7, 7 and 6 s over four reboots, 8-9 s until
// the SSM agent -- the route an SSM port forward needs -- is back; a cold boot
// from launch took 12 s to the listener and 15 s to SSM. The command-line
// default (5 attempts, 200 ms doubling to 5 s: 6.2 s of waiting) gives up
// before even the warm case is reliably back.
//
// 500 ms doubling to a 4 s cap, ten attempts: 0.5 + 1 + 2 + 4 x 7 = 31.5 s of
// waiting, twice the slowest boot measured, before the time the attempts
// themselves take. The cap keeps the gap between the listener coming back
// and the client noticing to at most 4 s.
inline constexpr int gui_reconnect_attempts = 10;
inline constexpr std::chrono::milliseconds gui_reconnect_base {500};
inline constexpr std::chrono::milliseconds gui_reconnect_cap {4000};

// The total time `config` waits across every attempt it allows: the sum of
// backoff_for(0 .. max_attempts - 1). The time the attempts themselves take
// comes on top.
[[nodiscard]] std::chrono::milliseconds total_backoff(const ReconnectConfig& config);

// The toolbar's countdown while a reconnect is pending: "attempt 3 of 9 in 4 s",
// the seconds rounded up so it never reads 0 while there is still a wait, and
// "attempt 3 of 9 now" once it is due. Empty when nothing is pending.
[[nodiscard]] std::string reconnect_countdown(const ReconnectSchedule& schedule,
                                              std::chrono::steady_clock::time_point now);

// A display size the toolbar's resolution row offers.
struct DisplaySize {
    int width = 0;
    int height = 0;
    friend bool operator==(const DisplaySize&, const DisplaySize&) = default;
};

// The common sizes offered, in the order shown.
[[nodiscard]] std::span<const DisplaySize> display_size_presets();

// What the resolution row asked for.
struct DisplaySizeChoice {
    enum class Kind {
        preset, // `size`
        window, // the window's current drawable size
        fit,    // fit-to-window was switched (DesktopShell::fit_to_window())
    };
    Kind kind = Kind::preset;
    DisplaySize size;
};

// Fit-to-window, settled: a window being dragged reports a new size on every
// step, and every RP_UPDATE_DISPLAY_MODE makes app_server rebuild clipping and
// repaint the entire desktop (Desktop.cpp:3644-3663). Sending one per step
// would queue a full repaint per step of the drag, so the size is only
// requested once the window has held still for `settle`.
class ResizeSettler {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds settle {250};

    // The window's drawable is now `size`. A zero or negative dimension (a
    // minimised window) is ignored.
    void window_resized(DisplaySize size, Clock::time_point now);
    // Forget a pending size (fit-to-window was switched off).
    void cancel() { pending_.reset(); }
    // The size to request, once it has settled and differs from `current`;
    // clamped to Surface::max_dimension. Returned once.
    [[nodiscard]] std::optional<DisplaySize> take(DisplaySize current,
                                                  Clock::time_point now);
    // When a pending size settles, for the front end's event-wait timeout.
    [[nodiscard]] std::optional<Clock::time_point> deadline() const;

private:
    std::optional<DisplaySize> pending_;
    Clock::time_point changed_at_ {};
};

// Reduce a failed connect_with_progress() to the reconnect policy's question.
// A missing credential stays one; a refused or untrusted credential (a denied
// broker token, a certificate that is unknown, changed, pinned elsewhere or
// fails verification) and a session refused at the gate are decisions, not
// blips, and map to non-retriable endings; everything else -- the host or the
// listener not being up yet -- is a retriable connect failure.
[[nodiscard]] ConnectionOutcome outcome_for_connect_error(const ConnectError& error);

class DesktopShell {
public:
    using Clock = std::chrono::steady_clock;

    enum class Action {
        none,
        disconnect,       // back to the library
        reconnect,        // tear the route down and build it again now
        repaint,         // request_repaint()
        fullscreen,       // toggle the window's fullscreen state
        stats,            // toggle the statistics panel
        shortcut,         // send take_shortcut()
        display_size,     // act on take_display_size()
    };

    // What the bar says about the link.
    enum class Link {
        connected,
        connecting,   // building a route now
        waiting,      // a reconnect is scheduled
        lost,         // not connected and no reconnect is scheduled
    };

    struct Region {
        // `keys` swaps the bar's buttons for the shortcut row and `sizes` for
        // the resolution row; `back` swaps them back. Keeping the shortcuts in the bar rather than a drop-down
        // keeps the overlay one fixed rectangle.
        enum class Kind {
            disconnect, reconnect, repaint, fullscreen, keys, stats, shortcut,
            back, sizes, fit, window_size, preset
        };
        Kind kind;
        int index = 0; // the shortcut or the preset
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;

        [[nodiscard]] bool hit(int px, int py) const
        {
            return px >= x && px < x + w && py >= y && py < y + h;
        }
    };

    static constexpr int bar_height = 40;
    // How close to the top edge the pointer must come to reveal the bar.
    static constexpr int reveal_band = 2;
    static constexpr std::chrono::milliseconds hide_delay {1200};

    DesktopShell(std::string name, int desktop_width, int desktop_height);

    // The remote desktop changed size (a live resolution change): re-centre
    // the bar, clamp it and the stats panel to the new width, and re-layout.
    void set_desktop_size(int width, int height);
    [[nodiscard]] DisplaySize desktop_size() const
    {
        return {desktop_width_, desktop_height_};
    }

    // Whether fit-to-window is on: the front end then follows the window's
    // size through a ResizeSettler. Switched by the resolution row's Fit
    // button; choosing a fixed size switches it off.
    [[nodiscard]] bool fit_to_window() const { return fit_to_window_; }

    // The status line. `detail` qualifies it ("reconnecting in 3 s", why it was
    // lost, the route in use).
    void set_link(Link link, std::string detail);
    [[nodiscard]] Link link() const { return link_; }

    // Pointer input in desktop coordinates. Each returns true when the shell
    // owns the event, which the front end must then not forward to the remote.
    // A release is owned exactly when its press was.
    bool pointer_move(int x, int y, Clock::time_point now);
    bool pointer_press(int x, int y, Clock::time_point now);
    bool pointer_release();

    // The hotkey: pin the bar open, or unpin and hide it.
    void toggle();
    // Run the hide timer. Call once per loop iteration.
    void tick(Clock::time_point now);

    [[nodiscard]] bool visible() const;
    [[nodiscard]] bool stats_visible() const { return stats_visible_; }
    void set_stats_visible(bool shown);
    void set_stats_lines(std::vector<std::string> lines);

    // Where the bar sits on the desktop: centred at the top, never wider than
    // the desktop.
    [[nodiscard]] IntRect bar_rect() const;
    // Where the stats panel sits: the bottom-left corner.
    [[nodiscard]] IntRect stats_rect() const;

    // Draw the bar into its own surface, sized bar_rect().
    const Surface& render_bar();
    const Surface& render_stats();
    // Whether the bar or the stats panel changed since the front end last
    // presented them; it calls mark_clean() once it has.
    [[nodiscard]] bool dirty() const { return dirty_; }
    void mark_clean() { dirty_ = false; }

    [[nodiscard]] Action take_action();
    // The shortcut chosen with Action::shortcut.
    [[nodiscard]] const RemoteShortcut* take_shortcut();
    // The choice made with Action::display_size. Read once.
    [[nodiscard]] std::optional<DisplaySizeChoice> take_display_size();

    [[nodiscard]] const std::vector<Region>& regions() const { return regions_; }
    // True while the bar shows the shortcut row instead of the main buttons.
    [[nodiscard]] bool keys_mode() const { return row_ == Row::keys; }
    // True while the bar shows the resolution row.
    [[nodiscard]] bool sizes_mode() const { return row_ == Row::sizes; }

private:
    enum class Row { main, keys, sizes };
    void layout();
    void show_row(Row row);
    [[nodiscard]] bool inside_bar(int x, int y) const;
    void draw_text(Surface& target, std::string_view text, int x, int baseline,
                   Color color, float size);
    void draw_button(const Region& region, std::string_view label);

    std::string name_;
    int desktop_width_;
    int desktop_height_;
    Link link_ = Link::connecting;
    std::string detail_;
    TextEngine text_;
    Surface bar_;
    Surface stats_;
    std::vector<Region> regions_;
    std::vector<std::string> stats_lines_;
    bool pinned_ = false;
    bool hovered_ = false;
    std::optional<Clock::time_point> hide_at_;
    Row row_ = Row::main;
    bool fit_to_window_ = false;
    std::optional<DisplaySizeChoice> display_size_;
    bool stats_visible_ = false;
    bool press_owned_ = false;
    int pointer_x_ = -1;
    int pointer_y_ = -1;
    Action action_ = Action::none;
    const RemoteShortcut* shortcut_ = nullptr;
    bool dirty_ = true;
};

} // namespace haiku_remote
