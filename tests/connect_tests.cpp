// Tests for DeBeOS-RDP issue #1 Part 3 / "3. Connecting state": connection
// progress, the RP handshake, cancel, actionable errors and the diagnostic log.
//
// Covered here:
//   - ConnectFlow builds the right stage checklist from a plan (per mode),
//     with issue #1's state names, and moves stages through
//     pending -> active -> done/failed/skipped, including the visible
//     broker->tunnel fallback of direct/auto mode and a cancel;
//   - classify_ssh_stderr() on stderr captured from real OpenSSH 8.7 runs, and
//     classify_connect_failure() / classify_handshake_failure() on what the
//     layers surface;
//   - DiagnosticLog's ring bound and its redaction of secrets;
//   - connect_with_progress() through a fake command runner, through a fake
//     `ssh` on PATH (so the real SystemCommandRunner, SshTunnel and
//     ManagedProcess run, and ssh's real stderr path is exercised), and against
//     an in-process RP server that answers -- or does not answer -- the
//     handshake;
//   - AsyncConnect: Cancel during a stalled tunnel and a stalled ssh command
//     leaves no process behind;
//   - and the progress and error screens' affordances, rendered headless.
//
// House rules (CLAUDE.md): assert exact values, keep expectations independent of
// the code that produces them -- the stderr samples are what ssh printed, the
// server's view of the handshake is read by the server, not by the client.

#include "haiku_remote/connect_flow.hpp"
#include "haiku_remote/connect_screen.hpp"
#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/png_writer.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/quic.hpp" // HAIKU_REMOTE_HAVE_QUIC

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(_WIN32) && defined(HAIKU_REMOTE_HAVE_QUIC)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

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

std::optional<StageState> state_of(const ConnectFlow& flow, StageKind kind,
                                   std::size_t step = 0)
{
    for (const auto& s : flow.stages())
        if (s.kind == kind && s.step_index == step)
            return s.state;
    return std::nullopt;
}

// Auto mode's plan in this build: [quic, wss, tunnel] with QUIC (#82),
// [wss, tunnel] with wss only, [tunnel] without wss; these say where each
// route sits.
constexpr std::size_t auto_wss_step = build_supports_quic ? 1 : 0;
constexpr std::size_t auto_tunnel_step = build_supports_wss ? auto_wss_step + 1 : 0;

std::vector<StageKind> kinds(const ConnectFlow& flow)
{
    std::vector<StageKind> out;
    for (const auto& s : flow.stages())
        out.push_back(s.kind);
    return out;
}

std::vector<StageState> states(const ConnectFlow& flow)
{
    std::vector<StageState> out;
    for (const auto& s : flow.stages())
        out.push_back(s.state);
    return out;
}

class FakeRunner : public CommandRunner {
public:
    std::string stdout_text;
    int exit_code = 0;
    bool fail_to_launch = false;
    int calls = 0;

    int run(const std::vector<std::string>&, std::string& out,
            std::string& error) override
    {
        ++calls;
        if (fail_to_launch) {
            error = "fake: could not launch";
            return -1;
        }
        out = stdout_text;
        return exit_code;
    }
};

using SK = StageKind;
using SS = StageState;

// -------------------------------------------------------------------------
// Stage model
// -------------------------------------------------------------------------

void test_flow_stage_model()
{
    ConnectFlow wss_flow(plan_launch(make_profile(ConnectionMode::wss, "h")));
    check(kinds(wss_flow)
              == std::vector<StageKind> {SK::contacting_broker, SK::authenticating,
                                         SK::waiting_for_protocol_handshake,
                                         SK::requesting_display},
          "a broker plan: contact, authenticate, handshake, display");
    check(wss_flow.state() == FlowState::idle, "a fresh flow is idle");
    check(issue_state_name(wss_flow) == "idle", "issue state name of a fresh flow");
    check(!wss_flow.active_stage().has_value(),
          "nothing is active until the first step begins");

    ConnectFlow ssh_flow(plan_launch(make_profile(ConnectionMode::ssh, "h")));
    check(kinds(ssh_flow)
              == std::vector<StageKind> {SK::fetching_cookie, SK::starting_tunnel,
                                         SK::waiting_for_forward,
                                         SK::connecting_transport,
                                         SK::waiting_for_protocol_handshake,
                                         SK::requesting_display},
          "a tunnel plan: cookie, tunnel, forward, transport, handshake, display");
    check(ssh_flow.stages()[1].label == "Starting SSH tunnel"
              && ssh_flow.stages()[4].label == "Waiting for the server handshake",
          "stage labels");

    // Auto mode as a build with wss plans it (the plan is pure, so this holds
    // in either build).
    ConnectFlow auto_flow(plan_launch(make_profile(ConnectionMode::automatic, "h"),
                                      /*wss_available=*/true,
                                      /*quic_available=*/false));
    std::vector<std::size_t> steps;
    for (const auto& s : auto_flow.stages())
        steps.push_back(s.step_index);
    check(steps == std::vector<std::size_t> {0, 0, 0, 0, 1, 1, 1, 1, 1, 1},
          "an auto plan: four broker stages (step 0), six tunnel (step 1)");
    check(auto_flow.stages().front().route == RouteKind::broker
              && auto_flow.stages().back().route == RouteKind::tunnel,
          "the auto plan tries the broker first and falls back to the tunnel");
    check(!auto_flow.fell_back(), "no fallback has happened yet");

    // With QUIC (#82) the broker is reached twice, over QUIC first, and the
    // QUIC run says so in every label.
    ConnectFlow quic_auto(plan_launch(make_profile(ConnectionMode::automatic, "h"),
                                      /*wss_available=*/true,
                                      /*quic_available=*/true));
    steps.clear();
    for (const auto& s : quic_auto.stages())
        steps.push_back(s.step_index);
    check(steps == std::vector<std::size_t> {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2},
          "an auto plan with QUIC: four QUIC broker stages, four wss, six tunnel");
    check(quic_auto.stages()[0].label == "Contacting broker (QUIC)"
              && quic_auto.stages()[3].label == "Requesting the display (QUIC)"
              && quic_auto.stages()[4].label == "Contacting broker",
          "auto's two broker routes read as QUIC, then plain (wss)");
    ConnectFlow quic_mode(plan_launch(make_profile(ConnectionMode::quic, "h")));
    check(quic_mode.stages().size() == 4
              && quic_mode.stages()[0].label == "Contacting broker (QUIC)",
          "a quic plan has the broker's four stages, labelled as QUIC");

    // A tunnel whose cookie the profile supplied never fetches one, so the
    // checklist does not list that stage.
    RouteStep preset;
    preset.kind = RouteKind::tunnel;
    preset.transport.cookie = "preset-cookie";
    check(route_stages(preset)
              == std::vector<StageKind> {SK::starting_tunnel,
                                         SK::waiting_for_forward,
                                         SK::connecting_transport,
                                         SK::waiting_for_protocol_handshake,
                                         SK::requesting_display},
          "a tunnel with a supplied cookie has no fetching_cookie stage");

    // A QUIC broker step is RouteKind::broker; its labels say how it travels,
    // so auto mode's two broker runs do not read as a repeat.
    LaunchPlan quic;
    RouteStep q;
    q.kind = RouteKind::broker;
    q.transport.url = "quic://h:10902";
    quic.steps = {q};
    ConnectFlow quic_flow(quic);
    check(quic_flow.stages().size() == 4
              && quic_flow.stages()[0].label == "Contacting broker (QUIC)"
              && quic_flow.stages()[1].label == "Authenticating with broker (QUIC)",
          "a quic:// broker step has the broker's stages, labelled QUIC");
}

// -------------------------------------------------------------------------
// Transitions, with issue #1's state names
// -------------------------------------------------------------------------

