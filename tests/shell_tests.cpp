// Tests for DeBeOS-RDP issue #1, "Desktop shell": the in-session toolbar, the
// pieces of glue between it and the session, and the time-driven reconnect
// schedule the GUI uses for auto-reconnect.
//
// House rules (CLAUDE.md): exact values only, expected values written down
// independently of the code under test (wire bytes as golden vectors, geometry
// from the documented constants), and asymmetric fixtures -- the bar is centred
// but its buttons are right-aligned, so a left/right mix-up cannot pass.

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/desktop_shell.hpp"
#include "haiku_remote/reconnect.hpp"
#include "haiku_remote/session.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace haiku_remote;
using namespace std::chrono_literals;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;

// --- remote shortcuts -------------------------------------------------------

void test_shortcut_wire_bytes()
{
    const auto shortcuts = remote_shortcuts();
    check(shortcuts.size() == 5, "five shortcuts are offered");
    check(shortcuts[0].label == "Cmd+F1", "the first shortcut is Cmd+F1");

    // Held locally: left Shift (B_SHIFT_KEY | B_LEFT_SHIFT_KEY = 0x101). The
    // chord must end by restoring exactly that, not by releasing everything.
    const auto messages = encode_shortcut(shortcuts[0], 0x101);
    check(messages.size() == 4, "a shortcut is four messages");
    if (messages.size() != 4)
        return;
    // RP_MODIFIERS_CHANGED (244): B_COMMAND_KEY | B_LEFT_COMMAND_KEY = 0x402.
    check(messages[0] == Bytes {0xf4, 0x00, 0x0a, 0, 0, 0, 0x02, 0x04, 0x00, 0x00},
          "Command is pressed first (RP_MODIFIERS_CHANGED 0x402)");
    // RP_KEY_DOWN (240): one byte B_FUNCTION_KEY (0x10), raw_char 0x10, key
    // B_F1_KEY (0x02). 6 header + 4 + 1 + 4 + 4 = 19 bytes.
    check(messages[1]
              == Bytes {0xf0, 0x00, 0x13, 0, 0, 0, 0x01, 0, 0, 0, 0x10, 0x10, 0, 0,
                        0, 0x02, 0, 0, 0},
          "F1 goes down as B_F1_KEY with B_FUNCTION_KEY bytes");
    check(messages[2]
              == Bytes {0xf1, 0x00, 0x13, 0, 0, 0, 0x01, 0, 0, 0, 0x10, 0x10, 0, 0,
                        0, 0x02, 0, 0, 0},
          "F1 comes up (RP_KEY_UP 241)");
    check(messages[3] == Bytes {0xf4, 0x00, 0x0a, 0, 0, 0, 0x01, 0x01, 0x00, 0x00},
          "the modifiers really held (0x101) are restored last");

    // Previous workspace: Command+` -- key 0x11, byte and raw_char 0x60.
    const auto back = encode_shortcut(shortcuts[4], 0);
    check(back.size() == 4
              && back[1]
                  == Bytes {0xf0, 0x00, 0x13, 0, 0, 0, 0x01, 0, 0, 0, 0x60, 0x60,
                            0, 0, 0, 0x11, 0, 0, 0},
          "Cmd+` goes down as key 0x11 with byte 0x60");
    check(back.size() == 4
              && back[3] == Bytes {0xf4, 0x00, 0x0a, 0, 0, 0, 0, 0, 0, 0},
          "with nothing held, the chord ends with no modifiers");
}

// --- repaint lever ----------------------------------------------------------

