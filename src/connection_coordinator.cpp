#include "haiku_remote/connection_coordinator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace haiku_remote {

std::string_view coordinator_state_name(CoordinatorState state)
{
    switch (state) {
    case CoordinatorState::idle:                           return "idle";
    case CoordinatorState::starting_tunnel:                return "starting_tunnel";
    case CoordinatorState::waiting_for_forward:            return "waiting_for_forward";
    case CoordinatorState::connecting_transport:           return "connecting_transport";
    case CoordinatorState::waiting_for_protocol_handshake: return "waiting_for_protocol_handshake";
    case CoordinatorState::requesting_display:             return "requesting_display";
    case CoordinatorState::connected:                      return "connected";
    case CoordinatorState::reconnecting:                   return "reconnecting";
    case CoordinatorState::disconnecting:                  return "disconnecting";
    case CoordinatorState::failed:                         return "failed";
    }
    return "unknown";
}

std::string_view reason_code(ConnectionReason reason)
{
    switch (reason) {
    case ConnectionReason::none:               return "none";
    case ConnectionReason::ssh_not_found:      return "ssh-not-found";
    case ConnectionReason::identity_missing:   return "identity-missing";
    case ConnectionReason::auth_failed:        return "auth-failed";
    case ConnectionReason::host_key:           return "host-key";
    case ConnectionReason::local_port_in_use:  return "local-port-in-use";
    case ConnectionReason::forward_timeout:    return "forward-timeout";
    case ConnectionReason::tcp_failed:         return "tcp-failed";
    case ConnectionReason::no_rp_init_ack:     return "no-RP_INIT-ack";
    case ConnectionReason::decode_failed:      return "decode-failed";
    case ConnectionReason::remote_closed:      return "remote-closed";
    }
    return "none";
}

ConnectionReason classify_ssh_failure(std::string_view text, bool forward_deadline_hit)
{
    const auto has = [&](std::string_view needle) {
        return text.find(needle) != std::string_view::npos;
    };

    // Most specific first. A changed TOFU key is a hard, actionable failure
    // (design decision 3): accept-new accepts a FIRST key but refuses a changed
    // one, and ssh then prints "Host key verification failed".
    if (has("Host key verification failed")
        || has("HOST IDENTIFICATION HAS CHANGED")
        || has("host key for") || has("known_hosts"))
        return ConnectionReason::host_key;

    // ssh warns "Warning: Identity file <path> not accessible" before it ever
    // tries to authenticate, which is distinct from an auth rejection.
    if (has("Identity file") && (has("not accessible") || has("No such file")))
        return ConnectionReason::identity_missing;

    if (has("Permission denied") || has("Too many authentication failures")
        || has("Authentication failed") || has("publickey"))
        return ConnectionReason::auth_failed;

    // ExitOnForwardFailure=yes turns a busy local port into an early ssh exit
    // with one of these on stderr.
    if (has("Address already in use") || has("cannot listen to port")
        || has("remote port forwarding failed") || has("bind:")
        || has("channel_setup_fwd_listener"))
        return ConnectionReason::local_port_in_use;

    // execvp could not find ssh: the child exits 127 with no stderr, which the
    // tunnel reports as "ssh exited (127)".
    if (has("exited (127)") || has("command not found")
        || has("could not launch ssh") || has("executable file not found"))
        return ConnectionReason::ssh_not_found;

    // A clean readiness timeout with nothing on stderr: the forward never came
    // up but ssh did not exit either.
    if (forward_deadline_hit)
        return ConnectionReason::forward_timeout;

    return ConnectionReason::tcp_failed;
}

namespace {

TransportFactory default_transport_factory()
{
    return [](const TransportOptions& options, std::string& error) {
        return make_transport(options, error);
    };
}

TunnelFactory default_tunnel_factory()
{
    return [](const TunnelConfig& config, const TunnelOptions& options) {
        return std::unique_ptr<ISshTunnel>(
            std::make_unique<SshTunnel>(config, options));
    };
}

} // namespace

ConnectionCoordinator::ConnectionCoordinator(CoordinatorConfig config,
                                             TransportFactory transport_factory,
                                             TunnelFactory tunnel_factory)
    : config_(std::move(config)),
      transport_factory_(transport_factory ? std::move(transport_factory)
                                            : default_transport_factory()),
      tunnel_factory_(tunnel_factory ? std::move(tunnel_factory)
                                      : default_tunnel_factory()),
      width_(config_.profile.width),
      height_(config_.profile.height),
      frame_(config_.profile.width, config_.profile.height)
{
}