void test_flow_connect_transitions()
{
    ConnectFlow flow(plan_launch(make_profile(ConnectionMode::wss, "h")));
    flow.begin_step(0, RouteKind::broker);
    check(flow.state() == FlowState::connecting, "begin_step starts connecting");
    flow.phase(0, ConnectPhase::contacting_broker);
    check(issue_state_name(flow) == "contacting_broker", "contacting_broker");
    flow.begin_transport(0, RouteKind::broker);
    check(issue_state_name(flow) == "authenticating", "authenticating");
    check(states(flow) == std::vector<StageState> {SS::done, SS::active, SS::pending,
                                                   SS::pending},
          "beginning the transport completes the substrate stage");
    flow.begin_handshake(0);
    check(issue_state_name(flow) == "waiting_for_protocol_handshake",
          "waiting_for_protocol_handshake");
    flow.request_display(0);
    check(issue_state_name(flow) == "requesting_display", "requesting_display");
    flow.connected();
    check(flow.state() == FlowState::connected, "connected() reaches connected");
    check(issue_state_name(flow) == "connected", "issue state name: connected");
    check(states(flow) == std::vector<StageState>(4, SS::done),
          "every stage is done on success");

    ConnectFlow tunnel(plan_launch(make_profile(ConnectionMode::ssh, "h")));
    tunnel.begin_step(0, RouteKind::tunnel);
    tunnel.phase(0, ConnectPhase::fetching_cookie);
    tunnel.phase(0, ConnectPhase::opening_tunnel);
    check(issue_state_name(tunnel) == "starting_tunnel", "starting_tunnel");
    tunnel.phase(0, ConnectPhase::waiting_for_forward);
    check(issue_state_name(tunnel) == "waiting_for_forward", "waiting_for_forward");
    tunnel.begin_transport(0, RouteKind::tunnel);
    check(issue_state_name(tunnel) == "connecting_transport", "connecting_transport");

    // Cancel: the running stage did not fail, it was stopped.
    tunnel.begin_disconnect();
    check(tunnel.state() == FlowState::disconnecting
              && issue_state_name(tunnel) == "disconnecting",
          "a cancel is disconnecting");
    check(states(tunnel)
              == std::vector<StageState> {SS::done, SS::done, SS::done, SS::skipped,
                                          SS::skipped, SS::skipped},
          "a cancel skips the running and pending stages, fails none");
    tunnel.finish_disconnect();
    check(tunnel.state() == FlowState::idle, "a finished cancel is idle again");
}

void test_flow_fallback()
{
    ConnectFlow flow(plan_launch(make_profile(ConnectionMode::automatic, "h"),
                                 /*wss_available=*/true, /*quic_available=*/false));
    flow.begin_step(0, RouteKind::broker);
    flow.phase(0, ConnectPhase::contacting_broker);
    flow.step_failed(0, RouteKind::broker, "broker returned no token");
    check(states(flow)
              == std::vector<StageState> {SS::failed, SS::skipped, SS::skipped,
                                          SS::skipped, SS::pending, SS::pending,
                                          SS::pending, SS::pending, SS::pending,
                                          SS::pending},
          "the broker's contact stage failed, its later stages skipped");
    check(flow.state() == FlowState::connecting,
          "a failed first step does not end the flow -- the fallback remains");
    flow.begin_step(1, RouteKind::tunnel);
    check(flow.fell_back(),
          "beginning the second step records the broker->tunnel fallback");
    flow.phase(1, ConnectPhase::fetching_cookie);
    flow.phase(1, ConnectPhase::opening_tunnel);
    check(state_of(flow, SK::fetching_cookie, 1) == SS::done,
          "fetching the cookie is done once the tunnel is starting");
    flow.begin_transport(1, RouteKind::tunnel);
    flow.begin_handshake(1);
    flow.request_display(1);
    flow.connected();
    check(flow.state() == FlowState::connected, "the fallback route connects");
    check(state_of(flow, SK::contacting_broker, 0) == SS::failed,
          "the broker failure stays visible after the tunnel connects");
}

// -------------------------------------------------------------------------
// ssh stderr, captured from real OpenSSH_8.7p1 runs on Linux
// -------------------------------------------------------------------------

// Each is what `ssh -o BatchMode=yes ...` wrote to stderr, verbatim.
const std::string kRefused = "ssh: connect to host 127.0.0.1 port 1: Connection refused\n";
const std::string kDns = "ssh: Could not resolve hostname no-such-host.invalid: Name or"
                         " service not known\n";
const std::string kTimedOut =
    "ssh: connect to host 10.255.255.1 port 22: Connection timed out\n";
const std::string kAuth =
    "Warning: Permanently added '127.0.0.1' (ED25519) to the list of known hosts.\n"
    "nobody@127.0.0.1: Permission denied (publickey).\n";
const std::string kIdentityMissing =
    "Warning: Identity file /nonexistent/key not accessible: No such file or"
    " directory.\n"
    "nobody@127.0.0.1: Permission denied (publickey).\n";
const std::string kIdentityMissingRefused =
    "Warning: Identity file /nonexistent/key not accessible: No such file or"
    " directory.\n"
    "ssh: connect to host 127.0.0.1 port 1: Connection refused\n";
const std::string kBadPermissions =
    "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
    "@         WARNING: UNPROTECTED PRIVATE KEY FILE!          @\n"
    "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
    "Permissions 0644 for '/tmp/c1key' are too open.\n"
    "It is required that your private key files are NOT accessible by others.\n"
    "This private key will be ignored.\n"
    "Load key \"/tmp/c1key\": bad permissions\n"
    "nobody@127.0.0.1: Permission denied (publickey).\n";
const std::string kBadFormat = "Load key \"/tmp/c1key\": invalid format\n"
                               "nobody@127.0.0.1: Permission denied (publickey).\n";
const std::string kHostKeyChanged =
    "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
    "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\n"
    "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
    "IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!\n"
    "Someone could be eavesdropping on you right now (man-in-the-middle attack)!\n"
    "It is also possible that a host key has just been changed.\n"
    "The fingerprint for the ED25519 key sent by the remote host is\n"
    "SHA256:TloDZADegoMbaHoo/wnBKIk2fSkWjYO3A2Y/CqOw4CY.\n"
    "Please contact your system administrator.\n"
    "Add correct host key in /tmp/c1-kh-wrong to get rid of this message.\n"
    "Offending ED25519 key in /tmp/c1-kh-wrong:1\n"
    "Host key for 127.0.0.1 has changed and you have requested strict checking.\n"
    "Host key verification failed.\n";
const std::string kHostKeyUnknown =
    "No ED25519 host key is known for 127.0.0.1 and you have requested strict"
    " checking.\n"
    "Host key verification failed.\n";
const std::string kPortInUse =
    "bind [127.0.0.1]:2299: Address already in use\n"
    "channel_setup_fwd_listener_tcpip: cannot listen to port: 2299\n"
    "Could not request local forwarding.\n";
const std::string kChannelRefused =
    "channel 2: open failed: connect failed: Connection refused\n";

void test_ssh_stderr_classification()
{
    using C = ConnectErrorCause;
    const struct {
        const std::string& text;
        C want;
        const char* what;
    } cases[] = {
        {kRefused, C::ssh_refused, "refused SSH port"},
        {kDns, C::host_not_found, "unresolvable host"},
        {kTimedOut, C::host_unreachable, "black-hole host"},
        {kAuth, C::ssh_auth_failed, "rejected key"},
        {kIdentityMissing, C::identity_missing,
         "missing identity outranks the auth failure it causes"},
        {kIdentityMissingRefused, C::ssh_refused,
         "a refused connection outranks a missing-identity warning"},
        {kBadPermissions, C::identity_unreadable, "key with bad permissions"},
        {kBadFormat, C::identity_unreadable, "key in an invalid format"},
        {kHostKeyChanged, C::ssh_host_key, "changed host key"},
        {kHostKeyUnknown, C::ssh_host_key, "unknown host key, strict checking"},
        {kPortInUse, C::local_port_in_use, "busy local forward port"},
        {kChannelRefused, C::server_listener, "forward's far end refused"},
        {"Warning: Permanently added '127.0.0.1' (ED25519) to the list of known"
         " hosts.\n",
         C::none, "a harmless warning names no cause"},
        {"", C::none, "no stderr names no cause"},
    };
    for (const auto& c : cases)
        check(classify_ssh_stderr(c.text) == c.want,
              std::string("classify_ssh_stderr: ") + c.what);
}

// -------------------------------------------------------------------------
// cause -> message/affordance table
// -------------------------------------------------------------------------

void expect_cause(ConnectFailPoint where, ConnectFailure cf,
                  const std::string& message, ConnectErrorCause want,
                  bool want_retry, std::string_view note,
                  const std::string& stderr_text = {})
{
    const ConnectError e = classify_connect_failure(where, cf, message, stderr_text);
    check(e.cause == want, note);
    check(e.can_retry == want_retry, std::string(note) + " (retry affordance)");
    check(!e.title.empty(), std::string(note) + " (has a title)");
    check(e.raw == message, std::string(note) + " (keeps the raw message)");
    check(e.ssh_stderr == stderr_text, std::string(note) + " (keeps ssh's stderr)");
}

