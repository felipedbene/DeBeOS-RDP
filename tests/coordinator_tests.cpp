// Tests for the connection coordinator, the application-state mapping, and the
// launch-flag parser. The worker-thread state machine is driven by a FAKE
// ISshTunnel and an INJECTED fake Transport, so these run with no sockets, no
// ssh and no python -- the state transitions, the bounded handshake timeout, the
// SSH-failure classification, and reconnect sequencing are all exercised in
// process.
//
// House rules (CLAUDE.md): assert exact values; keep expectations independent of
// the code that produces them (the transition strings and reason codes below are
// written out, not read back); and give the handshake path an asymmetric subject
// -- a transport that delivers a frame reaches connected, one that delivers
// nothing fails no-RP_INIT-ack, and one that dribbles undecodable bytes fails
// decode-failed.

#include "haiku_remote/application_state.hpp"
#include "haiku_remote/connection_coordinator.hpp"
#include "haiku_remote/launch_options.hpp"
#include "haiku_remote/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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

// One valid, framed server->client message. The session increments its message
// count for any decoded frame regardless of opcode, so this is enough to satisfy
// the protocol-handshake wait.
std::vector<std::uint8_t> one_message()
{
    return Writer(Op::update_display_mode).finish();
}

// ---- An injected fake transport. ----

class FakeTransport final : public Transport {
public:
    enum class End { timeout, clean_fin, reset, error };

    bool connect_ok = true;
    ConnectFailure connect_fail = ConnectFailure::none;
    std::vector<std::vector<std::uint8_t>> to_deliver;
    End end = End::timeout;

    bool connect(std::string& error) override
    {
        if (!connect_ok) {
            failure_ = connect_fail;
            error = "fake: connect refused";
            return false;
        }
        return true;
    }

    bool send_all(std::span<const std::uint8_t>, std::string&) override
    {
        return true;
    }

    int receive(std::span<std::uint8_t> destination, int timeout_ms,
                std::string& error) override
    {
        if (index_ < to_deliver.size()) {
            const auto& chunk = to_deliver[index_++];
            std::copy(chunk.begin(), chunk.end(), destination.begin());
            return static_cast<int>(chunk.size());
        }
        switch (end) {
        case End::timeout:
            std::this_thread::sleep_for(
                std::chrono::milliseconds(std::min(timeout_ms, 5)));
            return 0;
        case End::clean_fin:
            peer_closed_ = true;
            error = "peer closed";
            return -1;
        case End::reset:
            reset_ = true;
            error = "connection reset";
            return -1;
        case End::error:
            error = "transport error";
            return -1;
        }
        return 0;
    }

    void close() override {}
    [[nodiscard]] bool peer_closed() const override { return peer_closed_; }
    [[nodiscard]] bool connection_reset() const override { return reset_; }
    [[nodiscard]] std::string describe() const override { return "fake://endpoint"; }

private:
    std::size_t index_ = 0;
    bool peer_closed_ = false;
    bool reset_ = false;
};

// ---- A fake ssh tunnel. ----

struct FakeTunnelState {
    int start_calls = 0;
    int stop_calls = 0;
    int cookie_calls = 0;
    TunnelConfig last_config;
};

class FakeTunnel final : public ISshTunnel {
public:
    FakeTunnel(FakeTunnelState* state, TunnelStatus status, std::string stderr_text,
               std::optional<std::string> cookie)
        : state_(state), status_(status), stderr_(std::move(stderr_text)),
          cookie_(std::move(cookie))
    {
    }

