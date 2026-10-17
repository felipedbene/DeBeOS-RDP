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
#include "haiku_remote/surface.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
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
    check(a.max_attempts == 10 && a.base_backoff == 500ms && a.max_backoff == 4000ms,
          "with the GUI's reboot-sized backoff (10 attempts, 500 ms doubling to 4 s)");
    // The schedule written out from those three numbers, not from the code:
    // 500, 1000, 2000, then the 4000 cap for the remaining seven.
    const ReconnectPolicy policy(a);
    const std::chrono::milliseconds expected[] {500ms, 1000ms, 2000ms, 4000ms,
                                                4000ms, 4000ms, 4000ms, 4000ms,
                                                4000ms, 4000ms};
    bool schedule_matches = true;
    for (int i = 0; i < 10; ++i)
        schedule_matches = schedule_matches && policy.backoff_for(i) == expected[i];
    check(schedule_matches, "the waits are 0.5, 1, 2, then 4 s seven times");
    check(total_backoff(a) == 31500ms,
          "31.5 s of waiting in all: twice the slowest measured boot (15 s)");
    check(policy.should_retry(ConnectionOutcome::connect_failed, 9)
              && !policy.should_retry(ConnectionOutcome::connect_failed, 10),
          "the tenth attempt is the last");
    // The command line keeps its own defaults (another path owns those).
    check(total_backoff(ReconnectConfig {}) == 6200ms,
          "the CLI default waits 6.2 s in all (0.2 + 0.4 + 0.8 + 1.6 + 3.2)");

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
          ConnectErrorCause::decode_failed, ConnectErrorCause::cancelled,
          ConnectErrorCause::quic_alpn_mismatch})
        check(outcome(cause) == ConnectionOutcome::refused,
              "a refusal, a decision or local configuration is not a blip");
    for (ConnectErrorCause cause :
         {ConnectErrorCause::host_unreachable, ConnectErrorCause::host_not_found,
          ConnectErrorCause::ssh_refused, ConnectErrorCause::ssh_disconnected,
          ConnectErrorCause::ssh_failed, ConnectErrorCause::local_port_in_use,
          ConnectErrorCause::forward_timeout, ConnectErrorCause::tcp_failed,
          ConnectErrorCause::handshake_timeout,
          ConnectErrorCause::quic_udp_blocked,
          ConnectErrorCause::quic_handshake_timeout,
          ConnectErrorCause::broker_unavailable,
          ConnectErrorCause::cookie_unavailable,
          ConnectErrorCause::server_listener, ConnectErrorCause::other})
        check(outcome(cause) == ConnectionOutcome::connect_failed,
              "the far end not being up yet is a retriable connect failure");
}

// --- endings through an SSH forward ------------------------------------------

void test_route_status()
{
    ConnectionProfile profile;
    profile.host = "h.example";
    profile.ssh_user = "baron";
    profile.ssh_port = 2222;
    ManagedConnection broker;
    broker.ok = true;
    broker.kind = RouteKind::broker;
    broker.transport.url = "quic://h.example:10902";
    check(route_status(profile, broker) == "QUIC via broker h.example:10902",
          "the bar names a QUIC route as QUIC");
    broker.transport.url = "wss://h.example:10902";
    check(route_status(profile, broker) == "WSS via broker h.example:10902",
          "and a wss route as WSS, on the same broker port");
    ManagedConnection tunnel;
    tunnel.ok = true;
    tunnel.kind = RouteKind::tunnel;
    tunnel.transport.url = "";
    check(route_status(profile, tunnel) == "SSH tunnel via baron@h.example:2222",
          "a tunnel names its ssh target");
    check(route_status(profile, ManagedConnection {}).empty(),
          "nothing for a connection that is not up");
}