ConnectionCoordinator::~ConnectionCoordinator()
{
    disconnect();
    if (worker_.joinable())
        worker_.join();
    // The worker owns the tunnel; if it never ran, nothing to stop. If it ran,
    // it stopped the tunnel on its way out. This is belt-and-braces for the
    // never-started case where a tunnel was somehow created.
    if (tunnel_)
        tunnel_->stop();
}

void ConnectionCoordinator::connect()
{
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true))
        return; // already started
    worker_ = std::thread([this] { run(); });
}

void ConnectionCoordinator::disconnect()
{
    stop_.store(true);
    wake_.notify_all();
}

void ConnectionCoordinator::request_full_repaint()
{
    std::lock_guard<std::mutex> guard(lock_);
    if (state_ == CoordinatorState::connected)
        repaint_requested_ = true;
}

void ConnectionCoordinator::post_input(std::vector<std::uint8_t> message)
{
    std::lock_guard<std::mutex> guard(lock_);
    // Only meaningful once a session exists; and bound the queue so a UI that
    // keeps posting while the link is wedged cannot grow it without limit.
    if (state_ != CoordinatorState::connected)
        return;
    if (input_queue_.size() < 4096)
        input_queue_.push_back(std::move(message));
}

Snapshot ConnectionCoordinator::snapshot() const
{
    std::lock_guard<std::mutex> guard(lock_);
    Snapshot snap;
    snap.state = state_;
    snap.reason = reason_;
    snap.message = message_;
    snap.diag.assign(diag_.begin(), diag_.end());
    snap.surface_generation = surface_generation_;
    snap.reconnect_attempt = reconnect_attempt_;
    snap.finished = finished_.load();
    const auto end = end_time_.value_or(std::chrono::steady_clock::now());
    if (start_time_.time_since_epoch().count() != 0) {
        snap.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start_time_);
    }
    return snap;
}

std::uint64_t ConnectionCoordinator::copy_frame(Surface& out) const
{
    std::lock_guard<std::mutex> guard(lock_);
    if (!have_frame_ || out.width() != frame_.width()
        || out.height() != frame_.height())
        return 0;
    const auto src = frame_.pixels();
    auto dst = out.pixels();
    std::copy(src.begin(), src.end(), dst.begin());
    return surface_generation_;
}

void ConnectionCoordinator::set_state(CoordinatorState state)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (state_ == state)
        return;
    std::string line = std::string(coordinator_state_name(state_)) + " -> "
        + std::string(coordinator_state_name(state));
    state_ = state;
    diag_.push_back(std::move(line));
    while (diag_.size() > config_.diag_capacity)
        diag_.pop_front();
}

void ConnectionCoordinator::note(std::string line)
{
    std::lock_guard<std::mutex> guard(lock_);
    diag_.push_back(std::move(line));
    while (diag_.size() > config_.diag_capacity)
        diag_.pop_front();
}

void ConnectionCoordinator::set_failure(ConnectionReason reason, std::string message)
{
    std::lock_guard<std::mutex> guard(lock_);
    reason_ = reason;
    message_ = std::move(message);
    diag_.push_back(std::string(reason_code(reason)) + ": " + message_);
    while (diag_.size() > config_.diag_capacity)
        diag_.pop_front();
}

void ConnectionCoordinator::refresh_frame()
{
    std::lock_guard<std::mutex> guard(lock_);
    const auto src = session_->surface().pixels();
    auto dst = frame_.pixels();
    std::copy(src.begin(), src.end(), dst.begin());
    have_frame_ = true;
    ++surface_generation_;
}

bool ConnectionCoordinator::wait_for(std::chrono::milliseconds delay)
{
    std::unique_lock<std::mutex> guard(lock_);
    wake_.wait_for(guard, delay, [this] { return stop_.load(); });
    return !stop_.load();
}