    TunnelStatus start() override
    {
        if (state_)
            ++state_->start_calls;
        running_ = status_.ok();
        return status_;
    }
    void stop() override
    {
        if (state_)
            ++state_->stop_calls;
        running_ = false;
    }
    [[nodiscard]] TunnelState state() const override
    {
        return running_ ? TunnelState::running : TunnelState::stopped;
    }
    [[nodiscard]] int local_port() const override { return status_.local_port; }
    [[nodiscard]] const std::string& stderr_log() const override { return stderr_; }
    [[nodiscard]] std::optional<std::string> fetch_session_cookie() override
    {
        if (state_)
            ++state_->cookie_calls;
        return cookie_;
    }

private:
    FakeTunnelState* state_;
    TunnelStatus status_;
    std::string stderr_;
    std::optional<std::string> cookie_;
    bool running_ = false;
};

ConnectionProfile direct_profile()
{
    ConnectionProfile profile;
    profile.name = "direct";
    profile.mode = ConnectionMode::direct;
    profile.host = "127.0.0.1";
    profile.remote_port = 10900;
    profile.width = 320;
    profile.height = 240;
    return profile;
}

ConnectionProfile ssh_profile()
{
    ConnectionProfile profile;
    profile.name = "ssh";
    profile.mode = ConnectionMode::ssh;
    profile.host = "example.invalid";
    profile.ssh_user = "baron";
    profile.remote_port = 10900;
    profile.width = 320;
    profile.height = 240;
    return profile;
}

template <class Predicate>
bool wait_until(const ConnectionCoordinator& coordinator, Predicate predicate,
                int timeout_ms = 4000)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate(coordinator.snapshot()))
            return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate(coordinator.snapshot());
}

bool diag_has(const std::vector<std::string>& diag, std::string_view needle)
{
    for (const auto& line : diag)
        if (line.find(needle) != std::string::npos)
            return true;
    return false;
}

// ---- classify_ssh_failure (pure) ----

void test_ssh_failure_classification_is_exact()
{
    check(classify_ssh_failure("Host key verification failed.", false)
              == ConnectionReason::host_key,
          "host key verification -> host-key");
    check(classify_ssh_failure(
              "Warning: Identity file /no/such not accessible: No such file"
              " or directory", false)
              == ConnectionReason::identity_missing,
          "missing identity -> identity-missing");
    check(classify_ssh_failure("baron@h: Permission denied (publickey).", false)
              == ConnectionReason::auth_failed,
          "permission denied -> auth-failed");
    check(classify_ssh_failure(
              "bind [127.0.0.1]:15900: Address already in use", false)
              == ConnectionReason::local_port_in_use,
          "address in use -> local-port-in-use");
    check(classify_ssh_failure("ssh exited (127)", false)
              == ConnectionReason::ssh_not_found,
          "exit 127 -> ssh-not-found");
    check(classify_ssh_failure("forward did not open within timeout", true)
              == ConnectionReason::forward_timeout,
          "readiness timeout -> forward-timeout");
    check(classify_ssh_failure("", false) == ConnectionReason::tcp_failed,
          "nothing recognisable -> tcp-failed");
}

void test_reason_codes_are_the_actionable_set()
{
    check(reason_code(ConnectionReason::ssh_not_found) == "ssh-not-found", "r1");
    check(reason_code(ConnectionReason::identity_missing) == "identity-missing", "r2");
    check(reason_code(ConnectionReason::auth_failed) == "auth-failed", "r3");
    check(reason_code(ConnectionReason::host_key) == "host-key", "r4");
    check(reason_code(ConnectionReason::local_port_in_use) == "local-port-in-use", "r5");
    check(reason_code(ConnectionReason::forward_timeout) == "forward-timeout", "r6");
    check(reason_code(ConnectionReason::tcp_failed) == "tcp-failed", "r7");
    check(reason_code(ConnectionReason::no_rp_init_ack) == "no-RP_INIT-ack", "r8");
    check(reason_code(ConnectionReason::decode_failed) == "decode-failed", "r9");
    check(reason_code(ConnectionReason::remote_closed) == "remote-closed", "r10");
}

// ---- the direct-mode happy path and its transitions ----

