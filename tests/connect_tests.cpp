// Tests for DeBeOS-RDP issue #1 Part 3: connection progress + actionable errors.
//
// Covered here:
//   - ConnectFlow builds the right stage checklist from a plan (per mode),
//   - its step/phase/transport/connected/fail transitions move the right stages
//     through pending -> active -> done/failed/skipped, including the visible
//     broker->tunnel fallback of direct/auto mode,
//   - classify_connect_failure() maps the information the transport /
//     managed_transport layers surface onto the right actionable cause, message
//     and Back/Retry affordance -- and does NOT claim a distinction the layer
//     cannot make (the SSH collapse),
//   - connect_with_progress() drives a failing connect through a fake command
//     runner and returns with the error set and the flow in `failed` -- the
//     "return to the library with last_error, not a black window" guarantee,
//   - and the progress and error screens render headless to PNG.
//
// House rules (CLAUDE.md): assert exact values, keep expectations independent of
// the code that produces them.

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/connect_screen.hpp"
#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/png_writer.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/quic.hpp" // HAIKU_REMOTE_HAVE_QUIC

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef HAIKU_REMOTE_HAVE_QUIC
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#endif

using namespace haiku_remote;

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

bool has(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

ConnectionProfile make_profile(ConnectionMode mode, std::string host)
{
    ConnectionProfile p;
    p.name = "Target";
    p.host = std::move(host);
    p.mode = mode;
    p.remote_port = 10900;
    p.ssh_user = "baron";
    p.ssh_port = 22;
    return p;
}

// Count stages of a given state.
std::size_t count_state(const ConnectFlow& flow, StageState state)
{
    std::size_t n = 0;
    for (const auto& s : flow.stages())
        if (s.state == state)
            ++n;
    return n;
}

std::optional<StageState> state_of(const ConnectFlow& flow, StageKind kind)
{
    for (const auto& s : flow.stages())
        if (s.kind == kind)
            return s.state;
    return std::nullopt;
}

// The state of one step's stage, for plans that reach the broker twice.
std::optional<StageState> state_at(const ConnectFlow& flow, std::size_t step,
                                   StageKind kind)
{
    for (const auto& s : flow.stages())
        if (s.step_index == step && s.kind == kind)
            return s.state;
    return std::nullopt;
}

// Auto mode's plan is [quic, wss, tunnel] in a build with QUIC and
// [wss, tunnel] without; these say where each route sits.
#ifdef HAIKU_REMOTE_HAVE_QUIC
constexpr std::size_t auto_wss_step = 1;
#else
constexpr std::size_t auto_wss_step = 0;
#endif
constexpr std::size_t auto_tunnel_step = auto_wss_step + 1;

// Drives every broker step of an auto plan to a failed credential fetch.
void fail_auto_broker_steps(ConnectFlow& flow)
{
    for (std::size_t step = 0; step < auto_tunnel_step; ++step) {
        flow.begin_step(step, RouteKind::broker);
        flow.phase(step, ConnectPhase::contacting_broker);
        flow.step_failed(step, RouteKind::broker, "broker returned no token");
    }
}

class FakeRunner : public CommandRunner {
public:
    std::string stdout_text;
    int exit_code = 0;
    bool fail_to_launch = false;

    int run(const std::vector<std::string>&, std::string& out,
            std::string& error) override
    {
        if (fail_to_launch) {
            error = "fake: could not launch";
            return -1;
        }
        out = stdout_text;
        return exit_code;
    }
};

// -------------------------------------------------------------------------
// Stage model
// -------------------------------------------------------------------------

void test_flow_stage_model()
{
    const LaunchPlan wss = plan_launch(make_profile(ConnectionMode::wss, "h"));
    ConnectFlow wss_flow(wss);
    check(wss_flow.stages().size() == 2,
          "a broker-only plan has two stages (contact, authenticate)");
    check(state_of(wss_flow, StageKind::contacting_broker).has_value(),
          "the broker plan has a contacting-broker stage");
    check(state_of(wss_flow, StageKind::authenticating).has_value(),
          "the broker plan has an authenticating stage");
    check(wss_flow.state() == FlowState::connecting,
          "a fresh flow is in the connecting state");
    check(!wss_flow.active_stage().has_value(),
          "nothing is active until the first step begins");

    const LaunchPlan ssh = plan_launch(make_profile(ConnectionMode::ssh, "h"));
    ConnectFlow ssh_flow(ssh);
    check(ssh_flow.stages().size() == 3,
          "a tunnel-only plan has three stages (cookie, tunnel, connect)");
    check(state_of(ssh_flow, StageKind::fetching_cookie).has_value()
              && state_of(ssh_flow, StageKind::opening_tunnel).has_value()
              && state_of(ssh_flow, StageKind::connecting).has_value(),
          "the tunnel plan has cookie, tunnel and connect stages");

    const LaunchPlan direct =
        plan_launch(make_profile(ConnectionMode::direct, "h"));
    ConnectFlow direct_flow(direct);
    check(direct_flow.stages().size() == 2 * auto_tunnel_step + 3,
          "a direct/auto plan has two stages per broker route then three tunnel");
#ifdef HAIKU_REMOTE_HAVE_QUIC
    check(direct_flow.stages()[0].label == "Contacting broker (QUIC)"
              && direct_flow.stages()[2].label == "Contacting broker",
          "auto's two broker routes read as QUIC, then plain (wss)");
#endif
    check(direct_flow.stages().front().route == RouteKind::broker,
          "the direct plan tries the broker stages first");
    check(direct_flow.stages().back().route == RouteKind::tunnel,
          "the direct plan falls back to the tunnel stages");
    check(!direct_flow.fell_back(), "no fallback has happened yet");

    // A QUIC broker route has the broker's two stages, labelled as QUIC.
    ConnectFlow quic_flow(plan_launch(make_profile(ConnectionMode::quic, "h")));
    check(quic_flow.stages().size() == 2
              && quic_flow.stages()[0].label == "Contacting broker (QUIC)"
              && quic_flow.stages()[1].label == "Authenticating with broker (QUIC)",
          "a quic plan has the broker's two stages, labelled as QUIC");
}

// -------------------------------------------------------------------------
// Happy-path transitions (broker step up, then connected)
// -------------------------------------------------------------------------

void test_flow_connect_transitions()
{
    const LaunchPlan plan = plan_launch(make_profile(ConnectionMode::wss, "h"));
    ConnectFlow flow(plan);

    flow.begin_step(0, RouteKind::broker);
    flow.phase(0, ConnectPhase::contacting_broker);
    check(state_of(flow, StageKind::contacting_broker) == StageState::active,
          "contacting-broker goes active when its phase begins");
    const auto active = flow.active_stage();
    check(active.has_value() && flow.stages()[*active].kind
                                    == StageKind::contacting_broker,
          "active_stage points at the running stage");

    flow.begin_transport(0, RouteKind::broker);
    check(state_of(flow, StageKind::contacting_broker) == StageState::done,
          "beginning the transport marks the substrate stage done");
    check(state_of(flow, StageKind::authenticating) == StageState::active,
          "the authenticating stage goes active for the transport connect");

    flow.connected();
    check(flow.state() == FlowState::connected, "connected() reaches connected");
    check(state_of(flow, StageKind::authenticating) == StageState::done,
          "the final stage is marked done on success");
    check(count_state(flow, StageState::failed) == 0,
          "a clean connect fails no stage");
}

// -------------------------------------------------------------------------
// The broker->tunnel fallback, as a visible state change
// -------------------------------------------------------------------------

void test_flow_fallback()
{
    const LaunchPlan plan =
        plan_launch(make_profile(ConnectionMode::direct, "h"));
    ConnectFlow flow(plan);

    fail_auto_broker_steps(flow);
    check(state_of(flow, StageKind::contacting_broker) == StageState::failed,
          "the broker contact stage is marked failed");
    check(state_of(flow, StageKind::authenticating) == StageState::skipped,
          "the broker's later stage is skipped once the step failed");
    check(state_at(flow, auto_wss_step, StageKind::contacting_broker)
              == StageState::failed,
          "every broker route's failure is shown");
    check(flow.state() == FlowState::connecting,
          "a failed first step does not end the flow -- the fallback remains");

    const std::size_t tunnel = auto_tunnel_step;
    flow.begin_step(tunnel, RouteKind::tunnel);
    check(flow.fell_back(),
          "beginning the second step records the broker->tunnel fallback");
    flow.phase(tunnel, ConnectPhase::fetching_cookie);
    check(state_of(flow, StageKind::fetching_cookie) == StageState::active,
          "the tunnel's first stage lights up after the fallback");
    flow.phase(tunnel, ConnectPhase::opening_tunnel);
    check(state_of(flow, StageKind::fetching_cookie) == StageState::done,
          "fetching the cookie is done once the tunnel is opening");
    flow.begin_transport(tunnel, RouteKind::tunnel);
    flow.connected();
    check(flow.state() == FlowState::connected,
          "the fallback route connects");
    // The failed broker stage survives into the connected state as a record.
    check(state_of(flow, StageKind::contacting_broker) == StageState::failed,
          "the broker failure stays visible after the tunnel connects");
}

// -------------------------------------------------------------------------
// cause -> message/affordance table
// -------------------------------------------------------------------------

void expect_cause(ConnectFailPoint where, ConnectFailure cf,
                  const std::string& message, ConnectErrorCause want,
                  bool want_retry, std::string_view note)
{
    const ConnectError e = classify_connect_failure(where, cf, message);
    check(e.cause == want, note);
    check(e.can_retry == want_retry,
          std::string(note) + " (retry affordance)");
    check(!e.title.empty(), std::string(note) + " (has a title)");
    check(e.raw == message, std::string(note) + " (keeps the raw message)");
}

void test_classification()
{
    // Missing credential, both transports: refused before any socket, so it is
    // retryable once the credential is supplied upstream.
    expect_cause(ConnectFailPoint::transport_broker,
                 ConnectFailure::missing_credential, "no authentication token",
                 ConnectErrorCause::missing_credential, true,
                 "a missing broker token classifies as missing_credential");
    expect_cause(ConnectFailPoint::transport_tunnel,
                 ConnectFailure::missing_credential, "no session cookie",
                 ConnectErrorCause::missing_credential, true,
                 "a missing session cookie classifies as missing_credential");

    // SSH collapses key/host/DNS: an ssh exit is one honest bucket.
    expect_cause(ConnectFailPoint::substrate_broker, ConnectFailure::none,
                 "ssh to graviton exited 255", ConnectErrorCause::ssh_auth_or_host,
                 true, "an ssh exit over the broker route is ssh_auth_or_host");
    expect_cause(ConnectFailPoint::substrate_tunnel, ConnectFailure::none,
                 "ssh to graviton exited 255", ConnectErrorCause::ssh_auth_or_host,
                 true, "an ssh exit over the tunnel route is ssh_auth_or_host");

    // The forward failing (busy local port / dead remote port / bad key) is the
    // same honest bucket, named for the forward.
    expect_cause(ConnectFailPoint::substrate_tunnel, ConnectFailure::none,
                 "ssh exited before the forward on 127.0.0.1:52000 came up",
                 ConnectErrorCause::ssh_auth_or_host, true,
                 "a failed -L forward classifies as ssh_auth_or_host");

    // A timeout IS separable from an ssh exit: a black-hole host.
    expect_cause(ConnectFailPoint::substrate_broker, ConnectFailure::none,
                 "command timed out after 20s", ConnectErrorCause::host_unreachable,
                 true, "a connect timeout classifies as host_unreachable");

    // The broker returning no token/cert means it is not up on the host.
    expect_cause(ConnectFailPoint::substrate_broker, ConnectFailure::none,
                 "broker returned no token (is remote_broker present...)",
                 ConnectErrorCause::broker_unavailable, true,
                 "no broker token/cert classifies as broker_unavailable");

    // Missing/stale cookie on the tunnel substrate.
    expect_cause(ConnectFailPoint::substrate_tunnel, ConnectFailure::none,
                 "could not read session_cookie.10900 (is app_server up?)",
                 ConnectErrorCause::cookie_unavailable, true,
                 "an unreadable session cookie classifies as cookie_unavailable");

    // Broker TLS: a pin mismatch is terminal (Retry pins the same mismatch).
    expect_cause(ConnectFailPoint::transport_broker, ConnectFailure::other,
                 "certificate pin mismatch: server fingerprint sha256:ab",
                 ConnectErrorCause::broker_cert_pin, false,
                 "a pin mismatch classifies as broker_cert_pin and is NOT retryable");
    expect_cause(ConnectFailPoint::transport_broker, ConnectFailure::other,
                 "TLS handshake with broker failed",
                 ConnectErrorCause::broker_cert_untrusted, true,
                 "a TLS verification failure classifies as broker_cert_untrusted");
    expect_cause(ConnectFailPoint::transport_broker, ConnectFailure::other,
                 "broker denied the authentication token",
                 ConnectErrorCause::broker_token_denied, true,
                 "a denied token classifies as broker_token_denied");

    // Dead/absent listener, seen two ways: behind the broker, and on the tunnel.
    expect_cause(ConnectFailPoint::transport_broker, ConnectFailure::other,
                 "broker has no session to attach (the remote interface is not"
                 " reachable behind it)",
                 ConnectErrorCause::server_listener, true,
                 "no session behind the broker classifies as server_listener");
    expect_cause(ConnectFailPoint::transport_broker, ConnectFailure::other,
                 "broker accepted the token but could not read app_server's"
                 " session cookie",
                 ConnectErrorCause::server_listener, true,
                 "broker status 3 (no readable cookie) classifies as server_listener");
    expect_cause(ConnectFailPoint::transport_tunnel, ConnectFailure::other,
                 "connect to 127.0.0.1:52000 failed",
                 ConnectErrorCause::server_listener, true,
                 "a refused session port classifies as server_listener");

    // Post-connect refusal (stale cookie / dead listener on the tunnel path).
    expect_cause(ConnectFailPoint::session_refused, ConnectFailure::none,
                 "the server closed the connection before sending any drawing",
                 ConnectErrorCause::session_refused, true,
                 "a 0-message orderly close classifies as session_refused");

    // The summary() line is short and names the fix.
    const ConnectError pin = classify_connect_failure(
        ConnectFailPoint::transport_broker, ConnectFailure::other,
        "certificate pin mismatch: x");
    check(has(pin.summary(), "Broker certificate changed"),
          "summary() leads with the title");
}

// -------------------------------------------------------------------------
// connect_with_progress: a failing connect ends with the error set, not black
// -------------------------------------------------------------------------

void test_connect_with_progress_failure()
{
    // wss mode, ssh to the broker host fails: deterministic, no socket, no child.
    ConnectionProfile wss = make_profile(ConnectionMode::wss, "graviton.example");
    const LaunchPlan plan = plan_launch(wss);
    ConnectFlow flow(plan);
    FakeRunner runner;
    runner.exit_code = 255; // ssh to the broker failed

    int ticks = 0;
    ConnectResult result =
        connect_with_progress(wss, flow, [&] { ++ticks; }, &runner);

    check(!result.ok, "a failing connect returns ok == false");
    check(result.transport == nullptr,
          "no transport is handed back on failure");
    check(flow.state() == FlowState::failed,
          "the flow ends in the failed state -- the UI shows an error, not black");
    check(result.error.cause == ConnectErrorCause::ssh_auth_or_host,
          "the broker ssh failure is classified for the user");
    check(!result.error.summary().empty(),
          "the error has a one-line summary for the library card");
    check(ticks > 0, "the progress hook was called at least once");
    check(state_of(flow, StageKind::contacting_broker) == StageState::failed,
          "the broker contact stage is the one shown failed");

    // tunnel mode, the cookie cannot be read: also deterministic (no ssh child,
    // because the cookie fetch fails before the forward is spawned).
    ConnectionProfile ssh = make_profile(ConnectionMode::ssh, "graviton.example");
    const LaunchPlan tplan = plan_launch(ssh);
    ConnectFlow tflow(tplan);
    FakeRunner empty;
    empty.stdout_text = "\n"; // an empty cookie is rejected
    ConnectResult tr = connect_with_progress(ssh, tflow, [] {}, &empty);
    check(!tr.ok, "a tunnel connect with no readable cookie fails");
    check(tr.error.cause == ConnectErrorCause::cookie_unavailable,
          "the unreadable cookie is classified as cookie_unavailable");
    check(tflow.state() == FlowState::failed, "the tunnel flow ends failed");
}

// Answers each run() from a script, in order, and counts the calls.
class ScriptedRunner : public CommandRunner {
public:
    struct Reply {
        std::string out;
        int exit_code = 0;
    };
    std::vector<Reply> replies;
    std::size_t calls = 0;

    int run(const std::vector<std::string>&, std::string& out,
            std::string&) override
    {
        const Reply reply = calls < replies.size() ? replies[calls] : Reply {"", 255};
        ++calls;
        out = reply.out;
        return reply.exit_code;
    }
};

void test_broker_that_cannot_be_reached_falls_back_to_the_tunnel()
{
    // Auto mode: the broker's token arrives over SSH, so the broker route's
    // substrate is "up" -- and then the wss connect itself fails (here: nothing
    // listens on 127.0.0.1's broker port; on a real host a blocked port or a
    // stopped remote_broker). The plan's next route, the tunnel, must still be
    // tried. It used to end the attempt at the broker (found on Windows with
    // outbound 10902 blocked, issue #1 validation).
    ConnectionProfile p = make_profile(ConnectionMode::direct, "127.0.0.1");
    const LaunchPlan plan = plan_launch(p);
    check(plan.steps.size() == auto_tunnel_step + 1
              && plan.steps[auto_wss_step].kind == RouteKind::broker
              && plan.steps[auto_tunnel_step].kind == RouteKind::tunnel,
          "auto mode plans broker then tunnel");
    ConnectFlow flow(plan);
    ScriptedRunner runner;
    // 1: the broker credential fetch succeeds: a token, the marker, and a
    //    certificate block (unparsable, so it is left to first-use checking --
    //    never reached, the connect is refused first).
    // 2: the tunnel's cookie fetch returns an empty cookie, so the tunnel fails
    //    at the cookie -- deterministic, and no ssh child is ever spawned.
    runner.replies = {{"broker-token\n===BROKER_PEM===\n"
                       "-----BEGIN CERTIFICATE-----\nnot-a-real-cert\n"
                       "-----END CERTIFICATE-----\n",
                       0},
                      {"\n", 0}};
    ConnectResult result = connect_with_progress(p, flow, [] {}, &runner);

    check(!result.ok, "neither route connects in this scripted run");
    check(runner.calls == 2,
          "after the broker connect fails, the tunnel's cookie fetch runs"
          " (2 commands, not 1)");
    check(flow.fell_back(), "the flow records the broker->tunnel fallback");
    check(state_at(flow, auto_wss_step, StageKind::contacting_broker)
              == StageState::done,
          "the broker's credential fetch succeeded (the substrate came up)");
    check(state_at(flow, auto_wss_step, StageKind::authenticating)
              == StageState::failed,
          "the broker's connect stage is the one shown failed");
    check(state_of(flow, StageKind::fetching_cookie) == StageState::failed,
          "the tunnel's cookie stage ran and is the one that failed last");
    check(result.error.cause == ConnectErrorCause::cookie_unavailable,
          "the reported error is the tunnel's, the last route tried");
}

// -------------------------------------------------------------------------
// QUIC in auto mode: only where the broker advertises it, and a visible
// fallback to wss when it is not there or UDP is blocked (DeBeOS #766)
// -------------------------------------------------------------------------

// What the broker's SSH round trip prints: token, marker, PEM, and -- from a
// broker that writes one -- the QUIC marker and broker.quic.
const std::string fetched_pem = "-----BEGIN CERTIFICATE-----\nnot-a-real-cert\n"
                                "-----END CERTIFICATE-----";

void test_quic_advertisement_is_parsed()
{
    FakeRunner runner;
    SshConfig cfg;
    cfg.host = "h";
    BrokerCredentials creds;
    std::string error;
    runner.stdout_text = "tok\n===BROKER_PEM===\n" + fetched_pem
                         + "\n===BROKER_QUIC===\ndebeos-urp/1\ndebeos-urp/2\n";
    check(fetch_broker_credentials(runner, cfg, default_broker_port, creds, error)
              && creds.token == "tok" && creds.cert_pem == fetched_pem
              && creds.quic_alpns
                     == std::vector<std::string> {"debeos-urp/1", "debeos-urp/2"},
          "broker.quic is read as one ALPN per line, and the PEM stops at its"
          " marker");
    check(creds.offers_quic("debeos-urp/1") && !creds.offers_quic("debeos-urp/3"),
          "offers_quic() matches an advertised ALPN exactly");

    runner.stdout_text = "tok\n===BROKER_PEM===\n" + fetched_pem
                         + "\n===BROKER_QUIC===\n";
    check(fetch_broker_credentials(runner, cfg, default_broker_port, creds, error)
              && creds.quic_alpns.empty() && creds.cert_pem == fetched_pem,
          "no broker.quic (a broker without -q) offers no QUIC");

    runner.stdout_text = "tok\n===BROKER_PEM===\n" + fetched_pem + "\n";
    check(fetch_broker_credentials(runner, cfg, default_broker_port, creds, error)
              && creds.quic_alpns.empty() && creds.cert_pem == fetched_pem,
          "an older reply with no QUIC marker at all offers no QUIC either");
}

#ifdef HAIKU_REMOTE_HAVE_QUIC
#if defined(_WIN32)
using socket_t = SOCKET;
inline void close_socket(socket_t fd) { ::closesocket(fd); }
#else
using socket_t = int;
inline void close_socket(socket_t fd) { ::close(fd); }
#endif

class RecordingObserver : public ConnectObserver {
public:
    std::vector<std::pair<std::size_t, std::string>> failed;
    void on_step_failed(std::size_t index, RouteKind, const std::string& error) override
    {
        failed.emplace_back(index, error);
    }
};

void test_auto_skips_quic_the_broker_does_not_offer()
{
    // The broker runs without -q, so its reply carries no debeos-urp/1: the
    // QUIC route fails at once, visibly, and the wss route goes ahead on the
    // same credentials -- one SSH round trip, no UDP packet, no give-up wait.
    ConnectionProfile p = make_profile(ConnectionMode::direct, "h");
    ScriptedRunner runner;
    runner.replies = {{"tok\n===BROKER_PEM===\n" + fetched_pem
                           + "\n===BROKER_QUIC===\n",
                       0}};
    RecordingObserver observer;
    const ManagedConnection conn = open_connection(p, &runner, &observer);
    check(conn.ok && conn.kind == RouteKind::broker
              && conn.transport.url == "wss://h:10902" && conn.transport.token == "tok",
          "auto falls through to wss when the broker offers no QUIC");
    check(runner.calls == 1, "both broker routes share one credential fetch");
    check(observer.failed.size() == 1 && observer.failed[0].first == 0
              && observer.failed[0].second
                     == "the broker does not offer QUIC (no debeos-urp/1 in its"
                        " broker.quic: it is not running with -q)",
          "the QUIC route is the one shown failed, and says why");

    // And one that does offer it gets the QUIC route.
    runner.calls = 0;
    runner.replies = {{"tok\n===BROKER_PEM===\n" + fetched_pem
                           + "\n===BROKER_QUIC===\ndebeos-urp/1\n",
                       0}};
    RecordingObserver offered;
    const ManagedConnection quic = open_connection(p, &runner, &offered);
    check(quic.ok && quic.transport.url == "quic://h:10902"
              && quic.transport.quic_handshake_timeout_ms == quic_give_up_ms
              && offered.failed.empty(),
          "a broker that advertises debeos-urp/1 gets the QUIC route first");
}

void test_blocked_udp_falls_back_to_wss()
{
    // The broker advertises QUIC but its UDP never answers -- a firewall that
    // drops, the commonest way QUIC fails in the field. A local socket bound
    // to the broker port and never read stands in for it. Auto mode must give
    // up on QUIC after quic_give_up_ms and go on to wss (refused here: nothing
    // listens on TCP 10902), then the tunnel.
#if defined(_WIN32)
    WSADATA wsa {};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const socket_t black_hole = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(default_broker_port);
    if (::bind(black_hole, reinterpret_cast<sockaddr*>(&address), sizeof(address))
        != 0) {
        std::cerr << "SKIP: blocked-UDP fallback (UDP 127.0.0.1:10902 is in use)\n";
        close_socket(black_hole);
        return;
    }

    ConnectionProfile p = make_profile(ConnectionMode::direct, "127.0.0.1");
    ConnectFlow flow(plan_launch(p));
    ScriptedRunner runner;
    const ScriptedRunner::Reply offers_quic {
        "tok\n===BROKER_PEM===\n" + fetched_pem + "\n===BROKER_QUIC===\ndebeos-urp/1\n",
        0};
    runner.replies = {offers_quic, offers_quic, {"\n", 0}};
    const auto start = std::chrono::steady_clock::now();
    ConnectResult result = connect_with_progress(p, flow, [] {}, &runner);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    close_socket(black_hole);

    check(!result.ok && runner.calls == 3,
          "QUIC, then wss, then the tunnel's cookie fetch were all tried");
    check(state_at(flow, 0, StageKind::authenticating) == StageState::failed
              && state_at(flow, auto_wss_step, StageKind::authenticating)
                     == StageState::failed
              && state_at(flow, auto_tunnel_step, StageKind::fetching_cookie)
                     == StageState::failed,
          "the progress screen shows QUIC failed, then wss, then the tunnel");
    check(flow.fell_back(), "the flow records the fallback");
    check(elapsed >= quic_give_up_ms && elapsed < quic_give_up_ms + 2000,
          "QUIC was given up after quic_give_up_ms, not the transport's 10 s ("
              + std::to_string(elapsed) + " ms)");
}
#endif

// -------------------------------------------------------------------------
// ConnectScreen: error affordances + headless render to PNG
// -------------------------------------------------------------------------

std::optional<ConnectScreen::Region>
find_region(const ConnectScreen& screen, ConnectScreen::Region::Kind kind)
{
    for (const auto& r : screen.regions())
        if (r.kind == kind)
            return r;
    return std::nullopt;
}

std::filesystem::path output_dir()
{
    const char* tmp = std::getenv("TMPDIR");
    std::filesystem::path base = tmp && *tmp ? tmp : "/tmp";
    base /= "debeos-rdp-part3";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
}

void test_screen_render_and_actions()
{
    // A direct plan driven to a mid-flight fallback: the broker failed and the
    // tunnel is connecting. This is the progress screen's richest state.
    const LaunchPlan plan =
        plan_launch(make_profile(ConnectionMode::direct, "graviton.example"));
    ConnectFlow flow(plan);
    fail_auto_broker_steps(flow);
    flow.begin_step(auto_tunnel_step, RouteKind::tunnel);
    flow.phase(auto_tunnel_step, ConnectPhase::fetching_cookie);
    flow.phase(auto_tunnel_step, ConnectPhase::opening_tunnel);
    flow.begin_transport(auto_tunnel_step, RouteKind::tunnel);

    ConnectScreen progress(flow, "Graviton (us-west-2)", 900, 600);
    const Surface& psurface = progress.render();
    check(psurface.width() == 900 && psurface.height() == 600,
          "the progress screen renders at the requested size");
    check(progress.regions().empty(),
          "the progress screen offers no buttons while connecting");

    const auto dir = output_dir();
    std::string png_error;
    const std::string progress_path = (dir / "connect-progress.png").string();
    check(write_png(psurface, progress_path, png_error),
          "the progress screen writes a PNG");

    // Now an error screen with a retryable cause: Back + Retry.
    ConnectFlow efail(plan_launch(make_profile(ConnectionMode::wss, "h")));
    efail.begin_step(0, RouteKind::broker);
    efail.phase(0, ConnectPhase::contacting_broker);
    efail.begin_transport(0, RouteKind::broker);
    efail.fail(classify_connect_failure(ConnectFailPoint::transport_broker,
                                        ConnectFailure::other,
                                        "broker denied the authentication token"));
    ConnectScreen error_screen(efail, "Graviton (us-west-2)", 900, 600);
    const Surface& esurface = error_screen.render();
    const auto back = find_region(error_screen, ConnectScreen::Region::Kind::back);
    const auto retry = find_region(error_screen, ConnectScreen::Region::Kind::retry);
    check(back.has_value(), "a retryable error offers Back to library");
    check(retry.has_value(), "a retryable error offers Retry");

    // Clicking Retry is reported once.
    error_screen.pointer_press(retry->x + retry->w / 2, retry->y + retry->h / 2);
    check(error_screen.take_action() == ConnectScreen::Action::retry,
          "clicking Retry yields the retry action");
    check(error_screen.take_action() == ConnectScreen::Action::none,
          "the action is one-shot");

    const std::string error_path = (dir / "connect-error.png").string();
    check(write_png(esurface, error_path, png_error),
          "the error screen writes a PNG");

    // A terminal cause offers Back only -- Retry would pin the same mismatch.
    ConnectFlow epin(plan_launch(make_profile(ConnectionMode::wss, "h")));
    epin.begin_step(0, RouteKind::broker);
    epin.phase(0, ConnectPhase::contacting_broker);
    epin.begin_transport(0, RouteKind::broker);
    epin.fail(classify_connect_failure(ConnectFailPoint::transport_broker,
                                       ConnectFailure::other,
                                       "certificate pin mismatch: x"));
    ConnectScreen pin_screen(epin, "Graviton", 900, 600);
    pin_screen.render();
    check(find_region(pin_screen, ConnectScreen::Region::Kind::back).has_value(),
          "a non-retryable error still offers Back");
    check(!find_region(pin_screen, ConnectScreen::Region::Kind::retry).has_value(),
          "a non-retryable error offers no Retry");
    // Enter on a non-retryable error means Back, not Retry.
    pin_screen.key(ConnectScreen::Key::enter);
    check(pin_screen.take_action() == ConnectScreen::Action::back,
          "Enter on a terminal error goes Back, not Retry");

    std::cerr << "connect screenshots: " << progress_path << " , " << error_path
              << '\n';
}

} // namespace

int main()
{
    test_flow_stage_model();
    test_flow_connect_transitions();
    test_flow_fallback();
    test_classification();
    test_connect_with_progress_failure();
    test_broker_that_cannot_be_reached_falls_back_to_the_tunnel();
    test_quic_advertisement_is_parsed();
#ifdef HAIKU_REMOTE_HAVE_QUIC
    test_auto_skips_quic_the_broker_does_not_offer();
    test_blocked_udp_falls_back_to_wss();
#else
    std::cerr << "SKIP: QUIC in auto mode (built without OpenSSL 3.5)\n";
#endif
    test_screen_render_and_actions();

    if (failures == 0) {
        std::cout << "PASS - " << checks << " connect-flow checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " connect-flow checks FAILED\n";
    return 1;
}
