#pragma once

// The connected desktop, independent of any window system.
//
// One Session and one DesktopShell for the life of a desktop, over however many
// connections it takes: the surface (the only copy of the remote screen) and
// the session identity carry across a reconnect, the transport and whatever
// route it rode on do not. Everything that decides *whether* to come back is
// ReconnectSchedule's (src/reconnect.cpp); everything about *how* a connection
// is built is the SessionLink's; everything a window system does -- painting,
// event translation, resizing a window -- is the front end's, reached through
// DesktopWindow. DesktopController is what is left: the state machine of a
// desktop, written once, so the SDL and X11 front ends behave the same because
// they run the same code rather than two copies of it.
//
// Also here, for the same reason: the library -> connect -> desktop flow both
// front ends run (run_profile_flow), and its CLI counterpart (run_endpoint).

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/connect_screen.hpp"
#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/desktop_shell.hpp"
#include "haiku_remote/launch_arguments.hpp"
#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/profile_library.hpp"
#include "haiku_remote/reconnect.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/transport.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace haiku_remote {

// What builds the desktop's connections. The current transport, if any, is
// owned here together with the substrate it needs (an SSH tunnel), and
// teardown() drops both -- always before begin_connect() builds the next, so a
// reconnect never has two forwards up at once.
//
// A connect runs on a worker thread, so the desktop window keeps painting and
// taking input (the toolbar's Disconnect and Reconnect, a window close) while
// ssh and the sockets block: begin_connect() starts it, connect_done() is
// polled from the event loop (the worker calls `wake` at every stage, so the
// loop never polls on a timer for it), and finish_connect() collects it.
// teardown() during an attempt cancels it and waits for it to tear down what it
// started; every wait in an attempt is bounded and woken by the cancel, so that
// is prompt.
class SessionLink {
public:
    virtual ~SessionLink() = default;
    [[nodiscard]] virtual Transport* transport() = 0;
    virtual void teardown() = 0;
    // Start building and connecting a fresh transport. A link whose
    // runs_handshake() is true also runs the RP handshake on `session`
    // (reset() first); the caller must not touch `session` until
    // connect_done() is true.
    virtual void begin_connect(Session& session, std::function<void()> wake) = 0;
    [[nodiscard]] virtual bool connect_done() = 0;
    // A short label for the stage the attempt is in, for the toolbar.
    [[nodiscard]] virtual std::string connect_stage() = 0;
    // Once connect_done(): true on success. On failure `outcome` is the
    // reconnect policy's reading of it and `error` the user-facing reason.
    virtual bool finish_connect(ConnectionOutcome& outcome, std::string& error) = 0;
    // The connect leaves `session` handshaken (connect_flow's handshake-gated
    // "connected"), so the desktop must not start() it again.
    [[nodiscard]] virtual bool runs_handshake() const { return false; }
    // The Session the first connection's handshake already ran on, if any:
    // the desktop continues on it rather than handshaking twice.
    [[nodiscard]] virtual std::unique_ptr<Session> take_session() { return nullptr; }
    // The connection connect() built (or the one handed over at construction)
    // carried a session: the server sent something back.
    virtual void established() {}
    [[nodiscard]] virtual std::string route() const { return {}; }
    // The transport is the local end of an SSH -L forward (see
    // ConnectionResult::through_forward).
    [[nodiscard]] virtual bool through_forward() const { return false; }
};

// The command-line path: the transport options from argv, connected again on a
// manual Reconnect exactly the way the first connection was made.
class OptionsLink final : public SessionLink {
public:
    OptionsLink(TransportOptions options, std::unique_ptr<Transport> connected);
    ~OptionsLink() override;

    Transport* transport() override { return transport_.get(); }
    void teardown() override;
    void begin_connect(Session&, std::function<void()> wake) override;
    bool connect_done() override;
    std::string connect_stage() override;
    bool finish_connect(ConnectionOutcome& outcome, std::string& error) override;
    std::string route() const override;

private:
    void cancel_and_join();

    TransportOptions options_;
    std::unique_ptr<Transport> transport_;
    std::mutex mutex_;
    std::unique_ptr<Transport> pending_; // the attempt's; guarded by mutex_
    bool done_ = true;
    bool ok_ = false;
    ConnectionOutcome outcome_ = ConnectionOutcome::connect_failed;
    std::string error_;
    std::string stage_;
    std::thread worker_;
};

// The library path: a profile, connected through plan_launch and
// connect_with_progress -- the same managed route walk the progress screen ran
// -- with the result (transport plus ManagedConnection, which owns the ssh
// child) held here. A first successful exchange stamps the profile's
// last-connected time, whether or not the session later drops.
class ProfileLink final : public SessionLink {
public:
    ProfileLink(ConnectionProfile profile, ConnectionLibrary& library,
                ConnectResult connected);
    ~ProfileLink() override;