void test_classification()
{
    using C = ConnectErrorCause;
    using P = ConnectFailPoint;
    using F = ConnectFailure;
    const std::string exited = "ssh to graviton exited 255";

    expect_cause(P::transport_broker, F::missing_credential, "no authentication token",
                 C::missing_credential, true, "missing broker token");
    expect_cause(P::transport_tunnel, F::missing_credential, "no session cookie",
                 C::missing_credential, true, "missing session cookie");

    // ssh's exit status is 255 for every failure; the stderr is what tells
    // them apart.
    expect_cause(P::substrate_broker, F::none, exited, C::ssh_auth_failed, true,
                 "broker route, rejected key", kAuth);
    expect_cause(P::substrate_tunnel, F::none, exited, C::ssh_host_key, false,
                 "tunnel route, changed host key: no Retry", kHostKeyChanged);
    expect_cause(P::substrate_tunnel, F::none,
                 "ssh exited before the forward on 127.0.0.1:2299 came up",
                 C::local_port_in_use, true, "tunnel route, busy local port",
                 kPortInUse);
    expect_cause(P::substrate_broker, F::none, exited, C::host_not_found, true,
                 "broker route, unresolvable host", kDns);
    // With nothing recognisable on stderr, the honest bucket.
    expect_cause(P::substrate_broker, F::none, exited, C::ssh_failed, true,
                 "broker route, ssh failed and said nothing");
    expect_cause(P::substrate_tunnel, F::none,
                 "ssh exited before the forward on 127.0.0.1:52000 came up",
                 C::ssh_failed, true, "tunnel exit with no stderr");

    // ssh's own status is 255; anything else is the remote command's.
    expect_cause(P::substrate_tunnel, F::none, "ssh to graviton exited 1",
                 C::cookie_unavailable, true,
                 "exit 1 is the remote cookie read failing, not ssh");
    expect_cause(P::substrate_broker, F::none, "ssh to graviton exited 127",
                 C::broker_unavailable, true,
                 "exit 127 is the remote broker command missing");

    expect_cause(P::substrate_tunnel, F::none,
                 "timed out waiting for the ssh forward on 127.0.0.1:52000",
                 C::forward_timeout, true, "the forward never accepted");
    expect_cause(P::substrate_broker, F::none, "command timed out after 20s",
                 C::host_unreachable, true, "an ssh command timeout");
    expect_cause(P::substrate_broker, F::none,
                 "broker returned no token (is remote_broker present...)",
                 C::broker_unavailable, true, "no broker token/cert");
    expect_cause(P::substrate_tunnel, F::none,
                 "could not read session_cookie.10900 (is app_server up?)",
                 C::cookie_unavailable, true, "unreadable session cookie");

    expect_cause(P::preflight, F::none, "no ssh executable on PATH", C::ssh_not_found,
                 true, "preflight: no ssh");
    expect_cause(P::preflight, F::none, "identity file /k does not exist",
                 C::identity_missing, true, "preflight: missing identity");
    expect_cause(P::preflight, F::none, "identity file /k cannot be read",
                 C::identity_unreadable, true, "preflight: unreadable identity");
    expect_cause(P::preflight, F::none, "cancelled", C::cancelled, true,
                 "a cancel classifies as cancelled");

    expect_cause(P::transport_broker, F::other,
                 "certificate pin mismatch: server fingerprint sha256:ab",
                 C::broker_cert_pin, false, "pin mismatch: no Retry");
    expect_cause(P::transport_broker, F::other, "TLS handshake with broker failed",
                 C::broker_cert_untrusted, true, "TLS verification failure");
    expect_cause(P::transport_broker, F::other, "broker denied the authentication token",
                 C::broker_token_denied, true, "denied token");
    expect_cause(P::transport_broker, F::other,
                 "broker has no session to attach (the remote interface is not"
                 " reachable behind it)",
                 C::server_listener, true, "no session behind the broker");
    expect_cause(P::transport_broker, F::other,
                 "broker accepted the token but could not read app_server's"
                 " session cookie",
                 C::server_listener, true, "broker status 3");
    // TcpSocket::connect's message once every resolved address failed.
    expect_cause(P::transport_broker, F::other,
                 "could not connect to any resolved address", C::tcp_failed, true,
                 "broker port unreachable");
    expect_cause(P::transport_tunnel, F::other,
                 "could not connect to any resolved address", C::tcp_failed, true,
                 "local end of the forward refused");

    expect_cause(P::session_refused, F::none,
                 "the server closed the connection before sending any drawing",
                 C::session_refused, true, "0-message orderly close");

    const ConnectError pin = classify_connect_failure(
        P::transport_broker, F::other, "certificate pin mismatch: x");
    check(pin.summary()
              == "Broker certificate changed \xE2\x80\x94 If the broker was"
                 " reinstalled, update or clear the pinned fingerprint. Otherwise"
                 " do not trust this connection.",
          "summary() is title, dash, remedy");
}

void test_handshake_classification()
{
    using C = ConnectErrorCause;
    using H = HandshakeFailure;
    check(classify_handshake_failure(H::timeout, RouteKind::tunnel, "t").cause
              == C::handshake_timeout,
          "no echo in time: handshake_timeout");
    check(classify_handshake_failure(H::decode, RouteKind::broker, "d").cause
              == C::decode_failed,
          "undecodable stream: decode_failed");
    check(classify_handshake_failure(H::reset, RouteKind::broker, "r").cause
              == C::session_refused,
          "a reset on the broker's stream is app_server's refusal: session_refused");
    // Through ssh -L, app_server's refusal arrives as a FIN (ssh already read
    // the bytes); a reset is ssh failing to open the channel to a listener
    // that is not up, which a reconnect must retry (desktop-shell, #79).
    check(classify_handshake_failure(H::reset, RouteKind::tunnel, "r").cause
              == C::server_listener,
          "a reset through the tunnel is ssh's channel failure: server_listener");
    check(classify_handshake_failure(H::closed, RouteKind::tunnel, "c").cause
              == C::remote_closed,
          "an orderly close: remote_closed");
    check(classify_handshake_failure(H::closed, RouteKind::tunnel, "c",
                                     kChannelRefused)
                  .cause
              == C::server_listener,
          "a close after ssh could not open the channel: nothing listening");
    check(classify_handshake_failure(H::closed, RouteKind::broker, "c",
                                     kChannelRefused)
                  .cause
              == C::remote_closed,
          "the tunnel's stderr does not explain a broker close");
}

// -------------------------------------------------------------------------
// DiagnosticLog
// -------------------------------------------------------------------------

// The text after the "+S.mmms " stamp.
std::string unstamped(const std::string& line)
{
    const std::size_t space = line.find(' ');
    return space == std::string::npos ? line : line.substr(space + 1);
}

void test_diagnostic_log()
{
    DiagnosticLog log(3);
    for (const char* line : {"one", "two", "three", "four", "five"})
        log.add(line);
    const auto lines = log.lines();
    check(lines.size() == 3, "the ring keeps exactly its capacity");
    check(lines.size() == 3 && unstamped(lines[0]) == "three"
              && unstamped(lines[1]) == "four" && unstamped(lines[2]) == "five",
          "the ring keeps the newest lines, in order");
    check(log.dropped() == 2, "the ring counts what it dropped");
    check(log.text().rfind("[2 earlier lines dropped]\n", 0) == 0,
          "text() says how much was dropped");
    check(lines.size() == 3 && lines[0].rfind("+0.", 0) == 0
              && lines[0].find("s three") != std::string::npos,
          "each line is stamped +S.mmms");

    DiagnosticLog multi(10);
    multi.add("a\nb");
    check(multi.lines().size() == 2, "a multi-line message is one entry per line");

    DiagnosticLog secret_log(10);
    secret_log.add("ssh: echoing c00kie-secret-value by mistake");
    secret_log.add_secret("c00kie-secret-value");
    secret_log.add("again c00kie-secret-value and c00kie-secret-value");
    const std::string text = secret_log.text();
    check(!has(text, "c00kie-secret-value"), "a registered secret appears nowhere");
    check(unstamped(secret_log.lines()[0]) == "ssh: echoing [redacted] by mistake",
          "a line logged before the secret was known is redacted retroactively");
    check(unstamped(secret_log.lines()[1]) == "again [redacted] and [redacted]",
          "every occurrence in a later line is redacted");
    secret_log.add_secret("ab");
    secret_log.add("ab stays");
    check(unstamped(secret_log.lines()[2]) == "ab stays",
          "a too-short secret is not registered");

    DiagnosticLog long_log(4);
    long_log.add(std::string(1000, 'x'));
    check(unstamped(long_log.lines()[0])
              == std::string(DiagnosticLog::max_line_length, 'x') + " [...]",
          "a long line is truncated with a marker");
}

