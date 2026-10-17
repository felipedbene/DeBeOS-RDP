#pragma once

// DeBeOS-RDP issue #1 Part 3: connection progress + actionable error states.
//
// Parts 1 and 2 gave the client a connection library and a managed-transport
// substrate (plan_launch -> an ordered RouteStep list; open_connection ->
// a ManagedConnection owning the SSH tunnel or the broker credentials). What
// was still missing is what the *user* sees between pressing Connect and the
// desktop appearing: today a slow or failed connect is a silent black window.
//
// This header is the windowing-free half of the fix:
//
//   - ConnectFlow is a state machine over the plan's RouteStep sequence. It
//     turns the step/phase events open_connection reports (ConnectObserver),
//     the transport connect and the RP handshake into an ordered list of
//     ProgressStage entries a renderer can draw, and it knows when the whole
//     attempt has connected, failed, or been cancelled. It owns no Surface and
//     no socket, so it is exhaustively unit testable.
//
//   - classify_connect_failure() is the cause -> message/affordance table. It
//     maps what the layers surface -- the fail point, the ConnectFailure flag,
//     the error string, and ssh's own stderr -- onto an actionable
//     ConnectError. ssh says *why* it failed only on stderr ("Permission
//     denied (publickey).", "Host key verification failed."); its exit status
//     is 255 for all of them. So the stderr is captured and classified, and
//     only when it names nothing recognisable does the error fall back to an
//     honest "SSH failed" that shows what ssh said.
//
//   - connect_with_progress() drives the real thing: open_connection (with an
//     observer feeding the flow) + make_transport + Transport::connect() + the
//     RP handshake, calling an on_progress hook at each stage boundary. The
//     attempt is "connected" only once app_server has acknowledged
//     RP_INIT_CONNECTION and the display request is on the wire.
//
//   - AsyncConnect runs connect_with_progress on a worker thread, so a front
//     end keeps painting (elapsed time, a working Cancel) while ssh and the
//     sockets block. Cancel kills the attempt's ssh child and shuts its socket
//     down; the worker has reaped everything before finished() turns true.
//
// The protocol, renderer, session and transport cores know nothing about any of
// this. The dependency arrow points one way: frontend -> connect screen ->
// connect flow -> managed_transport. Never back.

#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/transport.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace haiku_remote {

// One line of the progress view. The stages are built up-front from the plan,
// one run per RouteStep, in plan order (so step_index lines up with
// plan.steps[] and a broker->tunnel fallback is visible as the broker's run
// failing and the tunnel's lighting up). Which stages a route contributes is
// route_stages(): a broker step is
//     contacting_broker, authenticating, waiting_for_protocol_handshake,
//     requesting_display
// a tunnel step is
//     fetching_cookie (unless the profile supplied the cookie), starting_tunnel,
//     waiting_for_forward, connecting_transport,
//     waiting_for_protocol_handshake, requesting_display
// and a direct step -- no ssh at all -- is
//     connecting_transport, waiting_for_protocol_handshake, requesting_display.
// The names after the substrate stages are issue #1's connecting-state names.
enum class StageKind {
    contacting_broker,              // broker: fetch token + certificate over SSH.
    authenticating,                 // broker: TCP + TLS + WebSocket upgrade + RP_AUTHENTICATE.
    fetching_cookie,                // tunnel: read app_server's session cookie over SSH.
    starting_tunnel,                // tunnel: spawn ssh -L.
    waiting_for_forward,            // tunnel: wait for the local end of the forward.
    connecting_transport,           // tunnel: TCP connect + the session-cookie frame.
    waiting_for_protocol_handshake, // RP_INIT_CONNECTION + RP_HELLO sent; awaiting the echo.
    requesting_display,             // RP_GET_SYSTEM_PALETTE + RP_UPDATE_DISPLAY_MODE going out.
};

[[nodiscard]] const char* stage_kind_name(StageKind kind);