void test_repaint_lever()
{
    std::vector<Bytes> sent;
    // Asymmetric size: a width/height swap cannot pass.
    Session session(1024, 700, [&](std::span<const std::uint8_t> bytes) {
        sent.emplace_back(bytes.begin(), bytes.end());
        return true;
    });
    session.start();
    sent.clear();

    // No RP_HELLO_ACK yet: nothing negotiated, so the lever every server
    // honours -- RP_UPDATE_DISPLAY_MODE (2) at the current size (§9.2).
    check(request_repaint(session) == RepaintLever::display_mode,
          "without RP_CAP_RESYNC the repaint is a display-mode change");
    check(sent.size() == 1
              && sent[0]
                  == Bytes {0x02, 0x00, 0x0e, 0, 0, 0, 0x00, 0x04, 0, 0, 0xbc, 0x02,
                            0, 0},
          "RP_UPDATE_DISPLAY_MODE carries 1024 x 700");

    // RP_HELLO_ACK (7) granting RP_CAP_RESYNC (1 << 2), session 0x11223344,
    // generation 5: 6 header + 16 payload = 22 bytes.
    const Bytes ack {0x07, 0x00, 0x16, 0, 0, 0, 0x01, 0, 0, 0, 0x04, 0, 0, 0,
                     0x44, 0x33, 0x22, 0x11, 0x05, 0, 0, 0};
    session.ingest(ack);
    check(session.negotiated_capabilities() == 0x04,
          "the ack negotiated exactly RP_CAP_RESYNC");
    sent.clear();
    check(request_repaint(session) == RepaintLever::resync,
          "with RP_CAP_RESYNC the repaint is RP_RESYNC");
    check(sent.size() == 1
              && sent[0] == Bytes {0x08, 0x00, 0x0a, 0, 0, 0, 0x05, 0, 0, 0},
          "RP_RESYNC (8) carries the generation we last saw (5)");

    Session dead(64, 48, [](std::span<const std::uint8_t>) { return false; });
    check(request_repaint(dead) == RepaintLever::failed,
          "a send that fails is reported as failed, not as a repaint");
}

// --- reconnect config and connect-error mapping ------------------------------

void test_profile_reconnect_config()
{
    ConnectionProfile on;
    on.auto_reconnect = true;
    const ReconnectConfig a = reconnect_config_for(on);
    check(a.enabled, "autoReconnect=true enables reconnect");
    check(a.max_attempts == 5 && a.base_backoff == 200ms && a.max_backoff == 5000ms,
          "with the policy's bounded backoff (5 attempts, 200 ms doubling to 5 s)");

    ConnectionProfile off;
    off.auto_reconnect = false;
    check(!reconnect_config_for(off).enabled, "autoReconnect=false disables it");
}

void test_connect_error_outcomes()
{
    const auto outcome = [](ConnectErrorCause cause) {
        ConnectError error;
        error.cause = cause;
        return outcome_for_connect_error(error);
    };
    check(outcome(ConnectErrorCause::missing_credential)
              == ConnectionOutcome::missing_credential,
          "a missing credential stays a missing credential");
    for (ConnectErrorCause cause :
         {ConnectErrorCause::broker_token_denied, ConnectErrorCause::broker_cert_pin,
          ConnectErrorCause::broker_cert_untrusted,
          ConnectErrorCause::broker_cert_unknown,
          ConnectErrorCause::broker_cert_changed,
          ConnectErrorCause::session_refused, ConnectErrorCause::remote_closed,
          ConnectErrorCause::ssh_auth_failed, ConnectErrorCause::ssh_host_key,
          ConnectErrorCause::ssh_not_found, ConnectErrorCause::identity_missing,
          ConnectErrorCause::identity_unreadable,
          ConnectErrorCause::decode_failed, ConnectErrorCause::cancelled})
        check(outcome(cause) == ConnectionOutcome::refused,
              "a refusal, a decision or local configuration is not a blip");
    for (ConnectErrorCause cause :
         {ConnectErrorCause::host_unreachable, ConnectErrorCause::host_not_found,
          ConnectErrorCause::ssh_refused, ConnectErrorCause::ssh_disconnected,
          ConnectErrorCause::ssh_failed, ConnectErrorCause::local_port_in_use,
          ConnectErrorCause::forward_timeout, ConnectErrorCause::tcp_failed,
          ConnectErrorCause::handshake_timeout,
          ConnectErrorCause::broker_unavailable,
          ConnectErrorCause::cookie_unavailable,
          ConnectErrorCause::server_listener, ConnectErrorCause::other})
        check(outcome(cause) == ConnectionOutcome::connect_failed,
              "the far end not being up yet is a retriable connect failure");
}

// --- endings through an SSH forward ------------------------------------------

void test_forwarded_endings()
{
    // A reset before any message, through ssh -L: ssh could not open the
    // remote channel (nothing listening yet). Retriable.
    ConnectionResult no_listener;
    no_listener.connected = true;
    no_listener.connection_reset = true;
    no_listener.through_forward = true;
    check(classify_connection(no_listener) == ConnectionOutcome::connect_failed,
          "a reset before any message through a forward is a connect failure");

    // The same reset on app_server's own socket is its refusal signature.
    ConnectionResult direct = no_listener;
    direct.through_forward = false;
    check(classify_connection(direct) == ConnectionOutcome::evicted,
          "the same reset on a direct socket is still a refusal");

    // After a session started, a reset is an eviction, forward or not.
    ConnectionResult evicted = no_listener;
    evicted.message_count = 12;
    check(classify_connection(evicted) == ConnectionOutcome::evicted,
          "a reset after messages through a forward is still an eviction");

    // app_server refusing through the forward arrives as a FIN with nothing
    // decoded: still a refusal.
    ConnectionResult refused;
    refused.connected = true;
    refused.peer_closed = true;
    refused.through_forward = true;
    check(classify_connection(refused) == ConnectionOutcome::refused,
          "a FIN with no messages through a forward is a refusal");
}