// -------------------------------------------------------------------------
// connect_with_progress with a fake runner, and the preflight
// -------------------------------------------------------------------------

void test_connect_with_progress_failure()
{
    ConnectionProfile wss = make_profile(ConnectionMode::wss, "graviton.example");
    ConnectFlow flow(plan_launch(wss));
    FakeRunner runner;
    runner.exit_code = 255;
    int ticks = 0;
    ConnectResult result =
        connect_with_progress(wss, flow, [&] { ++ticks; }, &runner);
    check(!result.ok && result.transport == nullptr && result.session == nullptr,
          "a failing connect hands back nothing");
    check(flow.state() == FlowState::failed, "the flow ends failed");
    check(result.error.cause == ConnectErrorCause::ssh_failed,
          "a fake runner's bare 255 is the honest ssh_failed");
    check(ticks > 0, "the progress hook was called");
    check(state_of(flow, SK::contacting_broker) == SS::failed,
          "the broker contact stage is the one shown failed");

    ConnectionProfile ssh = make_profile(ConnectionMode::ssh, "graviton.example");
    ConnectFlow tflow(plan_launch(ssh));
    FakeRunner empty;
    empty.stdout_text = "\n";
    ConnectResult tr = connect_with_progress(ssh, tflow, [] {}, &empty);
    check(tr.error.cause == ConnectErrorCause::cookie_unavailable,
          "the unreadable cookie is classified as cookie_unavailable");
}

std::filesystem::path scratch_dir(const std::string& name)
{
    const char* tmp = std::getenv("TMPDIR");
    std::filesystem::path base = tmp && *tmp ? tmp : "/tmp";
#if defined(_WIN32)
    base /= "debeos-rdp-connect-tests";
#else
    base /= "debeos-rdp-connect-tests-" + std::to_string(::getpid());
#endif
    base /= name;
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);
    return base;
}

void test_preflight()
{
    // A missing identity file is reported as itself before anything runs.
    ConnectionProfile p = make_profile(ConnectionMode::wss, "graviton.example");
    p.identity_file = "/nonexistent/debeos-rdp-test-key";
    ConnectFlow flow(plan_launch(p));
    FakeRunner runner;
    ConnectResult r = connect_with_progress(p, flow, [] {}, &runner);
    check(r.error.cause == ConnectErrorCause::identity_missing,
          "preflight: a missing identity file is identity_missing");
    check(runner.calls == 0, "preflight: nothing was run");
    check(state_of(flow, SK::contacting_broker) == SS::failed,
          "preflight: the first stage is shown failed");

#if !defined(_WIN32)
    // A key other users can read: ssh would ignore it ("UNPROTECTED PRIVATE
    // KEY FILE"), so it is reported up front. At 0600 the preflight passes and
    // the attempt reaches the runner.
    const auto dir = scratch_dir("identity");
    const std::string key = (dir / "id_test").string();
    std::ofstream(key) << "not really a key\n";
    ::chmod(key.c_str(), 0644);
    p.identity_file = key;
    ConnectFlow open_flow(plan_launch(p));
    FakeRunner open_runner;
    ConnectResult open = connect_with_progress(p, open_flow, [] {}, &open_runner);
    check(open.error.cause == ConnectErrorCause::identity_unreadable,
          "preflight: a 0644 key is identity_unreadable");
    check(open_runner.calls == 0, "preflight: a 0644 key runs nothing");
    ::chmod(key.c_str(), 0600);
    ConnectFlow ok_flow(plan_launch(p));
    FakeRunner ok_runner;
    ok_runner.exit_code = 255;
    (void)connect_with_progress(p, ok_flow, [] {}, &ok_runner);
    check(ok_runner.calls == 1, "preflight: a 0600 key gets as far as ssh");

    // No ssh on PATH: told so, nothing run.
    const auto empty_dir = scratch_dir("empty-path");
    const char* old_path = std::getenv("PATH");
    const std::string saved = old_path != nullptr ? old_path : "";
    ::setenv("PATH", empty_dir.c_str(), 1);
    ConnectionProfile q = make_profile(ConnectionMode::ssh, "graviton.example");
    ConnectFlow qflow(plan_launch(q));
    ConnectOptions options;
    FakeRunner q_runner;
    options.runner = &q_runner;
    options.check_ssh_executable = true;
    ConnectResult qr = connect_with_progress(q, qflow, [] {}, options);
    ::setenv("PATH", saved.c_str(), 1);
    check(qr.error.cause == ConnectErrorCause::ssh_not_found,
          "preflight: no ssh on PATH is ssh_not_found");
    check(q_runner.calls == 0, "preflight: no ssh runs nothing");
#endif
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
    // substrate is "up" -- and then the broker transport cannot be used: here
    // nothing listens on 127.0.0.1's broker port, or (in a build without
    // OpenSSL) there is no wss transport at all. The plan's next route, the
    // tunnel, must still be tried. The second case used to end the attempt at
    // the broker: make_transport()'s failure returned without falling back,
    // which is why this test failed on hosts without OpenSSL.
    ConnectionProfile p = make_profile(ConnectionMode::automatic, "127.0.0.1");
    const LaunchPlan plan = plan_launch(p);
    if (!build_supports_wss) {
        // Without wss the broker step is not planned at all -- a route this
        // build cannot honour is not a route -- so auto is the tunnel alone
        // and its one ssh command is the cookie fetch.
        check(plan.steps.size() == 1 && plan.steps[0].kind == RouteKind::tunnel,
              "auto mode in a build without wss plans the tunnel alone");
        ConnectFlow flow(plan);
        ScriptedRunner runner;
        runner.replies = {{"\n", 0}};
        ConnectResult result = connect_with_progress(p, flow, [] {}, &runner);
        check(!result.ok && runner.calls == 1,
              "without wss, auto runs one command: the tunnel's cookie fetch");
        check(!flow.fell_back(), "without wss there is nothing to fall back from");
        check(result.error.cause == ConnectErrorCause::cookie_unavailable,
              "without wss, the error is the tunnel's");
        return;
    }
    check(plan.steps.size() == auto_tunnel_step + 1
              && plan.steps[auto_wss_step].kind == RouteKind::broker
              && plan.steps[auto_tunnel_step].kind == RouteKind::tunnel,
          "auto mode plans broker then tunnel (QUIC first where the build has it)");
    ConnectFlow flow(plan);
    ScriptedRunner runner;
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
    check(state_of(flow, SK::contacting_broker, auto_wss_step) == SS::done,
          "the broker's credential fetch succeeded (the substrate came up)");
    check(state_of(flow, SK::authenticating, auto_wss_step) == SS::failed,
          "the broker's connect stage is the one shown failed");
    check(state_of(flow, SK::fetching_cookie, auto_tunnel_step) == SS::failed,
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
    ConnectionProfile p = make_profile(ConnectionMode::automatic, "h");
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

    ConnectionProfile p = make_profile(ConnectionMode::automatic, "127.0.0.1");
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
    check(state_of(flow, StageKind::authenticating, 0) == StageState::failed
              && state_of(flow, StageKind::authenticating, auto_wss_step)
                     == StageState::failed
              && state_of(flow, StageKind::fetching_cookie, auto_tunnel_step)
                     == StageState::failed,
          "the progress screen shows QUIC failed, then wss, then the tunnel");
    check(flow.fell_back(), "the flow records the fallback");
    check(elapsed >= quic_give_up_ms && elapsed < quic_give_up_ms + 2000,
          "QUIC was given up after quic_give_up_ms, not the transport's 10 s ("
              + std::to_string(elapsed) + " ms)");
}
#endif


#if !defined(_WIN32)

// -------------------------------------------------------------------------
// A fake `ssh` on PATH: the real runner, tunnel and process code run
// -------------------------------------------------------------------------

// Modes (FAKE_SSH_MODE): authfail / hostkey print what OpenSSH printed in
// those cases and exit 255; stall sleeps in every invocation; the rest answer
// the cookie fetch and then behave as the tunnel (-N) says. Each invocation
// appends "<pid> <tunnel?>" to $FAKE_SSH_PIDS, and `exec sleep` keeps that pid.
constexpr const char* kFakeSsh = R"SH(#!/bin/sh
tunnel=0
debug=0
lport=
prev=
for a in "$@"; do
  [ "$a" = "-N" ] && tunnel=1
  [ "$a" = "LogLevel=DEBUG1" ] && debug=1
  [ "$prev" = "-L" ] && lport=$(echo "$a" | cut -d: -f2)
  prev="$a"
done
[ -n "$FAKE_SSH_PIDS" ] && echo "$$ $tunnel" >> "$FAKE_SSH_PIDS"
case "$FAKE_SSH_MODE" in
authfail)
  echo "Warning: Permanently added '127.0.0.1' (ED25519) to the list of known hosts." >&2
  echo "nobody@127.0.0.1: Permission denied (publickey)." >&2
  exit 255 ;;