// The stage run one plan step contributes, in order.
[[nodiscard]] std::vector<StageKind> route_stages(const RouteStep& step);

enum class StageState {
    pending, // not reached yet.
    active,  // running now.
    done,    // completed successfully.
    failed,  // this stage is where the attempt failed.
    skipped, // not run because an earlier stage of the same step failed.
};

struct ProgressStage {
    StageKind kind;
    RouteKind route;
    std::size_t step_index; // which plan.steps[] entry this stage belongs to.
    StageState state = StageState::pending;
    std::string label;
    // The route this stage belongs to, as the progress screen names it:
    // "Broker over QUIC", "Broker over WSS" or "SSH tunnel".
    std::string route_name;
};

// The attempt's lifecycle. Issue #1's state list mixes lifecycle and step:
// its starting_tunnel .. requesting_display are FlowState::connecting with
// that StageKind active (issue_state_name() gives the combined name).
enum class FlowState {
    idle,          // not started, or a cancelled attempt has finished tearing down.
    connecting,    // working through the stages; active_stage() says which.
    connected,     // the RP handshake completed; the session can start.
    reconnecting,  // owned by the session shell's reconnect; never set here.
    failed,        // every route was exhausted, or a terminal failure; error() says why.
    disconnecting, // cancelled: the ssh child / socket are being torn down.
};

[[nodiscard]] const char* flow_state_name(FlowState state);

// What actually went wrong, in terms the user can act on. The cause is the
// machine-readable key; title/detail/remedy are the human text; can_retry says
// whether offering Retry makes sense (a pin mismatch, for instance, will not fix
// itself on a second try). `raw` always keeps the underlying message so nothing
// is lost, and `ssh_stderr` what ssh said, when it said anything.
enum class ConnectErrorCause {
    none,
    missing_credential,    // no cookie/token available (ConnectFailure::missing_credential).
    ssh_not_found,         // no ssh executable to run.
    identity_missing,      // the profile's identity file does not exist.
    identity_unreadable,   // it exists but cannot be read, or ssh refused to load it.
    ssh_auth_failed,       // ssh: "Permission denied (...)".
    ssh_host_key,          // ssh: host key changed / unknown under strict checking.
    host_not_found,        // the host name does not resolve.
    ssh_refused,           // nothing accepts SSH connections on the host's SSH port.
    host_unreachable,      // the SSH connect timed out / no route (a black-hole host).
    ssh_disconnected,      // the SSH server closed the connection before it was up.
    ssh_failed,            // ssh failed and its stderr named no cause we recognise.
    local_port_in_use,     // the local end of the forward could not be bound.
    forward_timeout,       // ssh stayed up but the forward never accepted.
    broker_unavailable,    // reached the host, but remote_broker returned no token/cert.
    broker_token_denied,   // RP_AUTH_RESULT status 1.
    broker_cert_pin,       // the broker's certificate did not match the pin.
    broker_cert_untrusted, // TLS chain / host-name verification failed.
    broker_cert_unknown,   // TOFU: no known_brokers entry, and it was not accepted.
    broker_cert_changed,   // TOFU: differs from the known_brokers entry; refused.
    cookie_unavailable,    // the session cookie could not be read on the host.
    server_listener,       // app_server's remote interface is not reachable/listening.
    tcp_failed,            // the TCP connect to the session/broker port failed.
    handshake_timeout,     // connected, but no RP_INIT_CONNECTION echo in time.
    decode_failed,         // the server's stream could not be decoded.
    remote_closed,         // the server closed the connection during the handshake.
    session_refused,       // the server reset/closed it before any drawing (stale cookie).
    quic_udp_blocked,      // quic://: no UDP answer at all (blocked, or no -q broker).
    quic_alpn_mismatch,    // quic://: the broker refused ALPN debeos-urp/1.
    quic_handshake_timeout, // quic://: answered over UDP, handshake did not finish.
    cancelled,             // the user cancelled.
    other,                 // anything else; shown verbatim.
};