void test_direct_connect_reaches_connected_with_expected_transitions()
{
    CoordinatorConfig config;
    config.profile = direct_profile();
    config.cookie = "cookie-value";

    TransportOptions seen_options;
    auto transport_factory = [&](const TransportOptions& options, std::string&) {
        seen_options = options;
        auto transport = std::make_unique<FakeTransport>();
        transport->to_deliver.push_back(one_message());
        transport->end = FakeTransport::End::timeout;
        return std::unique_ptr<Transport>(std::move(transport));
    };

    ConnectionCoordinator coordinator(config, transport_factory);
    coordinator.connect();
    const bool reached = wait_until(coordinator, [](const Snapshot& s) {
        return s.state == CoordinatorState::connected;
    });
    check(reached, "direct connect reaches connected");

    const Snapshot snap = coordinator.snapshot();
    check(seen_options.host == "127.0.0.1", "direct transport host is the profile host");
    check(seen_options.port == 10900, "direct transport port is the remote port");
    check(seen_options.cookie == "cookie-value", "direct transport carries the cookie");
    // The tunnel states are skipped in direct mode.
    check(!diag_has(snap.diag, "starting_tunnel"), "direct mode skips the tunnel");
    check(diag_has(snap.diag, "idle -> connecting_transport"),
          "direct mode goes straight to connecting_transport");
    check(diag_has(snap.diag, "connecting_transport -> waiting_for_protocol_handshake"),
          "then to the handshake wait");
    check(diag_has(snap.diag, "waiting_for_protocol_handshake -> requesting_display"),
          "then requesting_display after the first message");
    check(diag_has(snap.diag, "requesting_display -> connected"),
          "then connected");

    // A frame was handed out under lock.
    Surface frame(320, 240);
    check(coordinator.copy_frame(frame) > 0, "a frame copy is available once connected");

    coordinator.disconnect();
    const bool finished = wait_until(coordinator, [](const Snapshot& s) {
        return s.finished;
    });
    check(finished, "the worker finishes after disconnect");
    const Snapshot done = coordinator.snapshot();
    check(done.state == CoordinatorState::disconnecting,
          "a user disconnect ends in disconnecting, not failed");
    check(done.reason == ConnectionReason::none, "a clean disconnect has no failure reason");
}

// ---- the bounded handshake timeout ----

void test_handshake_timeout_fails_no_rp_init_ack()
{
    CoordinatorConfig config;
    config.profile = direct_profile();
    config.cookie = "c";
    config.handshake_timeout = 150ms;

    auto transport_factory = [&](const TransportOptions&, std::string&) {
        auto transport = std::make_unique<FakeTransport>();
        // Connects, but never delivers a single byte.
        transport->end = FakeTransport::End::timeout;
        return std::unique_ptr<Transport>(std::move(transport));
    };

    ConnectionCoordinator coordinator(config, transport_factory);
    coordinator.connect();
    const bool failed = wait_until(coordinator, [](const Snapshot& s) {
        return s.finished;
    });
    check(failed, "the worker finishes on a handshake timeout");
    const Snapshot snap = coordinator.snapshot();
    check(snap.state == CoordinatorState::failed, "a handshake timeout is a failure");
    check(snap.reason == ConnectionReason::no_rp_init_ack,
          "no bytes at all -> no-RP_INIT-ack");
}

void test_handshake_with_undecodable_bytes_fails_decode()
{
    CoordinatorConfig config;
    config.profile = direct_profile();
    config.cookie = "c";
    config.handshake_timeout = 150ms;

    auto transport_factory = [&](const TransportOptions&, std::string&) {
        auto transport = std::make_unique<FakeTransport>();
        // A partial frame header: bytes arrive, but nothing ever decodes.
        transport->to_deliver.push_back({0x02, 0x00, 0x06});
        transport->end = FakeTransport::End::timeout;
        return std::unique_ptr<Transport>(std::move(transport));
    };

    ConnectionCoordinator coordinator(config, transport_factory);
    coordinator.connect();
    const bool failed = wait_until(coordinator, [](const Snapshot& s) {
        return s.finished;
    });
    check(failed, "the worker finishes when bytes never decode");
    const Snapshot snap = coordinator.snapshot();
    check(snap.reason == ConnectionReason::decode_failed,
          "bytes but no decoded message -> decode-failed");
}