hostkey)
  echo "No ED25519 host key is known for 127.0.0.1 and you have requested strict checking." >&2
  echo "Host key verification failed." >&2
  exit 255 ;;
stall)
  exec sleep 60 ;;
esac
if [ $tunnel = 0 ]; then echo "c00kie-secret-value"; exit 0; fi
case "$FAKE_SSH_MODE" in
portinuse)
  echo "Warning: forwarding with c00kie-secret-value" >&2
  echo "bind [127.0.0.1]:2299: Address already in use" >&2
  echo "channel_setup_fwd_listener_tcpip: cannot listen to port: 2299" >&2
  echo "Could not request local forwarding." >&2
  exit 0 ;;
esac
# tunnel-stall: ssh -N never reports the forward (a real ssh still waiting on
# the network looks the same).
if [ $debug = 1 ] && [ "$FAKE_SSH_MODE" != tunnel-stall ]; then
  echo "debug1: Local connections to 127.0.0.1:$lport forwarded to remote address 127.0.0.1:10900" >&2
  echo "debug1: Local forwarding listening on 127.0.0.1 port $lport." >&2
  echo "debug1: channel 0: new port-listener [port listener] (inactive timeout: 0)" >&2
  echo "debug1: Entering interactive session." >&2
fi
exec sleep 60
)SH";

class FakeSsh {
public:
    explicit FakeSsh(const std::string& mode)
    {
        dir_ = scratch_dir("fake-ssh-" + mode);
        const auto script = dir_ / "ssh";
        std::ofstream(script) << kFakeSsh;
        ::chmod(script.c_str(), 0755);
        pids_ = (dir_ / "pids").string();
        const char* path = std::getenv("PATH");
        saved_path_ = path != nullptr ? path : "";
        ::setenv("PATH", (dir_.string() + ":" + saved_path_).c_str(), 1);
        ::setenv("FAKE_SSH_MODE", mode.c_str(), 1);
        ::setenv("FAKE_SSH_PIDS", pids_.c_str(), 1);
    }
    ~FakeSsh()
    {
        ::setenv("PATH", saved_path_.c_str(), 1);
        ::unsetenv("FAKE_SSH_MODE");
        ::unsetenv("FAKE_SSH_PIDS");
    }
    // pid, and whether that invocation was the tunnel (-N).
    std::vector<std::pair<long, bool>> invocations() const
    {
        std::vector<std::pair<long, bool>> out;
        std::ifstream in(pids_);
        long pid = 0;
        int tunnel = 0;
        while (in >> pid >> tunnel)
            out.emplace_back(pid, tunnel != 0);
        return out;
    }

private:
    std::filesystem::path dir_;
    std::string pids_;
    std::string saved_path_;
};

bool process_alive(long pid)
{
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
}

// Processes whose parent is this test, from /proc. Linux only; -1 elsewhere.
int child_process_count()
{
#if defined(__linux__)
    int count = 0;
    const long self = static_cast<long>(::getpid());
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
        std::ifstream stat(entry.path() / "stat");
        std::string content;
        if (!std::getline(stat, content))
            continue;
        // pid (comm) state ppid ...; comm may hold spaces, so skip past ')'.
        const std::size_t close = content.rfind(')');
        if (close == std::string::npos)
            continue;
        std::istringstream rest(content.substr(close + 1));
        std::string state;
        long ppid = 0;
        rest >> state >> ppid;
        if (ppid == self)
            ++count;
    }
    return count;
#else
    return -1;
#endif
}

void test_fake_ssh_stderr_reaches_the_classifier()
{
    {
        FakeSsh ssh("authfail");
        ConnectionProfile p = make_profile(ConnectionMode::wss, "127.0.0.1");
        ConnectFlow flow(plan_launch(p));
        DiagnosticLog log;
        ConnectOptions options;
        options.log = &log;
        ConnectResult r = connect_with_progress(p, flow, [] {}, options);
        check(r.error.cause == ConnectErrorCause::ssh_auth_failed,
              "real runner: ssh's 'Permission denied' is ssh_auth_failed");
        check(r.error.ssh_stderr == kAuth,
              "real runner: the error carries exactly what ssh printed");
        check(has(log.text(), "ssh: nobody@127.0.0.1: Permission denied (publickey).\n"),
              "real runner: ssh's stderr is in the diagnostic log");
    }
    {
        FakeSsh ssh("hostkey");
        ConnectionProfile p = make_profile(ConnectionMode::ssh, "127.0.0.1");
        ConnectFlow flow(plan_launch(p));
        ConnectResult r = connect_with_progress(p, flow, [] {}, ConnectOptions {});
        check(r.error.cause == ConnectErrorCause::ssh_host_key && !r.error.can_retry,
              "real runner: host key verification failed, no Retry");
    }
    {
        // The real tunnel: cookie fetch succeeds, then ssh -N reports the bind
        // failure on stderr. It also echoes the cookie, which the log must
        // never show.
        FakeSsh ssh("portinuse");
        ConnectionProfile p = make_profile(ConnectionMode::ssh, "127.0.0.1");
        ConnectFlow flow(plan_launch(p));
        DiagnosticLog log;
        ConnectOptions options;
        options.log = &log;
        ConnectResult r = connect_with_progress(p, flow, [] {}, options);
        check(r.error.cause == ConnectErrorCause::local_port_in_use,
              "real tunnel: ssh's bind failure is local_port_in_use");
        check(state_of(flow, SK::waiting_for_forward) == SS::failed,
              "real tunnel: the forward wait is the stage shown failed");
        check(!has(log.text(), "c00kie-secret-value"),
              "real tunnel: the cookie never reaches the diagnostic log");
        check(has(log.text(), "ssh: Warning: forwarding with [redacted]"),
              "real tunnel: the line that held the cookie is kept, redacted");
    }
    {
        // ssh missing, preflight skipped: the child's exec failure line is
        // what names the cause (exit 127 alone would read as a remote command).
        const auto empty_dir = scratch_dir("no-ssh");
        const char* old_path = std::getenv("PATH");
        const std::string saved = old_path != nullptr ? old_path : "";
        ::setenv("PATH", empty_dir.c_str(), 1);
        ConnectionProfile p = make_profile(ConnectionMode::wss, "127.0.0.1");
        ConnectFlow flow(plan_launch(p));
        ConnectOptions options;
        options.check_ssh_executable = false;
        ConnectResult r = connect_with_progress(p, flow, [] {}, options);
        ::setenv("PATH", saved.c_str(), 1);
        check(r.error.cause == ConnectErrorCause::ssh_not_found,
              "exec failure: the child's own line makes it ssh_not_found");
        check(r.error.ssh_stderr
                  == "haiku-remote: cannot execute ssh: No such file or directory\n",
              "exec failure: the exact line the child wrote");
    }
}

// Waits for an AsyncConnect to reach a state, woken by its wake callback.
class Waker {
public:
    std::function<void()> callback()
    {
        return [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            ++generation_;
            cv_.notify_all();
        };
    }
    template <typename Pred>
    bool wait_for(Pred pred, std::chrono::milliseconds limit)
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        std::unique_lock<std::mutex> lock(mutex_);
        while (!pred()) {
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout)
                return pred();
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::uint64_t generation_ = 0;
};

