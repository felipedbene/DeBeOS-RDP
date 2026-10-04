#pragma once

// The connection coordinator: a UI-free worker-thread state machine that turns
// a ConnectionProfile into a live remote-desktop Session. It lives in
// haiku_remote_core (which has no SDL on its link line) so every front end --
// SDL, X11, headless -- drives a connection the same way, and the network/SSH
// lifecycle is written once rather than per front end.
//
// Threading contract (load-bearing): Session, Surface and the Transport are NOT
// thread-safe, so this type confines ALL of them to one worker thread. A UI
// thread only ever touches the mutex-guarded Snapshot (connect progress, the
// current state, an actionable reason code and a bounded diagnostic log) and a
// handed-out COPY of the framebuffer via copy_frame(). Reading the session's own
// surface from the UI thread is exactly the reconnect/black-screen/tearing class
// this project already paid for; copy_frame() exists so the UI never has to.
//
// Input flows the other way through post_input(): the UI encodes an event and
// hands the coordinator the bytes, the worker drains the queue and sends them
// through the Session. The UI never calls Session::send_client_message directly.
//
// The orphan-reaper posture is inherited from the SSH tunnel it owns: a verified
// orphan is reaped before every connect (SshTunnel::start), and a front end
// should also reap at launch (reap_orphans on the pidfile dir). A still-running
// ssh is killed only when positively verified as ours; an unverifiable pid is
// never signalled. See ssh_tunnel.hpp.

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/reconnect.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/ssh_tunnel.hpp"
#include "haiku_remote/surface.hpp"
#include "haiku_remote/transport.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace haiku_remote {

// The connection lifecycle, exactly the states issue #1 names. The tunnel states
// are skipped in direct mode; reconnecting/disconnecting/failed are off the main
// path.
enum class CoordinatorState {
    idle,
    starting_tunnel,
    waiting_for_forward,
    connecting_transport,
    waiting_for_protocol_handshake,
    requesting_display,
    connected,
    reconnecting,
    disconnecting,
    failed,
};

[[nodiscard]] std::string_view coordinator_state_name(CoordinatorState state);

// Why a connection ended in `failed`, reduced to the actionable set issue #1
// lists. Each maps to a stable kebab-case code (reason_code) a UI can key help
// text on and a log can carry verbatim.
enum class ConnectionReason {
    none,
    ssh_not_found,      // the ssh executable could not be launched
    identity_missing,   // the configured identity file is absent/unreadable
    auth_failed,        // ssh authentication was refused
    host_key,           // host-key verification failed (a changed key, TOFU)
    local_port_in_use,  // the local forward port was already bound
    forward_timeout,    // the forward did not open before the timeout
    tcp_failed,         // the TCP connection itself failed
    no_rp_init_ack,     // connected, but no protocol message arrived in time
    decode_failed,      // bytes arrived but no message could be decoded
    remote_closed,      // the server closed/reset the connection
};

// The stable wire/log string for a reason, e.g. "host-key". "none" for none.
[[nodiscard]] std::string_view reason_code(ConnectionReason reason);

// Classify an SSH bring-up failure from the combined launch error and captured
// ssh stderr. PURE and testable: a drifted ssh message is the kind of bug that
// only shows against a real server, so the patterns are pinned by unit tests.
// `forward_deadline_hit` distinguishes a clean readiness timeout (nothing in
// stderr) from an early ssh exit, so a silent timeout maps to forward_timeout
// rather than a blank tcp_failed.
[[nodiscard]] ConnectionReason classify_ssh_failure(std::string_view text,
                                                    bool forward_deadline_hit);

// A copyable, mutex-guarded view of the coordinator for the UI thread. Carries
// no Session, Surface or Transport reference -- only values. surface_generation
// bumps every time the worker refreshes the framebuffer, so a UI re-copies via
// copy_frame() only when it changes.
struct Snapshot {
    CoordinatorState state = CoordinatorState::idle;
    std::chrono::milliseconds elapsed{0};
    ConnectionReason reason = ConnectionReason::none;
    std::string message;             // human-readable detail (never a secret)
    std::vector<std::string> diag;   // bounded ring, oldest first
    std::uint64_t surface_generation = 0;
    int reconnect_attempt = 0;       // how many reconnects have been made
    bool finished = false;           // the worker has ended
};

// Everything the coordinator needs that is not injected machinery.
struct CoordinatorConfig {
    ConnectionProfile profile;
    // Direct-mode session cookie (app_server's per-boot cookie). Ignored in SSH
    // mode, where the cookie is fetched over the same ssh identity. Never logged.
    std::string cookie;
    // Reconnect policy. enabled is independent of profile.auto_reconnect so a
    // caller stays in control; launch_options wires one to the other.
    ReconnectConfig reconnect;
    // Bound on waiting_for_protocol_handshake: with no decoded message by this,
    // the connection fails no_rp_init_ack (or decode_failed if bytes arrived).
    std::chrono::milliseconds handshake_timeout{8000};
    // Passed straight to the owned SshTunnel.
    TunnelOptions tunnel_options{};
    // Bounded diagnostic ring buffer capacity.
    std::size_t diag_capacity = 100;
    // When set, the transport is built from these options verbatim (and the
    // tunnel is skipped), rather than from host/port/cookie. This is how a
    // ws:///wss:// broker URL -- which carries its own host, token and pinning --
    // reaches the transport unchanged; the normal direct/ssh path leaves it
    // unset and the coordinator assembles the options itself.
    std::optional<TransportOptions> transport_override;
};