// ---- the SSH path through a fake tunnel ----

void test_ssh_path_brings_up_tunnel_then_connects()
{
    CoordinatorConfig config;
    config.profile = ssh_profile();

    FakeTunnelState tunnel_state;
    auto tunnel_factory = [&](const TunnelConfig& cfg, const TunnelOptions&) {
        tunnel_state.last_config = cfg;
        TunnelStatus status{TunnelState::running, 15901, {}};
        return std::unique_ptr<ISshTunnel>(std::make_unique<FakeTunnel>(
            &tunnel_state, status, std::string{}, std::string{"boot-cookie"}));
    };
    TransportOptions seen_options;
    auto transport_factory = [&](const TransportOptions& options, std::string&) {
        seen_options = options;
        auto transport = std::make_unique<FakeTransport>();
        transport->to_deliver.push_back(one_message());
        transport->end = FakeTransport::End::timeout;
        return std::unique_ptr<Transport>(std::move(transport));
    };

    ConnectionCoordinator coordinator(config, transport_factory, tunnel_factory);
    coordinator.connect();
    const bool reached = wait_until(coordinator, [](const Snapshot& s) {
        return s.state == CoordinatorState::connected;
    });
    check(reached, "ssh path reaches connected");
    const Snapshot snap = coordinator.snapshot();
    check(diag_has(snap.diag, "idle -> starting_tunnel"), "ssh enters starting_tunnel");
    check(diag_has(snap.diag, "starting_tunnel -> waiting_for_forward"),
          "then waiting_for_forward");
    check(tunnel_state.start_calls == 1, "the tunnel was started once");
    check(tunnel_state.cookie_calls == 1, "the cookie was fetched over ssh");
    check(seen_options.host == "127.0.0.1", "ssh transport targets the local forward");
    check(seen_options.port == 15901, "ssh transport uses the tunnel's local port");
    check(seen_options.cookie == "boot-cookie", "ssh transport uses the fetched cookie");
    check(tunnel_state.last_config.host == "example.invalid",
          "the tunnel config carried the profile host");

    coordinator.disconnect();
    wait_until(coordinator, [](const Snapshot& s) { return s.finished; });
    check(tunnel_state.stop_calls >= 1, "the tunnel is stopped on teardown");
}

void test_ssh_tunnel_failure_is_classified_and_reported()
{
    CoordinatorConfig config;
    config.profile = ssh_profile();

    FakeTunnelState tunnel_state;
    auto tunnel_factory = [&](const TunnelConfig&, const TunnelOptions&) {
        TunnelStatus status{TunnelState::failed, 0,
                            "ssh exited (255): Permission denied (publickey)."};
        return std::unique_ptr<ISshTunnel>(std::make_unique<FakeTunnel>(
            &tunnel_state, status,
            "baron@example.invalid: Permission denied (publickey).",
            std::nullopt));
    };
    auto transport_factory = [&](const TransportOptions&, std::string&) {
        // Should never be reached: the tunnel fails first.
        return std::unique_ptr<Transport>(std::make_unique<FakeTransport>());
    };

    ConnectionCoordinator coordinator(config, transport_factory, tunnel_factory);
    coordinator.connect();
    wait_until(coordinator, [](const Snapshot& s) { return s.finished; });
    const Snapshot snap = coordinator.snapshot();
    check(snap.state == CoordinatorState::failed, "an auth failure fails the connection");
    check(snap.reason == ConnectionReason::auth_failed,
          "ssh Permission denied -> auth-failed");
    check(tunnel_state.cookie_calls == 0, "no cookie fetch after a tunnel failure");
}

// ---- reconnect sequencing ----