void test_cancel_during_stalled_tunnel()
{
    FakeSsh ssh("tunnel-stall"); // cookie fetch answers; ssh -N then hangs
    ConnectionProfile p = make_profile(ConnectionMode::ssh, "127.0.0.1");
    Waker waker;
    auto attempt = std::make_unique<AsyncConnect>(p, ConnectOptions {},
                                                  waker.callback());
    const bool reached = waker.wait_for(
        [&] { return issue_state_name(attempt->flow()) == "waiting_for_forward"; },
        8s);
    check(reached, "async: the attempt reaches waiting_for_forward");
    // The forked ssh records itself as it starts, which can trail the stage
    // change by a moment; nothing wakes the waker for it, so poll briefly.
    for (int i = 0; i < 100 && ssh.invocations().size() < 2; ++i)
        std::this_thread::sleep_for(20ms);
    const auto running = ssh.invocations();
    check(running.size() == 2 && !running[0].second && running[1].second,
          "async: the cookie fetch and then the tunnel ssh ran");
    const long tunnel_pid = running.size() == 2 ? running[1].first : -1;
    check(tunnel_pid > 0 && process_alive(tunnel_pid),
          "async: the stalled tunnel ssh is alive before Cancel");
    check(!attempt->finished(), "async: a stalled tunnel has not finished");

    const auto before = std::chrono::steady_clock::now();
    attempt->cancel();
    attempt->wait();
    const auto took = std::chrono::steady_clock::now() - before;
    check(took < 2s, "async: Cancel ends a stalled tunnel promptly");
    check(attempt->flow().state() == FlowState::idle,
          "async: a cancelled attempt ends idle");
    check(tunnel_pid > 0 && !process_alive(tunnel_pid),
          "async: the tunnel's ssh is gone once finished()");
    check(child_process_count() == 0 || child_process_count() == -1,
          "async: no child process of this test is left");
    ConnectResult r = attempt->take_result();
    check(r.cancelled && !r.ok && r.transport == nullptr
              && r.connection.tunnel == nullptr,
          "async: the result is cancelled and holds nothing");
    check(has(attempt->log().text(), "cancelled; nothing left running"),
          "async: the log records the cancel");
    attempt.reset();
}

void test_cancel_during_stalled_ssh_command_does_not_fall_back()
{
    // Auto mode, the broker's credential fetch hangs. Cancel must kill it and
    // must NOT go on to the tunnel route, which would spawn another ssh.
    if (!build_supports_wss) {
        // Nothing to test: without wss, auto mode plans no broker step, so
        // there is no first route to cancel and no second to fall back to.
        std::cout << "SKIP: a cancelled broker step does not fall back (this"
                     " build has no wss, so auto mode plans no broker step)\n";
        return;
    }
    FakeSsh ssh("stall");
    ConnectionProfile p = make_profile(ConnectionMode::automatic, "127.0.0.1");
    // The tunnel step's cookie comes from a file, so that step would go
    // straight to spawning ssh -L: only the walk's own cancel check stops it.
    const auto cookie_dir = scratch_dir("cookie-file");
    const std::string cookie_file = (cookie_dir / "session_cookie").string();
    std::ofstream(cookie_file) << "file-cookie-value\n";
    p.cookie_source = cookie_file;
    check(route_stages(plan_launch(p).steps[auto_tunnel_step]).front()
              == SK::starting_tunnel,
          "async: with a cookie file the tunnel step starts at the tunnel");
    Waker waker;
    AsyncConnect attempt(p, ConnectOptions {}, waker.callback());
    check(waker.wait_for([&] { return ssh.invocations().size() == 1; }, 5s),
          "async: the broker's ssh command is running");
    attempt.cancel();
    attempt.wait();
    const auto ran = ssh.invocations();
    check(ran.size() == 1, "async: a cancelled broker step does not fall back");
    // A tunnel ssh killed at once might not live to record itself; the log
    // says whether the walk began the tunnel route at all.
    check(has(attempt.log().text(), "route 0: broker")
              && !has(attempt.log().text(), "route 1:"),
          "async: the walk never began route 1 after the cancel");
    check(ran.size() == 1 && !process_alive(ran[0].first),
          "async: the stalled ssh command is gone once finished()");
    check(child_process_count() == 0 || child_process_count() == -1,
          "async: no child process is left after a cancelled ssh command");
    check(attempt.take_result().cancelled, "async: the result is cancelled");
}

// -------------------------------------------------------------------------
// The RP handshake against an in-process server
// -------------------------------------------------------------------------

// Listens on 127.0.0.1, treats connections that send nothing as readiness
// probes, and on the first connection that sends a frame plays app_server's
// side of the handshake according to `mode`:
//   echo    -- answer RP_INIT_CONNECTION with RP_INIT_CONNECTION
//              (RemoteHWInterface.cpp:764-767), then read on;
//   silent  -- never answer;
//   close   -- close (FIN) once the client's RP_HELLO has arrived;
//   reset   -- reset (RST) once the client's RP_HELLO has arrived;
//   garbage -- answer with a frame whose size is below the 6-byte header.
// ops() is the opcode sequence the server read, decoded here, independently
// of the client.
class MiniServer {
public:
    explicit MiniServer(std::string mode) : mode_(std::move(mode))
    {
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        ::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listen_, 8);
        socklen_t len = sizeof(addr);
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~MiniServer()
    {
        stop_ = true;
        thread_.join();
        ::close(listen_);
    }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] std::vector<std::uint16_t> ops()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return ops_;
    }
    [[nodiscard]] std::string cookie()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return cookie_;
    }

private:
    void serve()
    {
        while (!stop_) {
            pollfd pfd {listen_, POLLIN, 0};
            if (::poll(&pfd, 1, 50) <= 0)
                continue;
            const int fd = ::accept(listen_, nullptr, nullptr);
            if (fd < 0)
                continue;
            session(fd);
        }
    }

    void session(int fd)
    {
        std::vector<std::uint8_t> buffer;
        bool any = false;
        bool answered = false;
        while (!stop_) {
            pollfd pfd {fd, POLLIN, 0};
            if (::poll(&pfd, 1, 50) <= 0)
                continue;
            std::uint8_t chunk[4096];
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0)
                break; // a probe, or the client is done
            any = true;
            buffer.insert(buffer.end(), chunk, chunk + n);
            // Frame: uint16 code, uint32 size (header included), payload.
            while (buffer.size() >= 6) {
                const std::uint16_t op =
                    static_cast<std::uint16_t>(buffer[0] | (buffer[1] << 8));
                const std::uint32_t size = buffer[2] | (buffer[3] << 8)
                                           | (buffer[4] << 16)
                                           | (static_cast<std::uint32_t>(buffer[5]) << 24);
                if (size < 6 || buffer.size() < size)
                    break;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ops_.push_back(op);
                    if (op == 12 && size >= 14) {
                        // uint32 method, then a uint32-length-prefixed string.
                        const std::uint32_t length = buffer[10] | (buffer[11] << 8)
                                                     | (buffer[12] << 16)
                                                     | (static_cast<std::uint32_t>(buffer[13]) << 24);
                        if (14 + length <= size)
                            cookie_.assign(buffer.begin() + 14,
                                           buffer.begin() + 14 + length);
                    }
                }
                buffer.erase(buffer.begin(), buffer.begin() + size);
                if (op == 6 && !answered) { // RP_HELLO: the client is waiting now
                    answered = true;
                    if (mode_ == "echo") {
                        const std::uint8_t echo[6] = {1, 0, 6, 0, 0, 0};
                        (void)::send(fd, echo, sizeof(echo), MSG_NOSIGNAL);
                    } else if (mode_ == "garbage") {
                        const std::uint8_t bad[6] = {1, 0, 2, 0, 0, 0};
                        (void)::send(fd, bad, sizeof(bad), MSG_NOSIGNAL);
                    } else if (mode_ == "close") {
                        ::close(fd);
                        return;
                    } else if (mode_ == "reset") {
                        linger hard {1, 0};
                        ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &hard, sizeof(hard));
                        ::close(fd);
                        return;
                    }
                }
            }
        }
        ::close(fd);
        if (any)
            stop_session_seen_ = true;
    }

    std::string mode_;
    int listen_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_ {false};
    std::atomic<bool> stop_session_seen_ {false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::uint16_t> ops_;
    std::string cookie_;
};