void test_quic_connect_endings()
{
    // classify_connection() is what the endpoint paths (--url quic://) read.
    ConnectionResult alpn;
    alpn.connect_failure = ConnectFailure::quic_alpn_refused;
    check(classify_connection(alpn) == ConnectionOutcome::refused,
          "a refused QUIC ALPN is not retried: the broker refuses it every time");
    ConnectionResult silent;
    silent.connect_failure = ConnectFailure::quic_no_answer;
    check(classify_connection(silent) == ConnectionOutcome::connect_failed,
          "no QUIC answer yet is a retriable connect failure (a booting broker)");
}

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
// full 860-px bar, centred, so it spans x 70..929 and y 0..39. Seven 80-px
// buttons with 6-px gaps are right-aligned 8 px from the bar's right edge: the
// first starts at 860 - 8 - 7*86 + 6 = 256 (bar-local), i.e. x 326 on the
// desktop, and the seventh ends at 851 (bar-local). Buttons sit at y 7..32.
// The keys row (six) starts at 860 - 8 - 6*86 + 6 = 342; the resolution row
// (eight) at 860 - 8 - 8*86 + 6 = 170.
constexpr int desktop_w = 1000;
constexpr int desktop_h = 700;
constexpr int bar_left = 70;
constexpr int first_button = 256;
constexpr int first_key = 342;
constexpr int first_size = 170;
constexpr int pitch = 86;

int button_x(int index) // desktop x of a main-row button's left edge
{
    return bar_left + first_button + index * pitch;
}

int key_x(int index) // ... of a keys-row slot
{
    return bar_left + first_key + index * pitch;
}

int size_x(int index) // ... of a resolution-row slot
{
    return bar_left + first_size + index * pitch;
}

void test_shell_geometry_and_reveal()
{
    DesktopShell shell("Lab box", desktop_w, desktop_h);
    const IntRect bar = shell.bar_rect();
    check(bar.left == 70 && bar.right == 929 && bar.top == 0 && bar.bottom == 39,
          "the bar is 860 x 40, centred at the top of a 1000-px desktop");

    check(shell.visible(), "the bar is shown while the link is still connecting");
    shell.set_link(DesktopShell::Link::connected, "");
    check(!shell.visible(), "and hidden once connected: no remote pixels taken");

    const Clock::time_point t0 {};
    check(!shell.pointer_move(69, 0, t0) && !shell.visible(),
          "the top edge left of the bar's span does not reveal it");
    check(!shell.pointer_move(930, 0, t0) && !shell.visible(),
          "nor does the top edge right of it");
    check(!shell.pointer_move(500, 2, t0) && !shell.visible(),
          "y = 2 is outside the 2-px reveal band");
    check(shell.pointer_move(70, 1, t0) && shell.visible(),
          "touching the top edge over the bar reveals it, and the move is the bar's");

    check(shell.pointer_move(929, 39, t0), "the bar's far corner is still the bar");
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
          "but is no button (x 325 is one left of Disconnect)");
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
    shell.pointer_press(button_x(6) + 40, 20, t0);
    check(shell.take_action() == DesktopShell::Action::stats && shell.stats_visible(),
          "Stats toggles the panel on");
    shell.pointer_press(button_x(6) + 40, 20, t0);
    check(shell.take_action() == DesktopShell::Action::stats
              && !shell.stats_visible(),
          "and off");

    // Keys swaps the row for the shortcuts, right-aligned like the main row.
    shell.pointer_press(button_x(5) + 10, 10, t0);
    check(shell.keys_mode() && shell.take_action() == DesktopShell::Action::none,
          "Keys opens the shortcut row without sending anything");
    shell.pointer_press(key_x(0) - 1, 10, t0);
    check(shell.take_action() == DesktopShell::Action::none,
          "one pixel left of the first shortcut is no shortcut");
    shell.pointer_press(key_x(0) + 10, 10, t0);
    check(shell.take_action() == DesktopShell::Action::shortcut,
          "the first slot is a shortcut");
    const RemoteShortcut* first = shell.take_shortcut();
    check(first != nullptr && first->key == 0x02 && first->label == "Cmd+F1",
          "the first slot sends Cmd+F1");
    check(shell.take_shortcut() == nullptr, "the shortcut is read once");
    shell.pointer_press(key_x(1) + 10, 10, t0);
    (void)shell.take_action();
    const RemoteShortcut* second = shell.take_shortcut();
    check(second != nullptr && second->key == 0x03, "the second sends Cmd+F2");
    shell.pointer_press(key_x(5) + 10, 10, t0);
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
    check(bar.width() == 860 && bar.height() == 40, "the bar surface is 860 x 40");
    check(bar.pixel(0, 0) == panel, "the bar's background is the panel colour");
    check(bar.pixel(0, 39) == border && bar.pixel(859, 39) == border,
          "its bottom row is the border");
    check(bar.pixel(255, 8) == panel, "x 255 is the gap before the first button");
    check(bar.pixel(256, 7) == face, "the first button starts at (256, 7)");
    check(bar.pixel(256, 6) == panel, "and not a row higher");
    check(bar.pixel(851, 32) == face, "the last button ends at (851, 32)");
    check(bar.pixel(852, 32) == panel && bar.pixel(859, 20) == panel,
          "leaving an 8-px right margin");

    shell.pointer_move(button_x(0) + 2, 9, t0);
    const Surface& hovered = shell.render_bar();
    check(hovered.pixel(256, 7) == hot, "the button under the pointer lights up");
    check(hovered.pixel(256 + pitch, 7) == face, "and only that one");

    shell.set_stats_lines({"messages: 1", "received: 0.1 KiB", "frames/s: 60.0"});
    const IntRect sr = shell.stats_rect();
    check(sr.left == 0 && sr.right == 299 && sr.top == 645 && sr.bottom == 699,
          "three stats lines take a 300 x 55 panel in the bottom-left corner");
    const Surface& stats = shell.render_stats();
    check(stats.width() == 300 && stats.height() == 55, "the stats surface is 300 x 55");
    check(stats.pixel(299, 0) == stats_bg && stats.pixel(0, 54) == stats_bg,
          "the stats panel has its own background");
}