    Transport* transport() override { return result_.transport.get(); }
    void teardown() override;
    void begin_connect(Session& session, std::function<void()> wake) override;
    bool connect_done() override;
    std::string connect_stage() override;
    bool finish_connect(ConnectionOutcome& outcome, std::string& error) override;
    void established() override;
    bool runs_handshake() const override { return true; }
    std::unique_ptr<Session> take_session() override;
    std::string route() const override;
    bool through_forward() const override;

private:
    void mark_stderr();

    ConnectionProfile profile_;
    ConnectionLibrary& library_;
    ConnectResult result_;
    std::optional<AsyncConnect> attempt_;
    std::size_t stderr_mark_ = 0;
};

// How a desktop ended, for the caller's exit status and the library card.
struct DesktopEnd {
    int status = exit_status::ok;
    // The user closed the window or pressed Disconnect.
    bool user_ended = false;
    // The last connection closed in an orderly way without sending anything.
    bool refused_no_data = false;
    // The last connection's ending, and why the desktop gave up on it.
    ConnectionOutcome outcome = ConnectionOutcome::completed;
    std::string reason;
};

// Describe how a session ended and say whether that ending was a success:
// true for an orderly end with drawing received (exit 0), false for a failure.
// A session the server refused before sending a single byte of drawing gets its
// own message, because it presents as "the desktop is black" while the process
// would otherwise report success.
bool report_session_end(bool orderly, std::size_t messages, std::string_view reason);

// "12.3 KiB" / "45.6 MiB".
[[nodiscard]] std::string format_bytes(std::uint64_t bytes);

// What the desktop needs from the window that shows it.
class DesktopWindow {
public:
    virtual ~DesktopWindow() = default;
    // The desktop is now `size` pixels: map presentation and input onto it.
    virtual void set_desktop_size(DisplaySize size) = 0;
    // The window's drawable size in device pixels: what "Window" and
    // fit-to-window ask the remote for, so the desktop is shown 1:1.
    [[nodiscard]] virtual DisplaySize drawable_size() = 0;
    // A fixed size was chosen: bring the window to it, so it is shown 1:1,
    // when it fits on the display and the window is not full screen.
    virtual void fit_window_to(DisplaySize size) = 0;
    virtual void toggle_fullscreen() = 0;
    // A reconnect has handed the session back: a front end with per-session
    // state of its own (the test script's pixel wait) picks it up here.
    virtual void reconnected() {}
};

// The desktop's state machine. The front end owns the window and the event
// loop, and calls in at fixed points of every iteration:
//
//     controller.start();
//     while (controller.running()) {
//         if (controller.advance(now)) continue;        // reconnects
//         if (controller.live()) {
//             if (controller.receive(2)) continue;      // the link ended
//         } else {
//             wait for window events, at most controller.idle_wait_ms(now)
//         }
//         translate window events: shell().pointer_*, send(), set_modifiers()
//         controller.after_events(now);                  // the toolbar's actions
//         if (controller.needs_present()) present, then presented()
//     }
//     return controller.finish();
class DesktopController {
public:
    using Clock = std::chrono::steady_clock;

    // `prestarted` is the Session the connect's handshake already ran on, or
    // null for a fresh one of width x height. `wake` is thread-safe and ends
    // the front end's event wait (a reconnect worker calls it at every stage).
    DesktopController(SessionLink& link, std::string name, int width, int height,
                      ReconnectConfig reconnect, DesktopWindow& window,
                      std::unique_ptr<Session> prestarted, std::function<void()> wake);
    ~DesktopController();
    DesktopController(const DesktopController&) = delete;
    DesktopController& operator=(const DesktopController&) = delete;

    [[nodiscard]] Session& session() { return *session_; }
    [[nodiscard]] DesktopShell& shell() { return shell_; }

    // Start the first connection's handshake. Throws std::runtime_error when
    // it cannot be sent: there is no desktop to show yet.
    void start();
    [[nodiscard]] bool running() const { return running_; }
    // A transport is connected and started.
    [[nodiscard]] bool live() const { return live_; }
    // A reconnect attempt owns the session (it resets it and ingests the
    // handshake on its worker): the front end must not read the surface.
    [[nodiscard]] bool connecting() const { return connecting_; }

    // Collect a finished reconnect or start a due one. True when it did
    // either; the front end then goes round its loop again.
    bool advance(Clock::time_point now);
    // While live: receive for up to `timeout_ms` and ingest. True when the
    // connection ended (classified, and a reconnect scheduled or the desktop
    // ended); the front end then goes round its loop again.
    bool receive(int timeout_ms);
    // While not live: refresh the bar's status and return how long the front
    // end may block on window events -- capped at a second so the countdown
    // stays current, sooner for a settling resize or a test-script step.
    [[nodiscard]] int idle_wait_ms(Clock::time_point now);