struct HandshakeRun {
    ConnectResult result;
    std::vector<std::string> names; // issue_state_name at each tick, deduplicated
    FlowState state = FlowState::idle;
};

// Tunnel mode through the fake ssh ("tunnel-up": the cookie fetch answers and
// ssh -N just stays up), with the profile's local port pinned to the server so
// the "forward" is the server itself.
HandshakeRun run_against(MiniServer& server, int timeout_ms = 15000)
{
    FakeSsh ssh("tunnel-up");
    ConnectionProfile p = make_profile(ConnectionMode::ssh, "127.0.0.1");
    p.local_port = server.port();
    p.width = 640;
    p.height = 480;
    ConnectFlow flow(plan_launch(p));
    HandshakeRun run;
    ConnectOptions options;
    options.handshake_timeout_ms = timeout_ms;
    run.result = connect_with_progress(
        p, flow,
        [&] {
            const std::string name = issue_state_name(flow);
            if (run.names.empty() || run.names.back() != name)
                run.names.push_back(name);
        },
        options);
    run.state = flow.state();
    return run;
}

void test_handshake_against_a_server()
{
    {
        MiniServer server("echo");
        HandshakeRun run = run_against(server);
        check(run.result.ok && run.state == FlowState::connected,
              "handshake: an echoed RP_INIT_CONNECTION connects");
        check(run.names
                  == std::vector<std::string> {"connecting", "fetching_cookie",
                                               "starting_tunnel",
                                               "waiting_for_forward",
                                               "connecting_transport",
                                               "waiting_for_protocol_handshake",
                                               "requesting_display", "connected"},
              "handshake: issue #1's states, in order");
        // What the server read: cookie, init, hello, then -- only after its
        // echo -- the palette and display requests (session.cpp's
        // Op::init_connection handler).
        check(server.ops() == std::vector<std::uint16_t> {12, 1, 6, 4, 2},
              "handshake: the server read cookie, init, hello, palette, display");
        check(server.cookie() == "c00kie-secret-value",
              "handshake: the fetched cookie went out in the cookie frame");
        check(run.result.session != nullptr
                  && run.result.session->message_count() == 1
                  && run.result.session->surface().width() == 640
                  && run.result.session->surface().height() == 480,
              "handshake: the session ingested the echo, at the profile's size");
        check(run.result.connection.tunnel != nullptr,
              "handshake: the result owns the tunnel");
    }
    {
        MiniServer server("silent");
        HandshakeRun run = run_against(server, 300);
        check(!run.result.ok
                  && run.result.error.cause == ConnectErrorCause::handshake_timeout,
              "handshake: a server that never answers is handshake_timeout");
        check(run.names.back() == "failed" && run.state == FlowState::failed,
              "handshake: a timeout ends failed, not connected");
        check(server.ops() == std::vector<std::uint16_t> {12, 1, 6},
              "handshake: nothing past hello was sent without the echo");
    }
    {
        MiniServer server("close");
        HandshakeRun run = run_against(server);
        check(run.result.error.cause == ConnectErrorCause::remote_closed,
              "handshake: a FIN before the echo is remote_closed");
    }
    {
        MiniServer server("reset");
        HandshakeRun run = run_against(server);
        // The route is the tunnel, so the reset is read as ssh's channel
        // failure, not app_server's refusal (see test_handshake_classification).
        check(run.result.error.cause == ConnectErrorCause::server_listener,
              "handshake: an RST through the tunnel before the echo is"
              " server_listener");
    }
    {
        MiniServer server("garbage");
        HandshakeRun run = run_against(server);
        check(run.result.error.cause == ConnectErrorCause::decode_failed,
              "handshake: an undecodable frame is decode_failed");
    }
    check(child_process_count() == 0 || child_process_count() == -1,
          "handshake: every tunnel ssh was reaped with its result");
}

void test_cancel_during_handshake()
{
    // Connected, waiting on a server that never answers, with the full
    // 15 s handshake timeout ahead: Cancel must shut the socket down under the
    // blocked receive (Transport::abort) rather than wait the timeout out.
    MiniServer server("silent");
    FakeSsh ssh("tunnel-up");
    ConnectionProfile p = make_profile(ConnectionMode::ssh, "127.0.0.1");
    p.local_port = server.port();
    Waker waker;
    ConnectOptions options;
    options.handshake_timeout_ms = 15000;
    AsyncConnect attempt(p, options, waker.callback());
    check(waker.wait_for(
              [&] {
                  return issue_state_name(attempt.flow())
                      == "waiting_for_protocol_handshake";
              },
              8s),
          "async: the attempt reaches waiting_for_protocol_handshake");
    // The stage is published just before the handshake is sent; a cancel that
    // early is caught by the loop's own check and never exercises abort().
    // Wait until the server has read RP_HELLO and then a further 300 ms, so the
    // worker is certainly parked in its 15 s receive.
    for (int i = 0; i < 200 && server.ops().size() < 3; ++i)
        std::this_thread::sleep_for(10ms);
    check(server.ops() == std::vector<std::uint16_t> {12, 1, 6},
          "async: the server has the cookie, init and hello");
    std::this_thread::sleep_for(300ms);
    const auto before = std::chrono::steady_clock::now();
    attempt.cancel();
    attempt.wait();
    check(std::chrono::steady_clock::now() - before < 2s,
          "async: Cancel ends a blocked handshake promptly");
    const auto ran = ssh.invocations();
    check(ran.size() == 2 && !process_alive(ran[1].first),
          "async: the tunnel's ssh is gone after a cancelled handshake");
    check(attempt.take_result().cancelled, "async: the handshake cancel is cancelled");
    check(child_process_count() == 0 || child_process_count() == -1,
          "async: no child process left after a cancelled handshake");
}

#endif // !_WIN32

// -------------------------------------------------------------------------
// ConnectScreen: affordances + headless render to PNG
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

bool pixel_is(const Surface& surface, int x, int y, Color c)
{
    const auto px = surface.pixel(x, y);
    return px.r == c.r && px.g == c.g && px.b == c.b;
}