struct ConnectError {
    ConnectErrorCause cause = ConnectErrorCause::none;
    std::string title;
    std::string detail;
    std::string remedy;
    bool can_retry = true;
    std::string raw;
    std::string ssh_stderr;

    // A single line suitable for the library card's last_error.
    [[nodiscard]] std::string summary() const;
};

// Where in the attempt a failure happened. This, plus the ConnectFailure flag,
// the raw message and ssh's stderr, is everything the classifier is given --
// deliberately, so it cannot claim a distinction the layers did not make.
// A new route kind (QUIC) adds its own transport_* point here, and its causes
// (UDP blocked, ALPN mismatch) as ConnectErrorCause values, without touching
// the others.
enum class ConnectFailPoint {
    preflight,        // before anything was spawned: ssh missing, identity file.
    substrate_broker, // open_connection, broker route (SSH fetch of token/cert).
    substrate_tunnel, // open_connection, tunnel route (cookie fetch / ssh -L).
    transport_broker, // WebSocketTransport::connect() (TLS, upgrade, auth).
    transport_tunnel, // TcpTransport::connect() (socket, cookie frame).
    transport_direct, // TcpTransport::connect() straight to host:remotePort.
    handshake,        // after connect, before the RP_INIT_CONNECTION echo; see
                      // classify_handshake_failure().
    session_refused,  // after a successful connect: 0 messages, orderly close.
};

[[nodiscard]] ConnectError classify_connect_failure(
    ConnectFailPoint where, ConnectFailure transport_failure,
    const std::string& message, const std::string& ssh_stderr = {});

// The cause ssh's stderr names, or ConnectErrorCause::none when it names
// nothing this recognises. Keyed on OpenSSH's own wording (captured from
// OpenSSH 8.7 for the tests), checked in the order ssh meets the failures:
// the earliest one stopped it. A missing identity file makes ssh warn and then
// fail authentication, and the missing file is the cause worth reporting.
[[nodiscard]] ConnectErrorCause classify_ssh_stderr(const std::string& ssh_stderr);

// How the RP handshake failed, as connect_with_progress observed it.
enum class HandshakeFailure {
    timeout,        // nothing acknowledged RP_INIT_CONNECTION before the deadline.
    decode,         // the server's bytes could not be framed/decoded.
    reset,          // the connection was reset (app_server's refusal signature).
    closed,         // an orderly close (FIN, or RP_CLOSE_CONNECTION).
    send_failed,    // writing the handshake failed.
    receive_failed, // any other receive error.
};

[[nodiscard]] ConnectError classify_handshake_failure(
    HandshakeFailure failure, RouteKind route, const std::string& message,
    const std::string& ssh_stderr = {});

// A bounded, thread-safe diagnostic log: the last `capacity` lines of stages,
// timings, errors and ssh's stderr, each stamped with the time since the log
// was created. Shown on the failure screen and copyable to the clipboard, so it
// must never hold a secret: add_secret() registers a token or cookie, and every
// occurrence -- in lines already logged and in every later one -- is replaced
// with "[redacted]". Nothing writes the environment or an argv's values here.
class DiagnosticLog {
public:
    explicit DiagnosticLog(std::size_t capacity = 200);

    void add(const std::string& line);
    void add_secret(const std::string& secret);

    [[nodiscard]] std::vector<std::string> lines() const;
    // Every retained line joined with '\n', preceded by a note of how many
    // older lines the ring dropped (if any).
    [[nodiscard]] std::string text() const;
    [[nodiscard]] std::size_t dropped() const;
    [[nodiscard]] std::size_t capacity() const { return capacity_; }

    // Longest line kept; a longer one is truncated with a marker.
    static constexpr std::size_t max_line_length = 400;
    // Secrets shorter than this are not registered: redacting a two-character
    // string would shred the whole log and protect nothing.
    static constexpr std::size_t min_secret_length = 6;

private:
    [[nodiscard]] std::string redact(std::string line) const;

    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::deque<std::string> lines_;
    std::vector<std::string> secrets_;
    std::size_t dropped_ = 0;
    std::chrono::steady_clock::time_point start_;
};