// --- the reconnect schedule ---------------------------------------------------

void test_reconnect_schedule()
{
    ReconnectConfig config;
    config.enabled = true;
    config.max_attempts = 3;
    const Clock::time_point t0 {};

    ReconnectSchedule s(config);
    check(!s.pending() && s.attempts_made() == 0, "nothing is pending at first");
    check(s.connection_ended(ConnectionOutcome::transport_dropped, t0),
          "a transport drop schedules a reconnect");
    check(s.due_at() == t0 + 200ms, "the first reconnect is due after 200 ms");
    check(!s.due(t0 + 199ms) && s.due(t0 + 200ms), "and not a millisecond sooner");

    s.attempt_started();
    check(s.attempts_made() == 1 && !s.pending(), "an attempt uses the budget");
    const Clock::time_point t1 = t0 + 1s;
    check(s.connection_ended(ConnectionOutcome::connect_failed, t1)
              && s.due_at() == t1 + 400ms,
          "a failed attempt backs off to 400 ms");
    s.attempt_started();
    const Clock::time_point t2 = t0 + 2s;
    check(s.connection_ended(ConnectionOutcome::connect_failed, t2)
              && s.due_at() == t2 + 800ms,
          "then 800 ms");
    s.attempt_started();
    check(!s.connection_ended(ConnectionOutcome::connect_failed, t0 + 3s),
          "the fourth is refused: the budget was 3");
    check(s.gave_up() && s.exhausted() && !s.pending(),
          "it gave up because the budget ran out");

    // A connection that carried a session refills the budget: the next outage
    // starts again at the base backoff with every attempt available.
    ReconnectSchedule r(config);
    r.connection_ended(ConnectionOutcome::transport_dropped, t0);
    r.attempt_started();
    r.connection_ended(ConnectionOutcome::connect_failed, t0);
    r.attempt_started();
    check(r.attempts_made() == 2, "two attempts made in the first outage");
    r.established();
    check(r.attempts_made() == 0, "an established session refills the budget");
    const Clock::time_point t5 = t0 + 5s;
    check(r.connection_ended(ConnectionOutcome::transport_dropped, t5)
              && r.due_at() == t5 + 200ms,
          "the next outage starts again at 200 ms, not 800 ms");

    ReconnectSchedule e(config);
    check(!e.connection_ended(ConnectionOutcome::evicted, t0),
          "an eviction is not retried");
    check(e.gave_up() && !e.exhausted(), "and that is not budget exhaustion");
    check(e.last_outcome() == ConnectionOutcome::evicted, "the outcome is kept");

    // Reconnect off: nothing is automatic, but the user may still ask.
    ReconnectSchedule off {ReconnectConfig {}};
    check(!off.connection_ended(ConnectionOutcome::transport_dropped, t0),
          "with reconnect off a drop is not retried");
    check(off.gave_up() && !off.exhausted(), "and that is not exhaustion either");
    off.request_now(t0 + 7s);
    check(off.pending() && off.due(t0 + 7s) && !off.gave_up(),
          "a requested reconnect is due at once");
    off.attempt_started();
    check(off.attempts_made() == 1, "and counts as an attempt");
    off.cancel();
    check(!off.pending(), "cancel drops a pending attempt");
}

// --- the toolbar --------------------------------------------------------------

// Geometry, from the documented layout constants: a 1000-px desktop takes the
// full 760-px bar, centred, so it spans x 120..879 and y 0..39. Six 80-px
// buttons with 6-px gaps are right-aligned 8 px from the bar's right edge: the
// first starts at 760 - 8 - 6*86 + 6 = 242 (bar-local), i.e. x 362 on the
// desktop, and the sixth ends at 751 (bar-local). Buttons sit at y 7..32.
constexpr int desktop_w = 1000;
constexpr int desktop_h = 700;
constexpr int bar_left = 120;
constexpr int first_button = 242;
constexpr int pitch = 86;