void test_transport_drop_triggers_a_bounded_reconnect()
{
    CoordinatorConfig config;
    config.profile = direct_profile();
    config.cookie = "c";
    config.reconnect.enabled = true;
    config.reconnect.max_attempts = 3;
    config.reconnect.base_backoff = 5ms;
    config.reconnect.max_backoff = 10ms;

    int built = 0;
    auto transport_factory = [&](const TransportOptions&, std::string&) {
        auto transport = std::make_unique<FakeTransport>();
        transport->to_deliver.push_back(one_message());
        // The first connection drops cleanly mid-session (a torn-down tunnel);
        // the second stays up.
        transport->end =
            built == 0 ? FakeTransport::End::clean_fin : FakeTransport::End::timeout;
        ++built;
        return std::unique_ptr<Transport>(std::move(transport));
    };

    ConnectionCoordinator coordinator(config, transport_factory);
    coordinator.connect();
    const bool reconnected = wait_until(coordinator, [](const Snapshot& s) {
        return s.state == CoordinatorState::connected && s.reconnect_attempt >= 1;
    });
    check(reconnected, "a clean mid-session drop reconnects and reaches connected again");
    check(built >= 2, "a fresh transport was built for the reconnect");

    coordinator.disconnect();
    wait_until(coordinator, [](const Snapshot& s) { return s.finished; });
}

void test_reset_is_not_retried()
{
    CoordinatorConfig config;
    config.profile = direct_profile();
    config.cookie = "c";
    config.reconnect.enabled = true;
    config.reconnect.max_attempts = 3;
    config.reconnect.base_backoff = 5ms;

    int built = 0;
    auto transport_factory = [&](const TransportOptions&, std::string&) {
        auto transport = std::make_unique<FakeTransport>();
        transport->to_deliver.push_back(one_message());
        transport->end = FakeTransport::End::reset; // eviction/refusal signature
        ++built;
        return std::unique_ptr<Transport>(std::move(transport));
    };

    ConnectionCoordinator coordinator(config, transport_factory);
    coordinator.connect();
    wait_until(coordinator, [](const Snapshot& s) { return s.finished; });
    const Snapshot snap = coordinator.snapshot();
    check(built == 1, "a reset is not retried, so only one transport is built");
    check(snap.state == CoordinatorState::failed, "a reset ends in failed");
    check(snap.reason == ConnectionReason::remote_closed, "a reset -> remote-closed");
}

// ---- application state mapping ----

void test_app_mode_mapping_covers_every_state()
{
    check(app_mode_for(CoordinatorState::idle, false) == AppMode::Connecting, "a1");
    check(app_mode_for(CoordinatorState::starting_tunnel, false) == AppMode::Connecting, "a2");
    check(app_mode_for(CoordinatorState::connected, false) == AppMode::Connected, "a3");
    check(app_mode_for(CoordinatorState::failed, false) == AppMode::Failed, "a4");
    check(app_mode_for(CoordinatorState::disconnecting, false) == AppMode::Connecting,
          "an in-flight disconnect is still connecting");
    check(app_mode_for(CoordinatorState::disconnecting, true) == AppMode::Library,
          "a finished disconnect returns to the library");
}

// ---- launch options ----

void test_no_args_opens_the_library()
{
    char arg0[] = "haiku-remote-gui";
    char* argv[] = {arg0};
    const LaunchResult result = parse_launch_options(1, argv);
    check(result.ok, "no args parses ok");
    check(result.options.open_library, "no args opens the library");
}

void test_direct_host_builds_an_ephemeral_direct_profile()
{
    char a0[] = "gui", a1[] = "--host", a2[] = "10.0.0.5", a3[] = "--port", a4[] = "20900";
    char* argv[] = {a0, a1, a2, a3, a4};
    const LaunchResult result = parse_launch_options(5, argv);
    check(result.ok, "direct flags parse ok");
    check(!result.options.open_library, "an endpoint flag bypasses the library");
    check(result.options.profile.mode == ConnectionMode::direct, "mode is direct");
    check(result.options.profile.host == "10.0.0.5", "host carried into the profile");
    check(result.options.profile.remote_port == 20900, "port carried as the remote port");
}