// The pure progress state machine. Build it from the plan, then feed it the
// same events open_connection's observer reports (plus the transport-connect
// and handshake stages and the terminal connected()/fail()). It is a plain
// value type -- no threads, no I/O -- so a test can drive the whole lifecycle
// by hand, and a front end can hold a copy (a snapshot) of a flow that a worker
// thread is advancing.
class ConnectFlow {
public:
    explicit ConnectFlow(const LaunchPlan& plan);

    void begin_step(std::size_t index, RouteKind kind);
    void phase(std::size_t index, ConnectPhase phase);
    void step_failed(std::size_t index, RouteKind kind, const std::string& error);
    // The substrate for `index` is up; the transport-connect stage is next.
    void begin_transport(std::size_t index, RouteKind kind);
    // The transport is connected; the RP handshake has been sent.
    void begin_handshake(std::size_t index);
    // app_server echoed RP_INIT_CONNECTION; the display request is going out.
    void request_display(std::size_t index);
    // Make `kind` of step `index` the active stage: earlier stages of the step
    // are done. The general form of the four calls above.
    void enter(std::size_t index, StageKind kind);
    void connected();
    void fail(ConnectError error);
    // Cancel: the attempt is tearing down; then it is idle again.
    void begin_disconnect();
    void finish_disconnect();

    [[nodiscard]] FlowState state() const { return state_; }
    [[nodiscard]] const std::vector<ProgressStage>& stages() const { return stages_; }
    // The index into stages() of the stage running now, if any.
    [[nodiscard]] std::optional<std::size_t> active_stage() const;
    // True once a broker step failed and a later tunnel step began -- the
    // visible broker->tunnel fallback of direct/auto mode.
    [[nodiscard]] bool fell_back() const { return fell_back_; }
    [[nodiscard]] const ConnectError& error() const { return error_; }
    [[nodiscard]] const std::string& plan_note() const { return plan_note_; }

private:
    [[nodiscard]] std::optional<std::size_t>
    find_stage(std::size_t step_index, StageKind kind) const;

    std::vector<ProgressStage> stages_;
    FlowState state_ = FlowState::idle;
    bool fell_back_ = false;
    ConnectError error_;
    std::string plan_note_;
};

// Issue #1's name for where `flow` is: "idle", "starting_tunnel", ...,
// "connected", "failed", "disconnecting". While connecting it is the active
// stage's name.
[[nodiscard]] std::string issue_state_name(const ConnectFlow& flow);

// The result of a whole connect attempt. On success `transport` is connected,
// the RP handshake is done, and `connection` owns the SSH tunnel / broker
// certificate -- keep the result alive for as long as the session runs,
// exactly as a ManagedConnection was kept alive before. `session` is the
// Session the handshake ran on (null when ConnectOptions::session supplied
// one): it has already ingested the server's first messages, so it must be the
// one the session loop continues with. Its sender writes to `transport`; rebind
// it with set_sender() if you want the error text.
struct ConnectResult {
    bool ok = false;
    bool cancelled = false;
    std::unique_ptr<Transport> transport;
    ManagedConnection connection;
    std::unique_ptr<Session> session;
    ConnectError error;
    std::string route_note;
};

struct ConnectOptions {
    // For tests: a fake command runner stands in for ssh. Null = the real one.
    CommandRunner* runner = nullptr;
    // How a frontend asks the user about a broker certificate that is unknown
    // or changed (trust on first use, known_brokers). Called on the connecting
    // thread while the flow sits in "authenticating"; null means nobody can be
    // asked, and an unknown or changed certificate fails the attempt.
    TrustPrompt ask_trust;
    // When set, cancel() stops the attempt: ssh children are killed and
    // reaped, sockets shut down, and the result comes back with cancelled.
    CancelToken* cancel = nullptr;
    DiagnosticLog* log = nullptr;
    // Session size for the handshake (RP_HELLO / RP_UPDATE_DISPLAY_MODE);
    // 0 means the profile's.
    int width = 0;
    int height = 0;
    // How long app_server gets to echo RP_INIT_CONNECTION once the transport
    // is connected.
    int handshake_timeout_ms = 15000;
    // Drive the handshake on this existing Session (reset() first, as a
    // reconnect must) instead of a new one; ConnectResult::session stays null.
    Session* session = nullptr;
    // The log callback a new Session is built with.
    Session::Log session_log;
    // Whether to check for the ssh executable on PATH before starting.
    // Defaults to "only when the real runner is used".
    std::optional<bool> check_ssh_executable;
};