std::optional<std::uint16_t>
ConnectionCoordinator::bring_up_endpoint(std::string& cookie_out)
{
    cookie_out = config_.cookie;

    if (config_.profile.mode == ConnectionMode::direct) {
        // Direct mode skips the tunnel states entirely (issue #1).
        if (config_.profile.remote_port < min_port
            || config_.profile.remote_port > max_port) {
            set_failure(ConnectionReason::tcp_failed,
                        "invalid remote port for a direct connection");
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(config_.profile.remote_port);
    }

    set_state(CoordinatorState::starting_tunnel);
    TunnelConfig tc;
    tc.host = config_.profile.host;
    tc.ssh_user = config_.profile.ssh_user;
    tc.ssh_port = config_.profile.ssh_port;
    tc.identity_file = config_.profile.identity_file;
    tc.known_hosts_file = config_.profile.known_hosts_file;
    tc.remote_port = config_.profile.remote_port;
    tc.local_port = config_.profile.local_port.value_or(0);

    tunnel_ = tunnel_factory_(tc, config_.tunnel_options);
    if (!tunnel_) {
        set_failure(ConnectionReason::ssh_not_found, "could not create ssh tunnel");
        return std::nullopt;
    }

    // The tunnel's own start() reaps a verified orphan before launching, launches
    // ssh, and polls the forward to readiness -- it blocks through both the
    // starting and the forward-waiting phases, so from here on we are waiting for
    // the forward to come up.
    set_state(CoordinatorState::waiting_for_forward);
    const TunnelStatus status = tunnel_->start();

    const std::string& ssh_err = tunnel_->stderr_log();
    if (!ssh_err.empty())
        note("ssh: " + ssh_err);

    if (!status.ok()) {
        const bool deadline_hit =
            status.error.find("forward did not open") != std::string::npos;
        const ConnectionReason reason =
            classify_ssh_failure(ssh_err + "\n" + status.error, deadline_hit);
        set_failure(reason, status.error.empty() ? "ssh tunnel failed to start"
                                                  : status.error);
        return std::nullopt;
    }

    // Fetch the per-boot cookie over the SAME ssh identity, unless the caller
    // already supplied one. In memory only; never logged or persisted. A failure
    // leaves the cookie empty, which the raw transport reports as a missing
    // credential at connect().
    if (cookie_out.empty()) {
        if (const auto cookie = tunnel_->fetch_session_cookie())
            cookie_out = *cookie;
    }

    return static_cast<std::uint16_t>(tunnel_->local_port());
}

ConnectionResult ConnectionCoordinator::run_one_connection()
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        reason_ = ConnectionReason::none;
        message_.clear();
    }
    ConnectionResult result;

    TransportOptions options;
    if (config_.transport_override) {
        // A verbatim transport (e.g. a broker URL): no tunnel, straight to the
        // transport connect.
        options = *config_.transport_override;
    } else {
        std::string cookie;
        const auto port = bring_up_endpoint(cookie);
        if (!port)
            return result; // reason/message already set; connected == false
        options.host = config_.profile.mode == ConnectionMode::ssh
            ? std::string("127.0.0.1")
            : config_.profile.host;
        options.port = *port;
        options.cookie = cookie;
    }

    set_state(CoordinatorState::connecting_transport);
    std::string error;
    transport_ = transport_factory_(options, error);
    if (!transport_) {
        set_failure(ConnectionReason::tcp_failed,
                    "could not create transport: " + error);
        return result;
    }
    if (!transport_->connect(error)) {
        result.connected = false;
        result.connect_failure = transport_->connect_failure();
        if (result.connect_failure == ConnectFailure::missing_credential) {
            // No cookie reached the gate -- a this-side credential problem, not a
            // wire failure: the socket never opened.
            set_failure(ConnectionReason::tcp_failed,
                        "no session cookie for a direct connection: " + error);
        } else {
            set_failure(ConnectionReason::tcp_failed,
                        "connect to " + transport_->describe() + " failed: " + error);
        }
        return result;
    }
    result.connected = true;

    // waiting_for_protocol_handshake: send RP_INIT_CONNECTION + RP_HELLO, then
    // wait for the first decoded message within a bounded timeout.
    set_state(CoordinatorState::waiting_for_protocol_handshake);
    session_->set_sender([this](std::span<const std::uint8_t> bytes) {
        return transport_->send_all(bytes, send_error_);
    });
    send_error_.clear();
    session_->start();
    if (!send_error_.empty()) {
        result.peer_closed = transport_->peer_closed();
        result.connection_reset = transport_->connection_reset();
        set_failure(ConnectionReason::remote_closed,
                    "handshake send failed: " + send_error_);
        return result;
    }

    std::array<std::uint8_t, 256 * 1024> buffer {};
    const auto handshake_deadline =
        std::chrono::steady_clock::now() + config_.handshake_timeout;
    std::size_t bytes_seen = 0;
    for (;;) {
        if (stop_requested())
            return result;
        if (std::chrono::steady_clock::now() >= handshake_deadline) {
            // A bounded handshake timeout: TCP is up but the protocol never
            // acked. Distinguish "nothing arrived" from "bytes arrived but
            // nothing decoded" so the reason is actionable.
            set_failure(bytes_seen > 0 ? ConnectionReason::decode_failed
                                       : ConnectionReason::no_rp_init_ack,
                        bytes_seen > 0
                            ? "bytes arrived but no protocol message decoded"
                            : "no RP_INIT_CONNECTION acknowledgement from the server");
            return result;
        }
        const int count = transport_->receive(buffer, 100, error);
        if (count < 0) {
            result.peer_closed = transport_->peer_closed();
            result.connection_reset = transport_->connection_reset();
            result.server_closed = session_->server_closed();
            set_failure(ConnectionReason::remote_closed,
                        "connection closed during the protocol handshake");
            return result;
        }
        if (count > 0) {
            bytes_seen += static_cast<std::size_t>(count);
            session_->ingest(
                std::span(buffer.data(), static_cast<std::size_t>(count)));
            refresh_frame();
        }
        if (session_->message_count() > 0)
            break; // handshake acknowledged
        if (session_->server_closed()) {
            result.server_closed = true;
            set_failure(ConnectionReason::remote_closed,
                        "the server closed the connection during the handshake");
            return result;
        }
    }

    // requesting_display: ask for a full replay so the first frame is complete.
    set_state(CoordinatorState::requesting_display);
    session_->request_full_repaint();
    refresh_frame();

    // connected: the steady receive loop. Input and repaint requests queued by
    // the UI thread are drained and sent here, so the Session stays confined to
    // this worker thread.
    set_state(CoordinatorState::connected);
    for (;;) {
        if (stop_requested())
            return result; // user-initiated disconnect

        std::vector<std::vector<std::uint8_t>> pending;
        bool repaint = false;
        {
            std::lock_guard<std::mutex> guard(lock_);
            pending.swap(input_queue_);
            repaint = repaint_requested_;
            repaint_requested_ = false;
        }
        for (auto& message : pending) {
            if (!session_->send_client_message(message)) {
                set_failure(ConnectionReason::remote_closed,
                            "send failed: " + send_error_);
                return result;
            }
        }
        if (repaint)
            session_->request_full_repaint();

        const int count = transport_->receive(buffer, 50, error);
        if (count < 0) {
            result.peer_closed = transport_->peer_closed();
            result.connection_reset = transport_->connection_reset();
            if (result.peer_closed || result.connection_reset) {
                set_failure(ConnectionReason::remote_closed,
                            result.connection_reset
                                ? "the server reset the connection"
                                : "the server closed the connection");
            } else {
                set_failure(ConnectionReason::tcp_failed, "receive failed: " + error);
            }
            return result;
        }
        if (count > 0) {
            session_->ingest(
                std::span(buffer.data(), static_cast<std::size_t>(count)));
            refresh_frame();
        }
        if (session_->server_closed()) {
            // An orderly, server-initiated teardown. Clean: leave reason none so
            // the terminal state is disconnecting, not failed.
            result.server_closed = true;
            return result;
        }
    }
}