// --- live resolution change -------------------------------------------------

// A server message: uint16 code, uint32 total length (header included), body.
Bytes server_message(std::uint16_t code, const Bytes& body)
{
    const auto total = static_cast<std::uint32_t>(6 + body.size());
    Bytes out {static_cast<std::uint8_t>(code & 0xff),
               static_cast<std::uint8_t>(code >> 8),
               static_cast<std::uint8_t>(total & 0xff),
               static_cast<std::uint8_t>((total >> 8) & 0xff),
               static_cast<std::uint8_t>((total >> 16) & 0xff),
               static_cast<std::uint8_t>(total >> 24)};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void put_i32(Bytes& out, std::int32_t value)
{
    const auto v = static_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}

void put_f32(Bytes& out, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xff));
}

void put_rect(Bytes& out, float l, float t, float r, float b)
{
    put_f32(out, l);
    put_f32(out, t);
    put_f32(out, r);
    put_f32(out, b);
}

// Opcodes, written down from RemoteMessage.h rather than taken from the
// client's own table.
constexpr std::uint16_t rp_create_state = 20;
constexpr std::uint16_t rp_set_high_color = 41;
constexpr std::uint16_t rp_constrain_clipping_region = 60;
constexpr std::uint16_t rp_fill_rect = 104;
constexpr std::uint16_t rp_fill_rect_color = 160;

Bytes create_state(std::int32_t token)
{
    Bytes body;
    put_i32(body, token);
    return server_message(rp_create_state, body);
}

Bytes set_high_color(std::int32_t token, Color c)
{
    Bytes body;
    put_i32(body, token);
    body.insert(body.end(), {c.r, c.g, c.b, c.a});
    return server_message(rp_set_high_color, body);
}

Bytes clip_to(std::int32_t token, float l, float t, float r, float b)
{
    Bytes body;
    put_i32(body, token);
    put_i32(body, 1);
    put_rect(body, l, t, r, b);
    return server_message(rp_constrain_clipping_region, body);
}

Bytes fill_rect(std::int32_t token, float l, float t, float r, float b)
{
    Bytes body;
    put_i32(body, token);
    put_rect(body, l, t, r, b);
    return server_message(rp_fill_rect, body);
}

Bytes fill_rect_color(std::int32_t token, float l, float t, float r, float b,
                      Color c)
{
    Bytes body;
    put_i32(body, token);
    put_rect(body, l, t, r, b);
    body.insert(body.end(), {c.r, c.g, c.b, c.a});
    return server_message(rp_fill_rect_color, body);
}