int button_x(int index) // desktop x of a button's left edge
{
    return bar_left + first_button + index * pitch;
}

void test_shell_geometry_and_reveal()
{
    DesktopShell shell("Lab box", desktop_w, desktop_h);
    const IntRect bar = shell.bar_rect();
    check(bar.left == 120 && bar.right == 879 && bar.top == 0 && bar.bottom == 39,
          "the bar is 760 x 40, centred at the top of a 1000-px desktop");

    check(shell.visible(), "the bar is shown while the link is still connecting");
    shell.set_link(DesktopShell::Link::connected, "");
    check(!shell.visible(), "and hidden once connected: no remote pixels taken");

    const Clock::time_point t0 {};
    check(!shell.pointer_move(119, 0, t0) && !shell.visible(),
          "the top edge left of the bar's span does not reveal it");
    check(!shell.pointer_move(880, 0, t0) && !shell.visible(),
          "nor does the top edge right of it");
    check(!shell.pointer_move(500, 2, t0) && !shell.visible(),
          "y = 2 is outside the 2-px reveal band");
    check(shell.pointer_move(120, 1, t0) && shell.visible(),
          "touching the top edge over the bar reveals it, and the move is the bar's");

    check(shell.pointer_move(879, 39, t0), "the bar's far corner is still the bar");
    check(!shell.pointer_move(500, 300, t0), "a move below the bar goes to the remote");
    shell.tick(t0 + 1199ms);
    check(shell.visible(), "the bar lingers for the hide delay");
    shell.tick(t0 + 1200ms);
    check(!shell.visible(), "and hides at 1200 ms");

    // Coming back before the delay runs out cancels the hide.
    shell.pointer_move(500, 0, t0);
    shell.pointer_move(500, 300, t0);
    shell.pointer_move(500, 20, t0 + 500ms);
    shell.tick(t0 + 1300ms);
    check(shell.visible(), "re-entering the bar cancels the pending hide");

    // The hotkey pins it open regardless of the pointer.
    DesktopShell pinned("Lab box", desktop_w, desktop_h);
    pinned.set_link(DesktopShell::Link::connected, "");
    pinned.toggle();
    check(pinned.visible(), "the hotkey shows the bar");
    pinned.pointer_move(500, 300, t0);
    pinned.tick(t0 + 10s);
    check(pinned.visible(), "a pinned bar does not hide on its own");
    pinned.toggle();
    check(!pinned.visible(), "the hotkey hides it again");

    // Hidden, the bar owns nothing: a click where it would be goes to the remote.
    check(!pinned.pointer_press(button_x(0) + 1, 8, t0),
          "a hidden bar does not swallow a click");
    check(!pinned.pointer_release(), "nor its release");

    // A link that is down forces the bar visible even with no pointer near it.
    pinned.set_link(DesktopShell::Link::waiting, "attempt 1 of 5 in 1 s");
    check(pinned.visible(), "a waiting link shows the bar");
    pinned.set_link(DesktopShell::Link::lost, "transport dropped");
    check(pinned.visible(), "a lost link shows the bar");

    // A narrow desktop: the bar is clamped to the desktop width.
    DesktopShell narrow("n", 600, 400);
    const IntRect nb = narrow.bar_rect();
    check(nb.left == 0 && nb.right == 599, "a 600-px desktop gets a 600-px bar");
}

