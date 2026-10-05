#include "haiku_remote/connect_flow.hpp"

#include <string_view>
#include <utility>

namespace haiku_remote {

namespace {

bool has(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

const char* label_for(StageKind kind)
{
    switch (kind) {
    case StageKind::contacting_broker:
        return "Contacting broker";
    case StageKind::authenticating:
        return "Authenticating with broker";
    case StageKind::fetching_cookie:
        return "Fetching session cookie";
    case StageKind::opening_tunnel:
        return "Opening SSH tunnel";
    case StageKind::connecting:
        return "Connecting to session";
    }
    return "";
}

StageKind stage_for_phase(ConnectPhase phase)
{
    switch (phase) {
    case ConnectPhase::contacting_broker:
        return StageKind::contacting_broker;
    case ConnectPhase::fetching_cookie:
        return StageKind::fetching_cookie;
    case ConnectPhase::opening_tunnel:
        return StageKind::opening_tunnel;
    }
    return StageKind::connecting;
}

} // namespace

// ---------------------------------------------------------------------------
// ConnectError
// ---------------------------------------------------------------------------

std::string ConnectError::summary() const
{
    if (title.empty())
        return raw;
    if (remedy.empty())
        return title;
    return title + " \xE2\x80\x94 " + remedy;
}

// ---------------------------------------------------------------------------
// classify_connect_failure -- the cause -> message/affordance table
// ---------------------------------------------------------------------------
//
// Every branch is driven only by what the layer actually told us: the fail
// point, the ConnectFailure flag, and the raw message string the layer wrote.
// Where several real causes reach us as one string (SSH's single error for a
// bad key, a refused host and an unresolvable name), they are deliberately
// mapped to one honest message that names the possibilities rather than picking
// one and being wrong.

ConnectError classify_connect_failure(ConnectFailPoint where,
                                      ConnectFailure transport_failure,
                                      const std::string& message)
{
    ConnectError e;
    e.raw = message;
    e.can_retry = true;

    switch (where) {
    case ConnectFailPoint::substrate_broker:
        if (has(message, "timed out")) {
            e.cause = ConnectErrorCause::host_unreachable;
            e.title = "Host unreachable";
            e.detail = "Contacting the host over SSH timed out before the broker"
                       " could answer.";
            e.remedy = "Check the host name and that it is reachable (VPN up,"
                       " security group open), then Retry.";
        } else if (has(message, "no token") || has(message, "no certificate")
                   || has(message, "certificate marker")) {
            e.cause = ConnectErrorCause::broker_unavailable;
            e.title = "Broker not available";
            e.detail = "SSH reached the host, but its remote-desktop broker did"
                       " not return a token and certificate.";
            e.remedy = "Make sure remote_broker and app_server are running on the"
                       " host, then Retry.";
        } else {
            // "ssh ... exited N" / "could not launch": one SSH error for several
            // real causes.
            e.cause = ConnectErrorCause::ssh_auth_or_host;
            e.title = "SSH connection failed";
            e.detail = "SSH to the host failed before the broker could be reached."
                       " SSH reports the same error for a rejected key, a refused"
                       " connection and an unresolvable name, so the cause is one"
                       " of those.";
            e.remedy = "Check the SSH user, identity file and host name/port, and"
                       " that you can ssh to it by hand. Then Retry.";
        }
        break;

    case ConnectFailPoint::substrate_tunnel:
        if (has(message, "session_cookie")) {
            e.cause = ConnectErrorCause::cookie_unavailable;
            e.title = "No session cookie on the server";
            e.detail = "SSH worked, but app_server's per-boot session cookie could"
                       " not be read on the host -- so it is missing, or the remote"
                       " interface has not published one yet.";
            e.remedy = "Confirm app_server's remote interface is running on the host"
                       " and that the cookie file is readable by your SSH user."
                       " Then Retry.";
        } else if (has(message, "command timed out")) {
            e.cause = ConnectErrorCause::host_unreachable;
            e.title = "Host unreachable";
            e.detail = "The SSH command to the host timed out.";
            e.remedy = "Check the host name and that it is reachable, then Retry.";
        } else if (has(message, "forward")) {
            // ExitOnForwardFailure, or the forward never came up: SSH collapses a
            // bad key, a busy local port and a dead remote port into this.
            e.cause = ConnectErrorCause::ssh_auth_or_host;
            e.title = "SSH tunnel could not be opened";
            e.detail = "SSH exited before the local forward came up. SSH collapses"
                       " several causes into this: a rejected key, the local"
                       " forward port already in use, or nothing listening on the"
                       " remote port.";
            e.remedy = "Check the SSH key/user/host, that the local port is free,"
                       " and that app_server's remote port is listening on the"
                       " host. Then Retry.";
        } else {
            e.cause = ConnectErrorCause::ssh_auth_or_host;
            e.title = "SSH connection failed";
            e.detail = "SSH to the host failed before the tunnel could be opened."
                       " SSH reports the same error for a rejected key, a refused"
                       " connection and an unresolvable name.";
            e.remedy = "Check the SSH user, identity file and host name/port, and"
                       " that you can ssh to it by hand. Then Retry.";
        }
        break;

    case ConnectFailPoint::transport_broker:
        if (transport_failure == ConnectFailure::missing_credential) {
            e.cause = ConnectErrorCause::missing_credential;
            e.title = "No broker token";
            e.detail = "A broker (wss) connection needs an authentication token and"
                       " none was available.";
            e.remedy = "The token is normally fetched for you; Retry, or check the"
                       " broker's token file on the host.";
        } else if (has(message, "pin mismatch")) {
            e.cause = ConnectErrorCause::broker_cert_pin;
            e.title = "Broker certificate changed";
            e.detail = "The broker's TLS certificate does not match the pinned"
                       " fingerprint for this host.";
            e.remedy = "If the broker was reinstalled, update or clear the pinned"
                       " fingerprint. Otherwise do not trust this connection.";
            e.can_retry = false; // a second try pins the same mismatch.
        } else if (has(message, "denied the authentication token")) {
            e.cause = ConnectErrorCause::broker_token_denied;
            e.title = "Broker rejected the token";
            e.detail = "The broker denied the authentication token.";
            e.remedy = "The token may be stale; Retry to fetch a fresh one, or"
                       " check the broker's token file on the host.";
        } else if (has(message, "no session to attach")) {
            e.cause = ConnectErrorCause::server_listener;
            e.title = "No session behind the broker";
            e.detail = "The broker accepted the token but has no app_server session"
                       " to attach to.";
            e.remedy = "Start app_server's remote interface on the host, then"
                       " Retry.";
        } else if (has(message, "could not read app_server's session cookie")) {
            e.cause = ConnectErrorCause::server_listener;
            e.title = "Broker cannot read the session cookie";
            e.detail = "The broker authenticated but could not read app_server's"
                       " session cookie, so it has nothing to open a session with.";
            e.remedy = "Make sure app_server's remote interface is listening and its"
                       " cookie file is readable by the broker's user. Then Retry.";
        } else if (has(message, "TLS handshake") || has(message, "trust store")
                   || has(message, "ca-file") || has(message, "host name")
                   || has(message, "no certificate to pin")) {
            e.cause = ConnectErrorCause::broker_cert_untrusted;
            e.title = "Broker certificate not trusted";
            e.detail = "The TLS handshake with the broker could not be verified.";
            e.remedy = "Check the broker's certificate or CA, or the pinned"
                       " fingerprint. Then Retry.";
        } else {
            e.cause = ConnectErrorCause::other;
            e.title = "Broker connection failed";
            e.detail = message;
            e.remedy = "Retry, or inspect the broker on the host.";
        }
        break;

    case ConnectFailPoint::transport_tunnel:
        if (transport_failure == ConnectFailure::missing_credential) {
            e.cause = ConnectErrorCause::missing_credential;
            e.title = "No session cookie";
            e.detail = "A direct connection to the session port needs app_server's"
                       " per-boot cookie and none was available.";
            e.remedy = "Set a cookie source on the profile, or use the broker (wss)."
                       " Then Retry.";
        } else {
            e.cause = ConnectErrorCause::server_listener;
            e.title = "Could not reach the session port";
            e.detail = "The local end of the tunnel did not accept the session"
                       " connection.";
            e.remedy = "Confirm app_server's remote interface is listening on the"
                       " host. Then Retry.";
        }
        break;

    case ConnectFailPoint::session_refused:
        e.cause = ConnectErrorCause::session_refused;
        e.title = "Session refused by the server";
        e.detail = "The connection opened but the server closed it without sending"
                   " any drawing. A stale session cookie, or an app_server remote"
                   " interface that is not listening, both look like this.";
        e.remedy = "Reconnect to fetch a fresh cookie; if it persists, check"
                   " app_server's remote interface on the host.";
        break;
    }

    if (e.title.empty()) {
        e.cause = ConnectErrorCause::other;
        e.title = "Connection failed";
        e.detail = message;
    }
    return e;
}

// ---------------------------------------------------------------------------
// ConnectFlow
// ---------------------------------------------------------------------------

ConnectFlow::ConnectFlow(const LaunchPlan& plan)
{
    plan_note_ = plan.note;
    for (std::size_t i = 0; i < plan.steps.size(); ++i) {
        const RouteKind kind = plan.steps[i].kind;
        const auto add = [&](StageKind sk) {
            ProgressStage stage;
            stage.kind = sk;
            stage.route = kind;
            stage.step_index = i;
            stage.state = StageState::pending;
            stage.label = label_for(sk);
            stages_.push_back(std::move(stage));
        };
        if (kind == RouteKind::broker) {
            add(StageKind::contacting_broker);
            add(StageKind::authenticating);
        } else {
            add(StageKind::fetching_cookie);
            add(StageKind::opening_tunnel);
            add(StageKind::connecting);
        }
    }
}

std::optional<std::size_t> ConnectFlow::find_stage(std::size_t step_index,
                                                   StageKind kind) const
{
    for (std::size_t i = 0; i < stages_.size(); ++i)
        if (stages_[i].step_index == step_index && stages_[i].kind == kind)
            return i;
    return std::nullopt;
}

std::optional<std::size_t> ConnectFlow::active_stage() const
{
    for (std::size_t i = 0; i < stages_.size(); ++i)
        if (stages_[i].state == StageState::active)
            return i;
    return std::nullopt;
}

void ConnectFlow::begin_step(std::size_t index, RouteKind kind)
{
    (void)kind;
    // Reaching any step after the first means the earlier route(s) failed and
    // we fell back -- the broker->tunnel fallback of direct/auto mode.
    if (index > 0)
        fell_back_ = true;
}

void ConnectFlow::phase(std::size_t index, ConnectPhase p)
{
    const auto target = find_stage(index, stage_for_phase(p));
    if (!target)
        return;
    // Earlier stages of the same step are now behind us.
    for (std::size_t i = 0; i < *target; ++i) {
        if (stages_[i].step_index == index
            && (stages_[i].state == StageState::active
                || stages_[i].state == StageState::pending)) {
            stages_[i].state = StageState::done;
        }
    }
    stages_[*target].state = StageState::active;
}

void ConnectFlow::begin_transport(std::size_t index, RouteKind kind)
{
    const StageKind sk = kind == RouteKind::broker ? StageKind::authenticating
                                                   : StageKind::connecting;
    const auto target = find_stage(index, sk);
    if (!target)
        return;
    for (std::size_t i = 0; i < *target; ++i) {
        if (stages_[i].step_index == index
            && (stages_[i].state == StageState::active
                || stages_[i].state == StageState::pending)) {
            stages_[i].state = StageState::done;
        }
    }
    stages_[*target].state = StageState::active;
}

void ConnectFlow::step_failed(std::size_t index, RouteKind kind,
                              const std::string& error)
{
    (void)kind;
    (void)error;
    // The stage running now is the one that failed; anything still pending in
    // this step is skipped (the fallback, if any, starts a fresh step).
    bool marked = false;
    for (std::size_t i = 0; i < stages_.size(); ++i) {
        if (stages_[i].step_index != index)
            continue;
        if (stages_[i].state == StageState::active) {
            stages_[i].state = StageState::failed;
            marked = true;
        } else if (stages_[i].state == StageState::pending) {
            stages_[i].state = StageState::skipped;
        }
    }
    if (!marked) {
        // No stage was active (defensive): fail the step's first stage.
        for (std::size_t i = 0; i < stages_.size(); ++i) {
            if (stages_[i].step_index == index) {
                stages_[i].state = StageState::failed;
                break;
            }
        }
    }
}

void ConnectFlow::connected()
{
    if (const auto active = active_stage())
        stages_[*active].state = StageState::done;
    state_ = FlowState::connected;
}

void ConnectFlow::fail(ConnectError error)
{
    if (const auto active = active_stage())
        stages_[*active].state = StageState::failed;
    error_ = std::move(error);
    state_ = FlowState::failed;
}

// ---------------------------------------------------------------------------
// connect_with_progress
// ---------------------------------------------------------------------------

namespace {

// Bridges open_connection's observer callbacks into the flow and the repaint
// hook, and remembers the last step failure and the ready step so the driver
// can classify precisely and start the right transport-connect stage.
class FlowBridge final : public ConnectObserver {
public:
    FlowBridge(ConnectFlow& flow, const std::function<void()>& on_progress)
        : flow_(flow), on_progress_(on_progress)
    {
    }