// Build an unconnected Transport for the given options. Defaults to
// make_transport; a test injects a fake. error is set on failure (nullptr).
using TransportFactory =
    std::function<std::unique_ptr<Transport>(const TransportOptions&, std::string&)>;

// Build an SSH tunnel for the given config/options. Defaults to a real
// SshTunnel; a test injects a fake ISshTunnel.
using TunnelFactory =
    std::function<std::unique_ptr<ISshTunnel>(const TunnelConfig&, const TunnelOptions&)>;

class ConnectionCoordinator {
public:
    explicit ConnectionCoordinator(CoordinatorConfig config,
                                   TransportFactory transport_factory = {},
                                   TunnelFactory tunnel_factory = {});
    ~ConnectionCoordinator();

    ConnectionCoordinator(const ConnectionCoordinator&) = delete;
    ConnectionCoordinator& operator=(const ConnectionCoordinator&) = delete;

    // Launch the worker thread and run the connect sequence (with reconnect).
    // Idempotent: a second call while a worker is live is a no-op.
    void connect();

    // Ask the worker to tear the connection down and end. Non-blocking; the
    // destructor joins. Safe to call before connect() or after it finished.
    void disconnect();

    // Queue a request to replay the whole display (RP_RESYNC / full repaint),
    // serviced on the worker thread. Dropped if not connected.
    void request_full_repaint();

    // Queue an already-encoded client message (an input event). The worker
    // drains the queue and sends each through the Session, so Session stays
    // single-threaded. Bytes are moved in; dropped silently if not connected.
    void post_input(std::vector<std::uint8_t> message);

    // A consistent, copyable view for the UI thread.
    [[nodiscard]] Snapshot snapshot() const;

    // Copy the latest framebuffer into `out` and return the generation copied.
    // `out` must have the coordinator's width()/height(); a mismatch copies
    // nothing and returns 0. Returns 0 before the first frame.
    [[nodiscard]] std::uint64_t copy_frame(Surface& out) const;

    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }

    // True once the worker has run to completion (success, failure, or
    // disconnect). A UI polls this to know it may return to the library.
    [[nodiscard]] bool finished() const { return finished_.load(); }

private:
    // One connect-and-receive attempt. Fills `result` for the reconnect policy
    // and, on failure, sets the reason/message under the lock. Returns when the
    // connection ends (drop, server close, handshake failure, or stop).
    void run();
    ConnectionResult run_one_connection();

    // SSH bring-up for one attempt. Returns the tunnel's local port on success,
    // or nullopt having set the failure reason. Direct mode returns the profile
    // port without touching a tunnel.
    std::optional<std::uint16_t> bring_up_endpoint(std::string& cookie_out);

    // State/diagnostic plumbing, all under lock_.
    void set_state(CoordinatorState state);
    void note(std::string line);
    void set_failure(ConnectionReason reason, std::string message);
    void refresh_frame();            // copy session surface -> frame_ (worker)
    [[nodiscard]] bool stop_requested() const { return stop_.load(); }
    // Interruptible sleep used for reconnect backoff. False if a stop was
    // requested during the wait.
    [[nodiscard]] bool wait_for(std::chrono::milliseconds delay);

    CoordinatorConfig config_;
    TransportFactory transport_factory_;
    TunnelFactory tunnel_factory_;
    const int width_;
    const int height_;

    // Worker-thread-only state (no lock needed; only run() and its callees).
    std::unique_ptr<ISshTunnel> tunnel_;
    std::unique_ptr<Transport> transport_;
    std::unique_ptr<Session> session_;
    // Scratch for the Session's send callback (worker thread only). A member so
    // the callback does not capture a reference into a run_one_connection local.
    std::string send_error_;

    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> finished_{false};

    // Shared state, guarded by lock_.
    mutable std::mutex lock_;
    std::condition_variable wake_;   // wakes the backoff/idle wait on stop
    CoordinatorState state_ = CoordinatorState::idle;
    ConnectionReason reason_ = ConnectionReason::none;
    std::string message_;
    std::deque<std::string> diag_;
    std::uint64_t surface_generation_ = 0;
    int reconnect_attempt_ = 0;
    std::chrono::steady_clock::time_point start_time_{};
    std::optional<std::chrono::steady_clock::time_point> end_time_;
    Surface frame_;                  // the UI's copy source
    bool have_frame_ = false;
    std::vector<std::vector<std::uint8_t>> input_queue_;
    bool repaint_requested_ = false;
};

} // namespace haiku_remote