void test_shell_actions()
{
    const Clock::time_point t0 {};
    DesktopShell shell("Lab box", desktop_w, desktop_h);
    shell.set_link(DesktopShell::Link::connected, "");
    shell.toggle();

    check(shell.pointer_press(button_x(0) - 1, 8, t0),
          "a click in the bar's gap is owned by the bar");
    check(shell.take_action() == DesktopShell::Action::none,
          "but is no button (x 361 is one left of Disconnect)");
    check(shell.pointer_release(), "its release is owned too");

    struct Expect {
        int index;
        DesktopShell::Action action;
        const char* name;
    };
    for (const Expect& e :
         {Expect {0, DesktopShell::Action::disconnect, "Disconnect"},
          Expect {1, DesktopShell::Action::reconnect, "Reconnect"},
          Expect {2, DesktopShell::Action::repaint, "Repaint"},
          Expect {3, DesktopShell::Action::fullscreen, "Full screen"}}) {
        check(shell.pointer_press(button_x(e.index), 7, t0)
                  && shell.take_action() == e.action,
              std::string("the top-left pixel of ") + e.name + " presses it");
        check(shell.pointer_press(button_x(e.index) + 79, 32, t0)
                  && shell.take_action() == e.action,
              std::string("the bottom-right pixel of ") + e.name + " presses it");
        shell.pointer_release();
    }
    check(shell.take_action() == DesktopShell::Action::none,
          "an action is read once");

    check(!shell.stats_visible(), "stats start hidden");
    shell.pointer_press(button_x(5) + 40, 20, t0);
    check(shell.take_action() == DesktopShell::Action::stats && shell.stats_visible(),
          "Stats toggles the panel on");
    shell.pointer_press(button_x(5) + 40, 20, t0);
    check(shell.take_action() == DesktopShell::Action::stats
              && !shell.stats_visible(),
          "and off");

    // Keys swaps the row for the shortcuts, in the same slots.
    shell.pointer_press(button_x(4) + 10, 10, t0);
    check(shell.keys_mode() && shell.take_action() == DesktopShell::Action::none,
          "Keys opens the shortcut row without sending anything");
    shell.pointer_press(button_x(0) + 10, 10, t0);
    check(shell.take_action() == DesktopShell::Action::shortcut,
          "the first slot is a shortcut");
    const RemoteShortcut* first = shell.take_shortcut();
    check(first != nullptr && first->key == 0x02 && first->label == "Cmd+F1",
          "the first slot sends Cmd+F1");
    check(shell.take_shortcut() == nullptr, "the shortcut is read once");
    shell.pointer_press(button_x(1) + 10, 10, t0);
    (void)shell.take_action();
    const RemoteShortcut* second = shell.take_shortcut();
    check(second != nullptr && second->key == 0x03, "the second sends Cmd+F2");
    shell.pointer_press(button_x(5) + 10, 10, t0);
    check(!shell.keys_mode() && shell.take_action() == DesktopShell::Action::none,
          "the last slot is Back");
}

void test_shell_pixels()
{
    constexpr Color panel {36, 39, 48, 255};
    constexpr Color border {70, 76, 92, 255};
    constexpr Color face {52, 56, 68, 255};
    constexpr Color hot {74, 144, 226, 255};
    constexpr Color stats_bg {24, 26, 32, 255};

    const Clock::time_point t0 {};
    DesktopShell shell("Lab box", desktop_w, desktop_h);
    shell.set_link(DesktopShell::Link::connected, "");
    shell.toggle();
    const Surface& bar = shell.render_bar();
    check(bar.width() == 760 && bar.height() == 40, "the bar surface is 760 x 40");
    check(bar.pixel(0, 0) == panel, "the bar's background is the panel colour");
    check(bar.pixel(0, 39) == border && bar.pixel(759, 39) == border,
          "its bottom row is the border");
    check(bar.pixel(241, 8) == panel, "x 241 is the gap before the first button");
    check(bar.pixel(242, 7) == face, "the first button starts at (242, 7)");
    check(bar.pixel(242, 6) == panel, "and not a row higher");
    check(bar.pixel(751, 32) == face, "the last button ends at (751, 32)");
    check(bar.pixel(752, 32) == panel && bar.pixel(759, 20) == panel,
          "leaving an 8-px right margin");

    shell.pointer_move(button_x(0) + 2, 9, t0);
    const Surface& hovered = shell.render_bar();
    check(hovered.pixel(242, 7) == hot, "the button under the pointer lights up");
    check(hovered.pixel(242 + pitch, 7) == face, "and only that one");

    shell.set_stats_lines({"messages: 1", "received: 0.1 KiB", "frames/s: 60.0"});
    const IntRect sr = shell.stats_rect();
    check(sr.left == 0 && sr.right == 299 && sr.top == 645 && sr.bottom == 699,
          "three stats lines take a 300 x 55 panel in the bottom-left corner");
    const Surface& stats = shell.render_stats();
    check(stats.width() == 300 && stats.height() == 55, "the stats surface is 300 x 55");
    check(stats.pixel(299, 0) == stats_bg && stats.pixel(0, 54) == stats_bg,
          "the stats panel has its own background");
}

} // namespace

int main()
{
    test_shortcut_wire_bytes();
    test_repaint_lever();
    test_profile_reconnect_config();
    test_connect_error_outcomes();
    test_forwarded_endings();
    test_reconnect_schedule();
    test_shell_geometry_and_reveal();
    test_shell_actions();
    test_shell_pixels();

    if (failures == 0) {
        std::cout << "PASS - " << checks << " desktop-shell checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " desktop-shell checks FAILED\n";
    return 1;
}