void test_screen_render_and_actions()
{
    ConnectFlow flow(plan_launch(make_profile(ConnectionMode::automatic, "graviton.example"),
                                 /*wss_available=*/true, /*quic_available=*/false));
    flow.begin_step(0, RouteKind::broker);
    flow.phase(0, ConnectPhase::contacting_broker);
    flow.step_failed(0, RouteKind::broker, "broker returned no token");
    flow.begin_step(1, RouteKind::tunnel);
    flow.phase(1, ConnectPhase::fetching_cookie);
    flow.phase(1, ConnectPhase::opening_tunnel);
    flow.phase(1, ConnectPhase::waiting_for_forward);

    ConnectScreen progress(flow, "Graviton (us-west-2)", 900, 560);
    progress.set_elapsed(12345ms);
    const Surface& psurface = progress.render();
    check(psurface.width() == 900 && psurface.height() == 560,
          "the progress screen renders at the requested size");
    check(progress.regions().size() == 1
              && progress.regions()[0].kind == ConnectScreen::Region::Kind::cancel,
          "while connecting the one affordance is Cancel");
    const auto cancel = find_region(progress, ConnectScreen::Region::Kind::cancel);
    // The shared chrome palette (ui_theme.hpp, values restated here so a
    // palette change has to change this test too).
    const Color paper {246, 243, 238, 255};
    const Color card {255, 253, 250, 255};
    const Color cobalt {37, 89, 189, 255};
    const Color danger {178, 50, 40, 255};
    const Color done_green {46, 125, 50, 255};
    check(pixel_is(psurface, 2, 2, paper), "the progress background is the theme's");
    // The secondary button style: card fill inside a border.
    check(cancel && pixel_is(psurface, cancel->x + 4, cancel->y + 4, card),
          "the Cancel button is drawn where its region is, card-filled");
    // Stage markers: 12 px discs centred 6 px above each row's baseline. Rows
    // start at baseline 128, 26 px apart, with 8 px between routes: stage 0
    // (failed) at 128, stage 4 (step 1's first, done) at 240, stage 6
    // (active) at 292.
    check(pixel_is(psurface, 32 + 6, 128 - 6, danger), "a failed stage's marker");
    check(pixel_is(psurface, 32 + 6, 240 - 6, done_green), "a done stage's marker");
    check(pixel_is(psurface, 32 + 6, 292 - 6, cobalt), "the active stage's marker");

    // Keyboard focus: Tab puts the 2 px ring, 1 px clear, around Cancel, and
    // Enter presses it.
    progress.key(ConnectScreen::Key::tab);
    const Surface& focused = progress.render();
    check(progress.focus() == ConnectScreen::Region::Kind::cancel
              && progress.focus_visible(),
          "Tab focuses Cancel");
    check(cancel && pixel_is(focused, cancel->x - 3, cancel->y + 18, cobalt)
              && pixel_is(focused, cancel->x - 2, cancel->y + 18, cobalt)
              && pixel_is(focused, cancel->x - 1, cancel->y + 18, paper)
              && pixel_is(focused, cancel->x - 4, cancel->y + 18, paper),
          "the focus ring: two cobalt pixels, one pixel clear of the button");
    progress.key(ConnectScreen::Key::enter);
    check(progress.take_action() == ConnectScreen::Action::cancel,
          "Enter on the focused Cancel cancels");
    check(cancel && cancel->y + cancel->h <= 560,
          "the Cancel row fits the window");
    check(ConnectScreen::elapsed_text(12345ms) == "Elapsed 12 s"
              && ConnectScreen::elapsed_text(125000ms) == "Elapsed 2 min 05 s",
          "the elapsed clock text");

    progress.pointer_press(cancel->x + 5, cancel->y + 5);
    check(progress.take_action() == ConnectScreen::Action::cancel,
          "clicking Cancel yields cancel");
    check(!progress.focus_visible(), "a click hides the focus ring");
    progress.key(ConnectScreen::Key::escape);
    check(progress.take_action() == ConnectScreen::Action::cancel,
          "Escape while connecting is Cancel");
    progress.key(ConnectScreen::Key::enter);
    check(progress.take_action() == ConnectScreen::Action::none,
          "Enter while connecting does nothing");
    progress.set_cancelling(true);
    progress.render();
    check(progress.regions().empty(), "once cancelling, the Cancel button is gone");
    progress.key(ConnectScreen::Key::escape);
    check(progress.take_action() == ConnectScreen::Action::none,
          "Escape while cancelling does nothing more");

    const auto dir = output_dir();
    std::string png_error;
    const std::string progress_path = (dir / "connect-progress.png").string();
    progress.set_cancelling(false);
    check(write_png(progress.render(), progress_path, png_error),
          "the progress screen writes a PNG");

    // An error screen: Back + Retry, and with a log, Copy diagnostics.
    ConnectFlow efail(plan_launch(make_profile(ConnectionMode::wss, "h")));
    efail.begin_step(0, RouteKind::broker);
    efail.phase(0, ConnectPhase::contacting_broker);
    efail.fail(classify_connect_failure(ConnectFailPoint::substrate_broker,
                                        ConnectFailure::none,
                                        "ssh to h exited 255", kAuth));
    ConnectScreen error_screen(efail, "Graviton", 900, 560);
    error_screen.render();
    check(find_region(error_screen, ConnectScreen::Region::Kind::back)
              && find_region(error_screen, ConnectScreen::Region::Kind::retry)
              && !find_region(error_screen, ConnectScreen::Region::Kind::copy_log),
          "without a log: Back and Retry, no Copy diagnostics");
    error_screen.set_diagnostics({"+0.000s stage contacting_broker",
                                  "+0.120s ssh: nobody@127.0.0.1: Permission denied"
                                  " (publickey)."});
    const Surface& esurface = error_screen.render();
    const auto copy = find_region(error_screen, ConnectScreen::Region::Kind::copy_log);
    check(copy.has_value(), "with a log: Copy diagnostics is offered");
    error_screen.pointer_press(copy->x + 5, copy->y + 5);
    check(error_screen.take_action() == ConnectScreen::Action::copy_log,
          "clicking Copy diagnostics yields copy_log");
    const auto retry = find_region(error_screen, ConnectScreen::Region::Kind::retry);
    check(retry && pixel_is(esurface, retry->x + 4, retry->y + 4, cobalt),
          "Retry is the primary, cobalt-filled button");
    check(pixel_is(esurface, 2, 2, paper), "the error background is the theme's");
    // Tab order follows the buttons: Back, Retry, Copy diagnostics, around.
    error_screen.key(ConnectScreen::Key::tab);
    check(error_screen.focus() == ConnectScreen::Region::Kind::back, "Tab 1: Back");
    error_screen.key(ConnectScreen::Key::tab);
    error_screen.key(ConnectScreen::Key::tab);
    check(error_screen.focus() == ConnectScreen::Region::Kind::copy_log,
          "Tab 3: Copy diagnostics");
    const Surface& copy_focused = error_screen.render();
    check(pixel_is(copy_focused, copy->x - 3, copy->y + 18, cobalt)
              && pixel_is(copy_focused, copy->x - 1, copy->y + 18, paper),
          "Copy diagnostics carries the focus ring");
    error_screen.key(ConnectScreen::Key::enter);
    check(error_screen.take_action() == ConnectScreen::Action::copy_log,
          "Enter on focused Copy diagnostics copies");
    error_screen.key(ConnectScreen::Key::tab);
    check(error_screen.focus() == ConnectScreen::Region::Kind::back,
          "Tab wraps to Back");
    error_screen.key(ConnectScreen::Key::back_tab);
    check(error_screen.focus() == ConnectScreen::Region::Kind::copy_log,
          "Shift-Tab wraps back");
    error_screen.pointer_press(retry->x + retry->w / 2, retry->y + retry->h / 2);
    check(error_screen.take_action() == ConnectScreen::Action::retry,
          "clicking Retry yields the retry action");
    check(error_screen.take_action() == ConnectScreen::Action::none,
          "the action is one-shot");
    const std::string error_path = (dir / "connect-error.png").string();
    check(write_png(esurface, error_path, png_error), "the error screen writes a PNG");

    // A terminal cause offers Back only.
    ConnectFlow epin(plan_launch(make_profile(ConnectionMode::wss, "h")));
    epin.begin_step(0, RouteKind::broker);
    epin.phase(0, ConnectPhase::contacting_broker);
    epin.begin_transport(0, RouteKind::broker);
    epin.fail(classify_connect_failure(ConnectFailPoint::transport_broker,
                                       ConnectFailure::other,
                                       "certificate pin mismatch: x"));
    ConnectScreen pin_screen(epin, "Graviton", 900, 560);
    pin_screen.render();
    check(find_region(pin_screen, ConnectScreen::Region::Kind::back).has_value()
              && !find_region(pin_screen, ConnectScreen::Region::Kind::retry),
          "a non-retryable error offers Back and no Retry");
    pin_screen.key(ConnectScreen::Key::enter);
    check(pin_screen.take_action() == ConnectScreen::Action::back,
          "Enter on a terminal error goes Back, not Retry");

    std::cerr << "connect screenshots: " << progress_path << " , " << error_path
              << '\n';
}

} // namespace

int main()
{
#if !defined(_WIN32)
    // A test that kills a socket mid-write must not die of SIGPIPE.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    test_flow_stage_model();
    test_flow_connect_transitions();
    test_flow_fallback();
    test_ssh_stderr_classification();
    test_classification();
    test_handshake_classification();
    test_diagnostic_log();
    test_connect_with_progress_failure();
    test_preflight();
    test_broker_that_cannot_be_reached_falls_back_to_the_tunnel();
    test_quic_advertisement_is_parsed();
#ifdef HAIKU_REMOTE_HAVE_QUIC
    test_auto_skips_quic_the_broker_does_not_offer();
    test_blocked_udp_falls_back_to_wss();
#else
    std::cerr << "SKIP: QUIC in auto mode (built without OpenSSL 3.5)\n";
#endif
#if !defined(_WIN32)
    test_fake_ssh_stderr_reaches_the_classifier();
    test_cancel_during_stalled_tunnel();
    test_cancel_during_stalled_ssh_command_does_not_fall_back();
    test_handshake_against_a_server();
    test_cancel_during_handshake();
#endif
    test_screen_render_and_actions();

    if (failures == 0) {
        std::cout << "PASS - " << checks << " connect-flow checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " connect-flow checks FAILED\n";
    return 1;
}