// Every pixel of `s` equals `expected(x, y)`; reports the first mismatch.
template <typename Expected>
bool every_pixel(const Surface& s, Expected expected, std::string& where)
{
    for (int y = 0; y < s.height(); ++y) {
        for (int x = 0; x < s.width(); ++x) {
            const Color want = expected(x, y);
            if (!(s.pixel(x, y) == want)) {
                const Color got = s.pixel(x, y);
                where = "(" + std::to_string(x) + "," + std::to_string(y) + ") is "
                    + std::to_string(got.r) + "," + std::to_string(got.g) + ","
                    + std::to_string(got.b) + "," + std::to_string(got.a);
                return false;
            }
        }
    }
    return true;
}

void test_surface_resize()
{
    constexpr Color black {0, 0, 0, 255};
    // Asymmetric: every pixel distinct, so a flip, a transpose or a row-stride
    // mix-up moves some value somewhere it does not belong.
    const auto original = [](int x, int y) {
        return Color {static_cast<std::uint8_t>(10 + x * 40),
                      static_cast<std::uint8_t>(20 + y * 70), 7, 255};
    };
    Surface s(5, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x)
            s.set_pixel(x, y, original(x, y));

    // Wider and shorter at once: columns 5..6 are new, row 2 is lost.
    s.resize(7, 2);
    check(s.width() == 7 && s.height() == 2 && s.stride() == 28,
          "resize(7, 2) gives a 7 x 2 canvas with a 28-byte stride");
    std::string where;
    check(every_pixel(s, [&](int x, int y) {
              return x < 5 ? original(x, y) : black; }, where),
          "the overlap keeps its coordinates and the new columns are black: "
              + where);

    // Narrower and taller: rows 2..3 are new (row 2's old pixels are gone,
    // not resurrected), columns 3..6 are lost.
    s.resize(3, 4);
    check(s.width() == 3 && s.height() == 4, "resize(3, 4) gives a 3 x 4 canvas");
    check(every_pixel(s, [&](int x, int y) {
              return y < 2 ? original(x, y) : black; }, where),
          "after shrinking the width and growing the height: " + where);

    bool threw = false;
    try {
        s.resize(0, 4);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "a zero width is refused");
    threw = false;
    try {
        s.resize(3, Surface::max_dimension + 1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "a height past max_dimension is refused");
    check(s.width() == 3 && s.height() == 4 && s.pixel(2, 1) == original(2, 1),
          "and a refused resize leaves the canvas exactly as it was");
}

void test_display_size_validation()
{
    check(valid_display_size(1, 1), "1 x 1 is a valid display size");
    check(valid_display_size(16384, 16384), "16384 x 16384 is the largest");
    check(!valid_display_size(0, 600), "a zero width is not");
    check(!valid_display_size(800, -1), "nor a negative height");
    check(!valid_display_size(16385, 600), "nor a width of 16385");
    check(!valid_display_size(800, 65536 + 600),
          "nor a height that would wrap to 600 in a uint16");
}

void test_display_mode_change_wire()
{
    std::vector<Bytes> sent;
    bool accept = true;
    Session session(1024, 700, [&](std::span<const std::uint8_t> bytes) {
        sent.emplace_back(bytes.begin(), bytes.end());
        return accept;
    });
    session.start();
    sent.clear();

    using Change = Session::DisplayModeChange;
    check(session.change_display_mode(1280, 800) == Change::sent,
          "a new size on a live session is sent");
    // RP_UPDATE_DISPLAY_MODE (2), 14 bytes: int32 1280 (0x500), int32 800
    // (0x320) -- RemoteHWInterface.cpp:985-987 reads width, then height.
    check(sent.size() == 1
              && sent[0]
                  == Bytes {0x02, 0x00, 0x0e, 0, 0, 0, 0x00, 0x05, 0, 0, 0x20, 0x03,
                            0, 0},
          "RP_UPDATE_DISPLAY_MODE carries 1280 x 800, width first");
    check(session.display_width() == 1280 && session.display_height() == 800
              && session.surface().width() == 1280
              && session.surface().height() == 800,
          "the session and its canvas are now 1280 x 800");

    sent.clear();
    check(session.change_display_mode(1280, 800) == Change::unchanged && sent.empty(),
          "asking for the current size sends nothing");
    check(session.change_display_mode(0, 800) == Change::invalid
              && session.change_display_mode(1280, 16385) == Change::invalid
              && sent.empty(),
          "an invalid size sends nothing");
    check(session.surface().width() == 1280 && session.surface().height() == 800,
          "and changes nothing");

    accept = false;
    check(session.change_display_mode(800, 600) == Change::send_failed,
          "a failed send is reported");
    check(session.display_width() == 800 && session.surface().height() == 600,
          "but the size is still adopted, for the next connection to ask for");

    // The repaint lever follows the new size too.
    accept = true;
    sent.clear();
    check(session.request_full_repaint() && sent.size() == 1
              && sent[0]
                  == Bytes {0x02, 0x00, 0x0e, 0, 0, 0, 0x20, 0x03, 0, 0, 0x58, 0x02,
                            0, 0},
          "a later full repaint asks for 800 x 600");
}

void test_reconnect_after_resize_asks_for_new_size()
{
    std::vector<Bytes> sent;
    Session session(1024, 700, [&](std::span<const std::uint8_t> bytes) {
        sent.emplace_back(bytes.begin(), bytes.end());
        return true;
    });
    // No connection: adopted locally, nothing sent.
    check(session.set_display_size(1366, 768) && sent.empty(),
          "set_display_size sends nothing");
    check(!session.set_display_size(1366, 0) && session.display_height() == 768,
          "and refuses an invalid size, keeping the old one");
    session.reset();
    session.start();
    // RP_HELLO (6): version, caps, max decode w/h, then the requested width and
    // height -- the last eight bytes. 1366 = 0x556, 768 = 0x300.
    check(sent.size() == 2 && sent[1].size() == 30
              && Bytes(sent[1].end() - 8, sent[1].end())
                  == Bytes {0x56, 0x05, 0, 0, 0x00, 0x03, 0, 0},
          "the next RP_HELLO asks for 1366 x 768");
    sent.clear();
    session.ingest(server_message(1, {})); // RP_INIT_CONNECTION accepted
    check(sent.size() == 2
              && sent[1]
                  == Bytes {0x02, 0x00, 0x0e, 0, 0, 0, 0x56, 0x05, 0, 0, 0x00, 0x03,
                            0, 0},
          "and the handshake's RP_UPDATE_DISPLAY_MODE carries 1366 x 768");
}

void test_pixels_after_resize()
{
    constexpr Color black {0, 0, 0, 255};
    constexpr Color old_paint {30, 160, 90, 255};
    constexpr Color marker {200, 40, 120, 255};
    constexpr Color stamp {250, 210, 20, 255};

    Session session(64, 48, [](std::span<const std::uint8_t>) { return true; });

    // Before the change: token 5 gets a high colour, token 6 a small clip, and
    // the old frame gets a mark at (2..4, 3..4).
    Bytes before;
    for (const Bytes& m : {create_state(5), set_high_color(5, marker),
                           clip_to(5, 0, 0, 63, 47), create_state(6),
                           clip_to(6, 0, 0, 9, 5),
                           fill_rect_color(5, 2, 3, 4, 4, old_paint)})
        before.insert(before.end(), m.begin(), m.end());
    session.ingest(before);

    check(session.change_display_mode(128, 80) == Session::DisplayModeChange::sent,
          "the session grows from 64 x 48 to 128 x 80");

    // What app_server sends next: the clip it rebuilt for the bigger screen
    // (RemoteDrawingEngine::ConstrainClippingRegion only sends a *changed*
    // region, :194-205) -- and nothing else for either token: the high colour
    // and token 6's clip are unchanged, so they are not re-sent (:227-239).
    // Token 5 fills an asymmetric 10 x 5 block (inclusive edges) that lies
    // wholly outside the old 64 x 48 frame, using the high colour it was given
    // before the change. Token 6 fills a big rect through its old 10 x 6 clip.
    Bytes after;
    for (const Bytes& m : {clip_to(5, 0, 0, 127, 79), fill_rect(5, 100, 60, 109, 64),
                           fill_rect_color(6, 0, 10, 40, 40, stamp),
                           fill_rect_color(6, 6, 0, 40, 3, stamp)})
        after.insert(after.end(), m.begin(), m.end());
    session.ingest(after);

    const Surface& s = session.surface();
    std::string where;
    check(every_pixel(s, [&](int x, int y) {
              if (x >= 100 && x <= 109 && y >= 60 && y <= 64)
                  return marker;
              if (x >= 6 && x <= 9 && y >= 0 && y <= 3)
                  return stamp;
              if (x >= 2 && x <= 4 && y >= 3 && y <= 4)
                  return old_paint;
              return black;
          }, where),
          "after the resize every pixel is where the server put it: " + where);
}

// --- fit-to-window settling ---------------------------------------------------

void test_resize_settler()
{
    const Clock::time_point t0 {};
    ResizeSettler settler;
    const DisplaySize current {1024, 700};
    check(!settler.deadline() && !settler.take(current, t0),
          "nothing is pending at first");

    // A drag: three steps 100 ms apart. Only the last size is asked for, and
    // only 250 ms after the last step.
    settler.window_resized({1030, 704}, t0);
    settler.window_resized({1100, 720}, t0 + 100ms);
    settler.window_resized({1180, 740}, t0 + 200ms);
    check(settler.deadline() == t0 + 450ms, "the size settles 250 ms after the last step");
    check(!settler.take(current, t0 + 449ms), "not a millisecond sooner");
    const auto settled = settler.take(current, t0 + 450ms);
    check(settled && *settled == DisplaySize {1180, 740},
          "then the last size is asked for: 1180 x 740");
    check(!settler.take(current, t0 + 2s) && !settler.deadline(), "once");

    // Back to the size already in force: nothing to ask for.
    settler.window_resized({1024, 700}, t0 + 3s);
    check(!settler.take(current, t0 + 4s), "the current size is not re-requested");

    // A minimised window reports 0 x 0, which is never a mode.
    settler.window_resized({0, 0}, t0 + 5s);
    check(!settler.deadline(), "a zero-size window is ignored");

    // Larger than any canvas this client holds: clamped.
    settler.window_resized({20000, 900}, t0 + 6s);
    const auto clamped = settler.take(current, t0 + 7s);
    check(clamped && *clamped == DisplaySize {16384, 900},
          "a 20000-px window asks for 16384");

    settler.window_resized({1300, 800}, t0 + 8s);
    settler.cancel();
    check(!settler.take(current, t0 + 9s), "cancel drops a pending size");
}

// --- the resolution row --------------------------------------------------------

void test_resolution_row()
{
    const Clock::time_point t0 {};
    DesktopShell shell("Lab box", desktop_w, desktop_h);
    shell.set_link(DesktopShell::Link::connected, "");
    shell.toggle();

    const auto presets = display_size_presets();
    check(presets.size() == 5 && presets[0] == DisplaySize {1024, 768}
              && presets[1] == DisplaySize {1280, 800}
              && presets[2] == DisplaySize {1366, 768}
              && presets[3] == DisplaySize {1600, 900}
              && presets[4] == DisplaySize {1920, 1080},
          "five common sizes are offered, smallest first");

    shell.pointer_press(button_x(4) + 10, 10, t0);
    check(shell.sizes_mode() && shell.take_action() == DesktopShell::Action::none,
          "Size opens the resolution row without changing anything");
    check(shell.regions().size() == 8 && shell.regions()[0].x == first_size,
          "eight slots, the first at x 170 of the bar");

    // The third preset slot (slot 4) is 1366 x 768. Its far edge, not just its
    // middle, selects it.
    shell.pointer_press(size_x(4) + 79, 32, t0);
    check(shell.take_action() == DesktopShell::Action::display_size,
          "a preset is a display-size action");
    auto choice = shell.take_display_size();
    check(choice && choice->kind == DisplaySizeChoice::Kind::preset
              && choice->size == DisplaySize {1366, 768},
          "slot 4 asks for 1366 x 768");
    check(!shell.take_display_size(), "the choice is read once");

    shell.pointer_press(size_x(0) + 10, 10, t0);
    choice = shell.take_display_size();
    check(shell.fit_to_window() && choice
              && choice->kind == DisplaySizeChoice::Kind::fit,
          "Fit switches fit-to-window on");
    shell.pointer_press(size_x(6) + 10, 10, t0);
    choice = shell.take_display_size();
    check(!shell.fit_to_window() && choice
              && choice->size == DisplaySize {1920, 1080},
          "choosing a fixed size (1920 x 1080) switches it off");
    shell.pointer_press(size_x(0) + 10, 10, t0);
    (void)shell.take_display_size();
    shell.pointer_press(size_x(1) + 10, 10, t0);
    choice = shell.take_display_size();
    check(!shell.fit_to_window() && choice
              && choice->kind == DisplaySizeChoice::Kind::window,
          "Window asks for the window's size, and switches fit off too");

    check(shell.take_action() == DesktopShell::Action::display_size,
          "the last choice's action is still waiting to be read");
    shell.pointer_press(size_x(7) + 10, 10, t0);
    check(!shell.sizes_mode() && !shell.keys_mode()
              && shell.take_action() == DesktopShell::Action::none,
          "Back returns to the main row");

    // The desktop changing size re-centres the bar and clamps it.
    shell.set_desktop_size(1600, 900);
    const IntRect wide = shell.bar_rect();
    check(wide.left == 370 && wide.right == 1229,
          "on a 1600-px desktop the 860-px bar spans x 370..1229");
    check(shell.desktop_size() == DisplaySize {1600, 900}, "the shell knows the size");
    shell.set_desktop_size(640, 480);
    const IntRect narrow = shell.bar_rect();
    check(narrow.left == 0 && narrow.right == 639 && shell.render_bar().width() == 640,
          "on a 640-px desktop the bar is 640 px wide");
    check(shell.regions().back().x + shell.regions().back().w == 640 - 8,
          "and its buttons are still right-aligned 8 px from its edge");
}

// --- reconnect countdown ----------------------------------------------------------

void test_reconnect_countdown()
{
    ReconnectConfig config;
    config.enabled = true;
    config.max_attempts = 9;
    config.base_backoff = 2500ms;
    config.max_backoff = 4000ms;
    const Clock::time_point t0 {};
    ReconnectSchedule s(config);
    check(reconnect_countdown(s, t0).empty(), "nothing pending, no countdown");
    s.connection_ended(ConnectionOutcome::transport_dropped, t0);
    check(reconnect_countdown(s, t0) == "attempt 1 of 9 in 3 s",
          "2.5 s away reads 3 s (rounded up)");
    check(reconnect_countdown(s, t0 + 500ms) == "attempt 1 of 9 in 2 s",
          "2.0 s away reads 2 s");
    check(reconnect_countdown(s, t0 + 2499ms) == "attempt 1 of 9 in 1 s",
          "1 ms away still reads 1 s, never 0");
    check(reconnect_countdown(s, t0 + 2500ms) == "attempt 1 of 9 now",
          "and at the deadline, now");
    s.attempt_started();
    s.connection_ended(ConnectionOutcome::connect_failed, t0 + 10s);
    check(reconnect_countdown(s, t0 + 10s) == "attempt 2 of 9 in 4 s",
          "the second attempt is capped at 4 s");
}

} // namespace

int main()
{
    test_shortcut_wire_bytes();
    test_repaint_lever();
    test_profile_reconnect_config();
    test_connect_error_outcomes();
    test_forwarded_endings();
    test_quic_connect_endings();
    test_route_status();
    test_reconnect_schedule();
    test_shell_geometry_and_reveal();
    test_shell_actions();
    test_shell_pixels();
    test_surface_resize();
    test_display_size_validation();
    test_display_mode_change_wire();
    test_reconnect_after_resize_asks_for_new_size();
    test_pixels_after_resize();
    test_resize_settler();
    test_resolution_row();
    test_reconnect_countdown();

    if (failures == 0) {
        std::cout << "PASS - " << checks << " desktop-shell checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " desktop-shell checks FAILED\n";
    return 1;
}