    // Input for the remote. Dropped while no connection is live.
    void send(const std::vector<std::uint8_t>& message);
    // The modifiers held locally now; RP_MODIFIERS_CHANGED when they changed.
    void set_modifiers(std::uint32_t modifiers);
    // The window lost the keyboard: nothing is held any more.
    void focus_lost();
    // The window's drawable size changed (fit-to-window follows it).
    void window_resized(Clock::time_point now);
    // The user closed the window, or the application is quitting.
    void user_quit();
    // The window must be repainted (exposed, resized).
    void mark_dirty() { dirty_ = true; }

    // After the window events: act on the toolbar, settle a resize, run the
    // hide timer and refresh the statistics panel.
    void after_events(Clock::time_point now);

    // True when the frame on screen is stale: the session drew, the bar or the
    // statistics panel changed, or either was shown or hidden.
    [[nodiscard]] bool needs_present() const;
    // True when the session surface changed since the front end last took it
    // (and it may be read: no reconnect owns it).
    [[nodiscard]] bool surface_changed() const { return dirty_ && !connecting_; }
    // The front end has presented the current state: the surface (if
    // surface_changed()), the bar and the panel.
    void presented();

    // Tear the link down and say how the desktop ended.
    DesktopEnd finish();

private:
    void connection_ended(ConnectionOutcome outcome, std::string reason);
    void start_connection(bool first);
    void observe_end(bool receive_failed);
    void attempt_reconnect();
    void collect_reconnect();
    [[nodiscard]] std::string attempt_label() const;
    bool apply_display_size(DisplaySize size);
    void update_secure_attention();
    void refresh_stats(Clock::time_point now);

    SessionLink& link_;
    ReconnectConfig reconnect_;
    DesktopWindow& window_;
    std::function<void()> wake_;
    // Before session_: it is initialised from `prestarted`, which session_'s
    // initialiser then moves from.
    bool first_handshaken_ = false;
    std::unique_ptr<Session> session_;
    DesktopShell shell_;
    ReconnectSchedule schedule_;
    ResizeSettler settler_;
    DesktopEnd end_;
    std::string socket_error_;

    bool live_ = false;        // a transport is connected and started
    bool connecting_ = false;  // a reconnect attempt is running on a worker
    bool established_ = false; // this connection has carried a session
    bool running_ = true;
    bool dirty_ = true;
    bool shell_was_visible_ = false;
    bool stats_were_visible_ = false;
    int reconnects_ = 0;
    std::uint32_t last_modifiers_ = 0;
    std::uint64_t bytes_received_ = 0;
    std::size_t frames_ = 0;
    Clock::time_point stats_since_;
    double frame_rate_ = 0;
    // A display size chosen while a reconnect owned the session; applied once
    // it hands the session back.
    std::optional<DisplaySize> deferred_size_;
    std::vector<std::uint8_t> buffer_;
};

// The result of a front end's connection-progress window: the connected
// transport (plus the managed substrate it needs kept alive, and the Session
// the handshake ran on) on success, the user's choice on the error screen, or
// a cancel.
struct ConnectOutcome {
    ConnectResult result;
    ConnectScreen::Action action = ConnectScreen::Action::none; // only on failure
    bool cancelled = false; // Cancel, Escape or closing the window mid-connect
};

// The size a desktop opens at and whether the command line fixed it.
struct DesktopSize {
    int width = 1280;
    int height = 800;
    bool explicit_size = false;
};

// The three windows of the app, as a front end draws them.
class FrontEnd {
public:
    virtual ~FrontEnd() = default;
    // The connection library; nullopt when it was closed without a pick.
    virtual std::optional<ConnectionProfile> run_library(ConnectionLibrary& library) = 0;
    // The connection-progress window for `profile`.
    virtual ConnectOutcome run_connect(const ConnectionProfile& profile) = 0;
    // A connected desktop over `link`, whose transport is already connected.
    virtual DesktopEnd run_desktop(DesktopSize size, SessionLink& link,
                                   const std::string& name,
                                   const ReconnectConfig& reconnect) = 0;
    // The application was asked to quit (SIGTERM, SIGINT, the session
    // manager): never show the library again.
    [[nodiscard]] virtual bool quit_requested() const { return false; }
};

// The CLI endpoint path: resolve a transport from the options, connect it
// inline (reporting a connect failure with the same exit code as ever), then
// run the desktop. Automatic reconnect is off here -- the command line has no
// flag for it -- but the toolbar's Reconnect rebuilds from the same options.
int run_endpoint(FrontEnd& front_end, const TransportOptions& transport,
                 DesktopSize size);

// The library path: the library (or, for `target` saved_profile/ssh_profile,
// that one profile at once), then its progress window and its desktop, then
// the library again until it is closed. Returns the exit status.
int run_profile_flow(FrontEnd& front_end, LaunchArguments& launch,
                     LaunchTarget target, DesktopSize size);

} // namespace haiku_remote