// Run the preflight checks, open_connection, make_transport,
// Transport::connect() and the RP handshake, updating `flow` and calling
// `on_progress` at every stage boundary so a frontend can repaint. Blocking:
// run it on a worker (AsyncConnect) to keep a UI responsive. Never throws.
ConnectResult connect_with_progress(const ConnectionProfile& profile,
                                    ConnectFlow& flow,
                                    const std::function<void()>& on_progress,
                                    const ConnectOptions& options);

// The original signature, kept for callers that need nothing else.
ConnectResult connect_with_progress(const ConnectionProfile& profile,
                                    ConnectFlow& flow,
                                    const std::function<void()>& on_progress,
                                    CommandRunner* runner = nullptr,
                                    const TrustPrompt& ask_trust = {});

// connect_with_progress on a worker thread, for a UI thread that must keep
// painting. Starts at construction. `wake` is called from the worker whenever
// there is something new to show (a stage change, a trust question, the end),
// so it must be thread-safe -- SDL_PushEvent is. Everything else is called
// from the owning thread.
//
// The destructor cancels and joins: it does not return while an ssh child of
// this attempt is alive, whatever state the attempt is in.
class AsyncConnect {
public:
    // `options.cancel`, `.log` and `.ask_trust` are this object's own and are
    // ignored if set; the trust question is relayed through trust_question().
    AsyncConnect(ConnectionProfile profile, ConnectOptions options,
                 std::function<void()> wake);
    ~AsyncConnect();
    AsyncConnect(const AsyncConnect&) = delete;
    AsyncConnect& operator=(const AsyncConnect&) = delete;

    // Ask the attempt to stop. Returns at once; finished() turns true once
    // everything it started has been torn down.
    void cancel();
    [[nodiscard]] bool cancel_requested() const;
    [[nodiscard]] bool finished() const;
    // Block until finished() (the attempt is guaranteed to end: every wait in
    // it is bounded, and cancel() shortens them).
    void wait();

    // A copy of the flow as of the last stage boundary.
    [[nodiscard]] ConnectFlow flow() const;
    // Time since construction, frozen when the attempt finishes.
    [[nodiscard]] std::chrono::milliseconds elapsed() const;

    // A broker certificate question waiting for the user, if any. Answer it
    // with answer_trust(); cancel() answers it with a rejection.
    [[nodiscard]] std::optional<BrokerCheck> trust_question() const;
    void answer_trust(TrustChoice choice);

    // Once finished(): the outcome. Call once.
    [[nodiscard]] ConnectResult take_result();

    [[nodiscard]] DiagnosticLog& log() { return log_; }

private:
    void run(ConnectionProfile profile, ConnectOptions options);

    std::function<void()> wake_;
    DiagnosticLog log_;
    CancelToken cancel_;
    std::chrono::steady_clock::time_point start_;

    mutable std::mutex mutex_;
    std::condition_variable trust_cv_;
    std::condition_variable done_cv_;
    ConnectFlow shared_flow_; // the worker's flow as of its last publish
    std::optional<BrokerCheck> question_;
    std::optional<TrustChoice> answer_;
    bool finished_ = false;
    std::chrono::steady_clock::time_point end_;
    ConnectResult result_;

    std::thread worker_; // last: started after everything above exists
};

} // namespace haiku_remote