void ConnectionCoordinator::run()
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        start_time_ = std::chrono::steady_clock::now();
    }
    // One Session, one Surface, for the life of the coordinator: they persist
    // across reconnects (reset() between attempts) exactly as the headless
    // client does, so the surface identity survives a reconnect.
    session_ = std::make_unique<Session>(
        width_, height_, [](std::span<const std::uint8_t>) { return true; },
        [this](std::string_view line) { note(std::string(line)); });

    const ReconnectPolicy policy(config_.reconnect);
    int attempts_made = 0;

    for (;;) {
        if (stop_requested())
            break;
        if (attempts_made > 0) {
            set_state(CoordinatorState::reconnecting);
            {
                std::lock_guard<std::mutex> guard(lock_);
                reconnect_attempt_ = attempts_made;
            }
            // Tear the previous forward and transport down before rebuilding
            // (issue #1), then discard every scrap of the previous session's
            // client state so the replay is not composited on top of it.
            if (transport_) {
                transport_->close();
                transport_.reset();
            }
            if (tunnel_) {
                tunnel_->stop();
                tunnel_.reset();
            }
            session_->reset();
            if (!wait_for(policy.backoff_for(attempts_made - 1)))
                break; // stop requested during backoff
        }

        ConnectionResult result = run_one_connection();
        result.message_count = session_->message_count();

        if (stop_requested())
            break;

        const ConnectionOutcome outcome = classify_connection(result);
        if (!policy.should_retry(outcome, attempts_made))
            break;
        ++attempts_made;
    }

    // Teardown: close the transport and stop the tunnel unconditionally.
    if (transport_) {
        transport_->close();
        transport_.reset();
    }
    if (tunnel_) {
        tunnel_->stop();
        tunnel_.reset();
    }

    // Terminal state. A user-requested stop is a disconnect; a clean
    // server-initiated close (reason left none) is also a disconnect; anything
    // with a reason set is a failure the UI can act on.
    const bool stopped = stop_requested();
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (stopped || reason_ == ConnectionReason::none)
            state_ = CoordinatorState::disconnecting;
        else
            state_ = CoordinatorState::failed;
        end_time_ = std::chrono::steady_clock::now();
    }
    finished_.store(true);
}

} // namespace haiku_remote