void test_ssh_flags_build_an_ephemeral_ssh_profile()
{
    char a0[] = "gui", a1[] = "--ssh-host", a2[] = "ec2.example", a3[] = "--ssh-user",
         a4[] = "ec2-user", a5[] = "--identity", a6[] = "~/.ssh/id", a7[] = "--local-port",
         a8[] = "19000";
    char* argv[] = {a0, a1, a2, a3, a4, a5, a6, a7, a8};
    const LaunchResult result = parse_launch_options(9, argv);
    check(result.ok, "ssh flags parse ok");
    check(result.options.profile.mode == ConnectionMode::ssh, "mode is ssh");
    check(result.options.profile.host == "ec2.example", "ssh host carried");
    check(result.options.profile.ssh_user == "ec2-user", "ssh user carried");
    check(result.options.profile.identity_file == "~/.ssh/id",
          "identity path carried verbatim (tilde kept)");
    check(result.options.profile.local_port.has_value()
              && *result.options.profile.local_port == 19000,
          "local port carried");
}

void test_direct_and_ssh_host_conflict_is_rejected()
{
    char a0[] = "gui", a1[] = "--direct", a2[] = "--ssh-host", a3[] = "h";
    char* argv[] = {a0, a1, a2, a3};
    const LaunchResult result = parse_launch_options(4, argv);
    check(!result.ok, "--direct with --ssh-host is rejected");
    check(result.exit_code == exit_status::usage, "a conflict is a usage error");
}

void test_invalid_port_is_a_usage_error()
{
    char a0[] = "gui", a1[] = "--host", a2[] = "h", a3[] = "--port", a4[] = "70000";
    char* argv[] = {a0, a1, a2, a3, a4};
    const LaunchResult result = parse_launch_options(5, argv);
    check(!result.ok, "an out-of-range port is rejected");
    check(result.exit_code == exit_status::usage, "and is a usage error");
}

void test_coordinator_config_from_carries_cookie_and_reconnect()
{
    char a0[] = "gui", a1[] = "--host", a2[] = "h", a3[] = "--cookie", a4[] = "ck",
         a5[] = "--reconnect";
    char* argv[] = {a0, a1, a2, a3, a4, a5};
    const LaunchResult result = parse_launch_options(6, argv);
    check(result.ok, "flags parse ok");
    const CoordinatorConfig config = coordinator_config_from(result.options);
    check(config.cookie == "ck", "the cookie is carried onto the coordinator config");
    check(config.reconnect.enabled, "--reconnect enables the policy");
    check(config.profile.mode == ConnectionMode::direct, "profile mode preserved");
    check(!config.transport_override.has_value(), "no url -> no transport override");
}

} // namespace

int main()
{
    test_ssh_failure_classification_is_exact();
    test_reason_codes_are_the_actionable_set();

    test_direct_connect_reaches_connected_with_expected_transitions();
    test_handshake_timeout_fails_no_rp_init_ack();
    test_handshake_with_undecodable_bytes_fails_decode();

    test_ssh_path_brings_up_tunnel_then_connects();
    test_ssh_tunnel_failure_is_classified_and_reported();

    test_transport_drop_triggers_a_bounded_reconnect();
    test_reset_is_not_retried();

    test_app_mode_mapping_covers_every_state();

    test_no_args_opens_the_library();
    test_direct_host_builds_an_ephemeral_direct_profile();
    test_ssh_flags_build_an_ephemeral_ssh_profile();
    test_direct_and_ssh_host_conflict_is_rejected();
    test_invalid_port_is_a_usage_error();
    test_coordinator_config_from_carries_cookie_and_reconnect();

    if (failures == 0) {
        std::cout << "PASS - " << checks << " coordinator checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " coordinator checks failed\n";
    return 1;
}
