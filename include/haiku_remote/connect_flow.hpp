#pragma once

// DeBeOS-RDP issue #1 Part 3: connection progress + actionable error states.
//
// Parts 1 and 2 gave the client a connection library and a managed-transport
// substrate (plan_launch -> an ordered RouteStep list; open_connection ->
// a ManagedConnection owning the SSH tunnel or the broker credentials). What
// was still missing is what the *user* sees between pressing Connect and the
// desktop appearing: today a slow or failed connect is a silent black window.
//
// This header is the pure, windowing-free half of the fix:
//
//   - ConnectFlow is a state machine over the plan's RouteStep sequence. It
//     turns the step/phase events open_connection reports (ConnectObserver) and
//     the transport-connect phase into an ordered list of ProgressStage entries
//     a renderer can draw, and it knows when the whole attempt has connected or
//     failed. It owns no Surface and no socket, so it is exhaustively unit
//     testable.
//
//   - classify_connect_failure() is the cause -> message/affordance table. It
//     maps *only* the information the transport / managed_transport layers
//     actually surface (ConnectFailure, and the error strings those layers
//     already produce) onto an actionable ConnectError. It never invents a
//     distinction the layer cannot make: where SSH collapses a rejected key, a
//     refused host and an unresolvable name into one exit code, this collapses
//     them into one honest message that names the possibilities.
//
//   - connect_with_progress() drives the real thing: open_connection (with an
//     observer feeding the flow) + make_transport + Transport::connect(),
//     calling an on_progress hook at each stage boundary so a frontend can
//     repaint. It consumes plan_launch's routing and open_connection's tunnel
//     lifecycle unchanged; it adds presentation, not protocol.
//
// The protocol, renderer, session and transport cores know nothing about any of
// this. The dependency arrow points one way: frontend -> connect screen ->
// connect flow -> managed_transport. Never back.

#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/transport.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace haiku_remote {

// One line of the progress view. The stages are built up-front from the plan:
// a broker step contributes "contacting broker" then "authenticating"; a tunnel
// step contributes "fetching cookie", "opening SSH tunnel", then "connecting".
// For direct/auto (broker then tunnel) the list holds the broker stages ahead
// of the tunnel stages, so a broker->tunnel fallback is visible as the broker
// stages failing and the tunnel stages lighting up.
enum class StageKind {
    contacting_broker, // broker: fetch token + certificate over SSH.
    authenticating,    // broker: TLS + WebSocket upgrade + RP_AUTHENTICATE.
    fetching_cookie,   // tunnel: read app_server's session cookie over SSH.
    opening_tunnel,    // tunnel: spawn and wait on the ssh -L forward.
    connecting,        // tunnel: open the TCP session and present the cookie.
};

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
};

enum class FlowState {
    connecting, // still working through the stages.
    connected,  // a route is up and the session can start.
    failed,     // every route was exhausted; error() says why.
};

// What actually went wrong, in terms the user can act on. The cause is the
// machine-readable key; title/detail/remedy are the human text; can_retry says
// whether offering Retry makes sense (a pin mismatch, for instance, will not fix
// itself on a second try). `raw` always keeps the underlying message so nothing
// is lost.
enum class ConnectErrorCause {
    none,
    missing_credential,    // no cookie/token available (ConnectFailure::missing_credential).
    ssh_auth_or_host,      // ssh exec/forward failed: key, refused host, DNS, port-in-use -- COLLAPSED.
    host_unreachable,      // the SSH connect itself timed out (a black-hole host).
    broker_unavailable,    // reached the host, but remote_broker returned no token/cert.
    broker_token_denied,   // RP_AUTH_RESULT status 1.
    broker_cert_pin,       // the broker's certificate did not match the pin.
    broker_cert_untrusted, // TLS chain / host-name verification failed.
    cookie_unavailable,    // the session cookie could not be read on the host.
    server_listener,       // app_server's remote interface is not reachable/listening.
    session_refused,       // socket opened, server hung up before any drawing (post-connect).
    other,                 // anything else; shown verbatim.
};

struct ConnectError {
    ConnectErrorCause cause = ConnectErrorCause::none;
    std::string title;
    std::string detail;
    std::string remedy;
    bool can_retry = true;
    std::string raw;

    // A single line suitable for the library card's last_error.
    [[nodiscard]] std::string summary() const;
};

// Where in the attempt a failure happened. This, plus the ConnectFailure flag
// and the raw message, is everything the classifier is given -- deliberately,
// so it cannot claim a distinction the layers did not make.
enum class ConnectFailPoint {
    substrate_broker, // open_connection, broker route (SSH fetch of token/cert).
    substrate_tunnel, // open_connection, tunnel route (cookie fetch / ssh -L).
    transport_broker, // WebSocketTransport::connect() (TLS, upgrade, auth).
    transport_tunnel, // TcpTransport::connect() (socket, cookie frame).
    session_refused,  // after a successful connect: 0 messages, orderly close.
};

[[nodiscard]] ConnectError classify_connect_failure(
    ConnectFailPoint where, ConnectFailure transport_failure,
    const std::string& message);

// The pure progress state machine. Build it from the plan, then feed it the
// same events open_connection's observer reports (plus the transport-connect
// phase and the terminal connected()/fail()). It is a plain value type -- no
// threads, no I/O -- so a test can drive the whole lifecycle by hand.
class ConnectFlow {
public:
    explicit ConnectFlow(const LaunchPlan& plan);

    void begin_step(std::size_t index, RouteKind kind);
    void phase(std::size_t index, ConnectPhase phase);
    void step_failed(std::size_t index, RouteKind kind, const std::string& error);
    // The substrate for `index` is up; the transport-connect stage is next.
    void begin_transport(std::size_t index, RouteKind kind);
    void connected();
    void fail(ConnectError error);

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
    FlowState state_ = FlowState::connecting;
    bool fell_back_ = false;
    ConnectError error_;
    std::string plan_note_;
};

// The result of a whole connect attempt. On success `transport` is connected
// and `connection` owns the SSH tunnel / broker certificate -- keep the result
// alive for as long as the session runs, exactly as a ManagedConnection was
// kept alive before.
struct ConnectResult {
    bool ok = false;
    std::unique_ptr<Transport> transport;
    ManagedConnection connection;
    ConnectError error;
    std::string route_note;
};

// Run open_connection + make_transport + Transport::connect(), updating `flow`
// and calling `on_progress` at every stage boundary so a frontend can repaint.
// Single-threaded and cooperative: the blocking SSH / connect calls run on this
// thread, with the stage label painted before each one. `runner` is for tests
// (a fake command runner stands in for ssh); when null a SystemCommandRunner is
// used. Never throws.
ConnectResult connect_with_progress(const ConnectionProfile& profile,
                                    ConnectFlow& flow,
                                    const std::function<void()>& on_progress,
                                    CommandRunner* runner = nullptr);

} // namespace haiku_remote