    void on_step_begin(std::size_t index, RouteKind kind) override
    {
        flow_.begin_step(index, kind);
        on_progress_();
    }
    void on_phase(std::size_t index, ConnectPhase phase) override
    {
        flow_.phase(index, phase);
        on_progress_();
    }
    void on_step_failed(std::size_t index, RouteKind kind,
                        const std::string& error) override
    {
        have_fail = true;
        fail_kind = kind;
        fail_error = error;
        flow_.step_failed(index, kind, error);
        on_progress_();
    }
    void on_step_ready(std::size_t index, RouteKind kind) override
    {
        ready_index = index;
        ready_kind = kind;
    }

    bool have_fail = false;
    RouteKind fail_kind = RouteKind::broker;
    std::string fail_error;
    std::size_t ready_index = 0;
    RouteKind ready_kind = RouteKind::broker;

private:
    ConnectFlow& flow_;
    const std::function<void()>& on_progress_;
};

} // namespace

ConnectResult connect_with_progress(const ConnectionProfile& profile,
                                    ConnectFlow& flow,
                                    const std::function<void()>& on_progress,
                                    CommandRunner* runner)
{
    const std::function<void()> tick =
        on_progress ? on_progress : std::function<void()>([] {});

    ConnectResult result;
    FlowBridge bridge(flow, tick);

    ManagedConnection conn = open_connection(profile, runner, &bridge);
    if (!conn.ok) {
        const ConnectFailPoint where =
            bridge.have_fail && bridge.fail_kind == RouteKind::broker
                ? ConnectFailPoint::substrate_broker
                : ConnectFailPoint::substrate_tunnel;
        const std::string msg = bridge.have_fail ? bridge.fail_error : conn.error;
        result.error = classify_connect_failure(where, ConnectFailure::none, msg);
        flow.fail(result.error);
        tick();
        return result;
    }

    // Substrate is up; the transport-connect stage begins.
    flow.begin_transport(bridge.ready_index, conn.kind);
    tick();

    const std::string note = conn.note;
    const RouteKind kind = conn.kind;
    const ConnectFailPoint transport_point = kind == RouteKind::broker
                                                 ? ConnectFailPoint::transport_broker
                                                 : ConnectFailPoint::transport_tunnel;

    std::string error;
    auto transport = make_transport(conn.transport, error);
    if (transport == nullptr) {
        result.error =
            classify_connect_failure(transport_point, ConnectFailure::other, error);
        flow.fail(result.error);
        tick();
        return result;
    }
    if (!transport->connect(error)) {
        result.error = classify_connect_failure(
            transport_point, transport->connect_failure(), error);
        flow.fail(result.error);
        tick();
        return result;
    }

    flow.connected();
    tick();
    result.ok = true;
    result.route_note = note;
    result.transport = std::move(transport);
    result.connection = std::move(conn);
    return result;
}

} // namespace haiku_remote
