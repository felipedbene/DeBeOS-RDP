// Tests for DeBeOS-RDP issue #1 Part 2: the managed connection transports.
//
// Covered here:
//   - the pure SSH argv builders (what ssh is actually invoked with),
//   - broker-credential and session-cookie parsing, driven by a fake command
//     runner so no socket or ssh child is involved,
//   - open_connection()'s route selection and broker->tunnel fallback,
//   - and the lifecycle guarantee that matters most: an owned child process is
//     reaped (never orphaned) when its handle is torn down.
//
// House rules (CLAUDE.md): assert exact values, keep expectations independent
// of the code that produces them.

#include "haiku_remote/connect_flow.hpp" // classify_ssh_stderr
#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/launch_arguments.hpp"
#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/profile_launch.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwchar>
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

// Whole-vector equality, reporting the first differing position so a failure
// says which argument moved.
void check_argv(const std::vector<std::string>& actual,
                const std::vector<std::string>& expected, std::string_view what)
{
    const bool equal = actual == expected;
    check(equal, what);
    if (equal)
        return;
    std::size_t i = 0;
    while (i < actual.size() && i < expected.size() && actual[i] == expected[i])
        ++i;
    std::cerr << "      " << what << ": first difference at [" << i << "]: got \""
              << (i < actual.size() ? actual[i] : std::string("<end>"))
              << "\", expected \""
              << (i < expected.size() ? expected[i] : std::string("<end>"))
              << "\" (sizes " << actual.size() << " vs " << expected.size()
              << ")\n";
}

bool contains(const std::vector<std::string>& v, std::string_view needle)
{
    for (const auto& s : v)
        if (s == needle)
            return true;
    return false;
}

bool contains_str(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Index of `needle` in `v`, or -1.
int index_of(const std::vector<std::string>& v, std::string_view needle)
{
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i] == needle)
            return static_cast<int>(i);
    return -1;
}

// A scratch directory, removed on scope exit.
struct TestDir {
    std::filesystem::path path;
    explicit TestDir(const std::string& name)
    {
        path = std::filesystem::temp_directory_path()
               / ("haiku-remote-tunnel-" + name + "-"
                  + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TestDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// A CommandRunner that returns a scripted answer and records what it was asked
// to run, so the SSH-driven logic can be exercised without a real ssh.
class FakeRunner : public CommandRunner {
public:
    std::string stdout_text;
    int exit_code = 0;
    bool fail_to_launch = false;
    std::vector<std::string> last_argv;
    int calls = 0;

    int run(const std::vector<std::string>& argv, std::string& out,
            std::string& error) override
    {
        ++calls;
        last_argv = argv;
        if (fail_to_launch) {
            error = "fake: could not launch";
            return -1;
        }
        out = stdout_text;
        return exit_code;
    }
};

ConnectionProfile make_profile(ConnectionMode mode, std::string host)
{
    ConnectionProfile p;
    p.name = "T";
    p.host = std::move(host);
    p.mode = mode;
    p.remote_port = 10900;
    p.ssh_user = "baron";
    p.ssh_port = 22;
    return p;
}

void test_ssh_argv_builders()
{
    SshConfig cfg;
    cfg.user = "baron";
    cfg.host = "graviton.example";
    cfg.port = 2222;
    cfg.identity_file = "/keys/id_ed25519";
    cfg.known_hosts_file = "/keys/known_hosts";

    // Golden vectors, written out by hand from issue #1's "Required SSH
    // options" list -- never derived from the builder -- and compared whole, so
    // a dropped, added, reordered or mistyped argument fails. Elementwise
    // equality, not "contains": a contains-check passed with ConnectTimeout=10
    // still present beside a new =15.
    const std::vector<std::string> expected_base = {
        "ssh", "-T",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=15",
        "-o", "ServerAliveInterval=15",
        "-o", "ServerAliveCountMax=3",
        "-o", "ControlMaster=no",
        "-o", "ControlPath=none",
        "-p", "2222",
        "-i", "/keys/id_ed25519",
        "-o", "IdentitiesOnly=yes",
        "-o", "UserKnownHostsFile=/keys/known_hosts",
    };
    check_argv(build_ssh_base_argv(cfg), expected_base, "base argv");

    std::vector<std::string> expected_tunnel = expected_base;
    // LogLevel=DEBUG1 is the readiness signal; the -L bind address is
    // explicit, so a taken 127.0.0.1 port fails the forward instead of leaving
    // ssh up on [::1] only.
    for (const char* arg : {"-o", "LogLevel=DEBUG1", "-o", "ExitOnForwardFailure=yes",
                            "-N", "-L", "127.0.0.1:10900:127.0.0.1:10901",
                            "baron@graviton.example"})
        expected_tunnel.emplace_back(arg);
    const auto tunnel = build_ssh_tunnel_argv(cfg, 10900, "127.0.0.1", 10901);
    check_argv(tunnel, expected_tunnel, "tunnel argv");
    check(!contains(tunnel, "-f"),
          "the tunnel is NOT backgrounded by ssh -- we own it in the foreground");

    // The cookie/token fetch gets the same hardening: -T (no tty to mangle the
    // secret), keepalives, and above all no ControlMaster reuse, which would
    // ignore -i and BatchMode. No -N/-L: it runs a command and exits.
    std::vector<std::string> expected_exec = expected_base;
    expected_exec.emplace_back("baron@graviton.example");
    expected_exec.emplace_back("cat /some/file");
    check_argv(build_ssh_exec_argv(cfg, "cat /some/file"), expected_exec,
               "exec argv");

    // Minimal config: port 22 is still spelled out (the profile, not an
    // ssh_config Port line, decides), and nothing optional appears.
    SshConfig plain;
    plain.host = "h";
    plain.port = 22;
    const std::vector<std::string> expected_plain = {
        "ssh", "-T",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=15",
        "-o", "ServerAliveInterval=15",
        "-o", "ServerAliveCountMax=3",
        "-o", "ControlMaster=no",
        "-o", "ControlPath=none",
        "-p", "22",
        "h", "true",
    };
    check_argv(build_ssh_exec_argv(plain, "true"), expected_plain,
               "minimal exec argv (no user, default port, no key)");
}

void test_broker_credentials_parse()
{
    SshConfig cfg;
    cfg.user = "baron";
    cfg.host = "graviton.example";

    FakeRunner runner;
    runner.stdout_text =
        "tok-abc123\n"
        "===BROKER_PEM===\n"
        "-----BEGIN CERTIFICATE-----\nMIIBdummy\n-----END CERTIFICATE-----\n";
    BrokerCredentials creds;
    std::string error;
    check(fetch_broker_credentials(runner, cfg, default_broker_port, creds, error),
          "a well-formed broker response parses");
    check(creds.token == "tok-abc123", "the token is parsed, whitespace trimmed");
    check(contains_str(creds.cert_pem, "BEGIN CERTIFICATE"),
          "the certificate PEM is parsed");
    check(contains_str(creds.cert_pem, "END CERTIFICATE"),
          "the full certificate body survives, newlines intact");
    check(index_of(runner.last_argv, "baron@graviton.example") >= 0,
          "the broker fetch ssh'd to the right host");

    // Missing marker -> failure.
    FakeRunner no_marker;
    no_marker.stdout_text = "just a token with no cert";
    BrokerCredentials c2;
    std::string e2;
    check(!fetch_broker_credentials(no_marker, cfg, default_broker_port, c2, e2),
          "a response with no certificate marker is rejected");

    // Empty token -> failure.
    FakeRunner no_token;
    no_token.stdout_text =
        "===BROKER_PEM===\n-----BEGIN CERTIFICATE-----\nx\n-----END CERTIFICATE-----\n";
    BrokerCredentials c3;
    std::string e3;
    check(!fetch_broker_credentials(no_token, cfg, default_broker_port, c3, e3),
          "an empty token is rejected");

    // No certificate body -> failure.
    FakeRunner no_cert;
    no_cert.stdout_text = "tok\n===BROKER_PEM===\n";
    BrokerCredentials c4;
    std::string e4;
    check(!fetch_broker_credentials(no_cert, cfg, default_broker_port, c4, e4),
          "a missing certificate is rejected");

    // ssh itself failed -> failure.
    FakeRunner ssh_failed;
    ssh_failed.exit_code = 255;
    ssh_failed.stdout_text = "";
    BrokerCredentials c5;
    std::string e5;
    check(!fetch_broker_credentials(ssh_failed, cfg, default_broker_port, c5, e5),
          "a non-zero ssh exit is a failure");
}

void test_session_cookie_parse()
{
    SshConfig cfg;
    cfg.host = "h";

    FakeRunner runner;
    runner.stdout_text = "  deadbeefcafe\n";
    std::string cookie;
    std::string error;
    check(fetch_session_cookie(runner, cfg, 10900, cookie, error),
          "a cookie line parses");
    check(cookie == "deadbeefcafe", "the cookie is trimmed of surrounding space");
    check(contains_str(runner.last_argv.back(), "session_cookie.10900"),
          "the cookie fetch reads the port-specific cookie file");

    FakeRunner empty;
    empty.stdout_text = "\n";
    std::string c2;
    std::string e2;
    check(!fetch_session_cookie(empty, cfg, 10900, c2, e2),
          "an empty cookie is rejected");
}

void test_open_connection_broker()
{
    // wss mode: the broker route, fully driven by the fake runner -- no ssh
    // child, no socket.
    ConnectionProfile wss = make_profile(ConnectionMode::wss, "graviton.example");
    FakeRunner runner;
    runner.stdout_text =
        "tok-xyz\n===BROKER_PEM===\n"
        "-----BEGIN CERTIFICATE-----\nMIIBdummy\n-----END CERTIFICATE-----\n";

    ManagedConnection conn = open_connection(wss, &runner);
    check(conn.ok, "a wss profile with a good broker response connects");
    check(conn.kind == RouteKind::broker, "the taken route is the broker");
    check(contains_str(conn.transport.url, "wss://graviton.example:10902"),
          "the broker transport targets the broker port");
    check(conn.transport.token == "tok-xyz", "the fetched token is carried");
    // The SSH-fetched certificate is a known_brokers seed, never a ca_file: a
    // chain+name check fails whenever the host is dialed by IP. This dummy PEM
    // does not parse, so nothing is seeded and the user would be asked.
    check(conn.transport.ca_file.empty(),
          "the broker certificate is not passed as a ca_file");
    check(conn.transport.known_broker_fingerprint.empty(),
          "an unparsable fetched certificate seeds nothing");
    check(conn.transport.cookie.empty(),
          "no session cookie is sent over the broker (it presents its own)");
    check(conn.tunnel == nullptr, "the broker route owns no ssh tunnel");
}

void test_open_connection_broker_failure_is_terminal_for_wss()
{
    // wss forces the broker: a broker failure is the whole failure, with no
    // tunnel fallback (which would otherwise try to spawn ssh).
    ConnectionProfile wss = make_profile(ConnectionMode::wss, "127.0.0.1");
    FakeRunner runner;
    runner.exit_code = 255; // ssh to the broker failed
    ManagedConnection conn = open_connection(wss, &runner);
    check(!conn.ok, "wss with a failing broker does not connect");
    check(contains_str(conn.error, "broker route"),
          "the error names the broker route");
    check(runner.calls == 1,
          "wss makes exactly one attempt -- no tunnel fallback");
    check(conn.tunnel == nullptr, "no tunnel was spawned for a forced-broker mode");
}

#if !defined(_WIN32)

// True once `pid` no longer exists (ESRCH), polling up to `ms`. A pid that is
// not ours to reap -- the guard's child, or a guard reparented to init -- can
// only be watched, not waited for.
bool gone_within(long pid, int ms)
{
    if (pid <= 0)
        return false;
    for (int waited = 0; waited <= ms; waited += 20) {
        if (::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void test_managed_process_is_reaped()
{
    // The core teardown guarantee: a spawned child is gone after the handle is
    // torn down. We spawn a long-lived child, confirm it is alive, then reap it
    // and confirm the OS no longer knows the pid.
    long pid = -1;
    long program = -1;
    {
        ManagedProcess proc;
        std::string error;
        check(proc.spawn({"sh", "-c", "sleep 60"}, error),
              "a benign long-lived child spawns");
        check(proc.spawned(), "the process reports itself spawned");
        pid = proc.pid();
        program = proc.program_pid();
        check(pid > 0, "a real pid was assigned");
        check(program > 0 && program != pid,
              "the program runs under a guard, and its own pid is reported");
        // Give it a moment to be scheduled, then confirm it is alive.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(proc.running(), "the child is running before teardown");
        check(::kill(static_cast<pid_t>(pid), 0) == 0,
              "the OS confirms the child is alive");
        proc.terminate();
        check(!proc.running(), "the child is not running after terminate()");
    }
    // After terminate() the pid must be dead and reaped: kill(pid, 0) fails with
    // ESRCH (no such process). If it were orphaned, it would still exist.
    const int rc = ::kill(static_cast<pid_t>(pid), 0);
    check(rc == -1 && errno == ESRCH,
          "the child is gone (reaped, not orphaned) after teardown");
    check(gone_within(program, 3000),
          "and so is the program the guard ran -- not just the guard");
}

void test_managed_process_reaped_by_destructor()
{
    // Same guarantee, but relying on the destructor rather than an explicit
    // terminate() -- this is the path the GUI hits at the end of each session.
    long pid = -1;
    long program = -1;
    {
        ManagedProcess proc;
        std::string error;
        proc.spawn({"sh", "-c", "sleep 60"}, error);
        pid = proc.pid();
        program = proc.program_pid();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } // destructor runs here
    const int rc = ::kill(static_cast<pid_t>(pid), 0);
    check(rc == -1 && errno == ESRCH,
          "the destructor alone reaps the child -- no orphan on scope exit");
    check(gone_within(program, 3000),
          "and the program under the guard went with it");
}

// The guarantee a crashed or force-quit client needs, and the one POSIX gave
// none of: the owner is SIGKILLed, so ~ManagedProcess never runs and nothing
// in this process gets to clean up. Before the parent-death guard the ssh
// child simply outlived it (measured on Linux and on macOS 26 with
// validate-macos.sh: "ssh ... outlived a SIGKILLed client"). A forked holder
// owns the ManagedProcess here, reports both pids, and is SIGKILLed.
void test_managed_process_dies_with_a_sigkilled_owner()
{
    int report[2];
    if (::pipe(report) != 0) {
        check(false, "the report pipe opens");
        return;
    }
    const pid_t holder = ::fork();
    if (holder == 0) {
        ::close(report[0]);
        ManagedProcess proc;
        std::string error;
        long pids[2] = {-1, -1};
        if (proc.spawn({"sh", "-c", "sleep 60"}, error)) {
            pids[0] = proc.pid();
            pids[1] = proc.program_pid();
        }
        (void)!::write(report[1], pids, sizeof(pids));
        for (;;)
            ::pause();
    }
    ::close(report[1]);
    long pids[2] = {-1, -1};
    const bool read_ok = ::read(report[0], pids, sizeof(pids))
        == static_cast<ssize_t>(sizeof(pids));
    ::close(report[0]);
    check(read_ok && pids[0] > 0 && pids[1] > 0,
          "the holder spawned a guarded program and reported both pids");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(::kill(static_cast<pid_t>(pids[1]), 0) == 0,
          "the program is running while its owner lives");

    ::kill(holder, SIGKILL); // no destructor, no cleanup code
    int status = 0;
    ::waitpid(holder, &status, 0);

    const int survivors = (gone_within(pids[1], 3000) ? 0 : 1)
        + (gone_within(pids[0], 3000) ? 0 : 1);
    check(survivors == 0,
          "SIGKILLing the owner takes down the program and its guard ("
              + std::to_string(survivors) + " of 2 survived)");
    if (survivors != 0) {
        ::kill(static_cast<pid_t>(pids[1]), SIGKILL);
        ::kill(static_cast<pid_t>(pids[0]), SIGKILL);
    }
}

// The guard exits when its program does, so running() still notices a
// program that ends on its own -- e.g. an ssh that failed to connect.
void test_a_program_that_exits_is_noticed()
{
    ManagedProcess proc;
    std::string error;
    check(proc.spawn({"sh", "-c", "exit 3"}, error),
          "a program that exits at once spawns");
    bool stopped = false;
    for (int i = 0; i < 300 && !stopped; ++i) {
        stopped = !proc.running();
        if (!stopped)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(stopped, "running() turns false within 3 s of the program exiting");
}

#endif // !_WIN32

void test_ssh_tunnel_unreachable_leaves_no_child()
{
    // Point a real SshTunnel at a closed loopback port. ssh must fail (refused
    // or exit), start() must report it, and crucially no ssh child may be left
    // running afterwards.
    SshConfig cfg;
    cfg.user = "nobody";
    cfg.host = "127.0.0.1";
    cfg.port = 1; // nothing listens here; ssh connect is refused fast
    SshTunnel tunnel(cfg, 10900, 0);
    TestDir records("records-unreachable");
    tunnel.set_record_dir(records.path);
    std::string error;
    const bool ok = tunnel.start(error);
    check(!ok, "a tunnel to a closed ssh port does not come up");
    check(!error.empty(), "the failure is reported with a message");
    check(!tunnel.running(),
          "no ssh child is left running after a failed tunnel start");
}


// --- ssh's own readiness signal (forward_readiness) -------------------------

// What OpenSSH 9.9p1 wrote to stderr, from "Authenticated" on, for
// `ssh -T -o BatchMode=yes -o LogLevel=DEBUG1 -o ExitOnForwardFailure=yes -N
// -L <spec> localhost` against a real sshd. Verbatim except the home
// directory, and cut after the lines that matter.
const char* const kSshReady =
    "Authenticated to localhost ([127.0.0.1]:22) using \"publickey\".\n"
    "debug1: pkcs11_del_provider: called, provider_id = (null)\n"
    "debug1: Local connections to 127.0.0.1:41240 forwarded to remote address 127.0.0.1:22\n"
    "debug1: Local forwarding listening on 127.0.0.1 port 41240.\n"
    "debug1: channel 0: new port-listener [port listener] (inactive timeout: 0)\n"
    "debug1: Requesting no-more-sessions@openssh.com\n"
    "debug1: Entering interactive session.\n"
    "debug1: pledge: filesystem\n";
// -L 127.0.0.1:41241:... with 127.0.0.1:41241 already taken: fatal, exit 255.
const char* const kSshExplicitTaken =
    "Authenticated to localhost ([127.0.0.1]:22) using \"publickey\".\n"
    "debug1: pkcs11_del_provider: called, provider_id = (null)\n"
    "debug1: Local connections to 127.0.0.1:41241 forwarded to remote address 127.0.0.1:22\n"
    "debug1: Local forwarding listening on 127.0.0.1 port 41241.\n"
    "bind [127.0.0.1]:41241: Address already in use\n"
    "channel_setup_fwd_listener_tcpip: cannot listen to port: 41241\n"
    "Could not request local forwarding.\n";
// -L 41242:... (no bind address) with 127.0.0.1:41242 taken: ssh binds [::1]
// only, counts that a success, and carries on -- the defect the explicit bind
// address fixes. It still prints "Entering interactive session.".
const char* const kSshImplicitTaken =
    "Authenticated to localhost ([127.0.0.1]:22) using \"publickey\".\n"
    "debug1: pkcs11_del_provider: called, provider_id = (null)\n"
    "debug1: Local connections to LOCALHOST:41242 forwarded to remote address 127.0.0.1:22\n"
    "debug1: Local forwarding listening on ::1 port 41242.\n"
    "debug1: channel 0: new port-listener [port listener] (inactive timeout: 0)\n"
    "debug1: Local forwarding listening on 127.0.0.1 port 41242.\n"
    "bind [127.0.0.1]:41242: Address already in use\n"
    "debug1: Requesting no-more-sessions@openssh.com\n"
    "debug1: Entering interactive session.\n"
    "debug1: pledge: filesystem\n";

void test_forward_readiness_from_ssh_stderr()
{
    std::string why;
    check(forward_readiness(kSshReady, 41240, why) == ForwardReadiness::ready
              && why.empty(),
          "readiness: the real transcript of a bound forward is ready");
    check(forward_readiness(kSshReady, 4124, why) == ForwardReadiness::pending,
          "readiness: another port's lines are not this forward's (4124 vs 41240)");

    check(forward_readiness(kSshExplicitTaken, 41241, why) == ForwardReadiness::failed
              && why == "bind [127.0.0.1]:41241: Address already in use",
          "readiness: a taken port fails, naming ssh's bind error");

    // The defect the explicit bind address closes: ssh up on [::1] only. Even
    // past "Entering interactive session." it is not ready, it failed.
    check(forward_readiness(kSshImplicitTaken, 41242, why) == ForwardReadiness::failed
              && why == "bind [127.0.0.1]:41242: Address already in use",
          "readiness: a forward that lost 127.0.0.1 is failed, not ready");

    // The listening line is logged BEFORE bind(): alone it is still pending.
    const std::string listening_only =
        "debug1: Local forwarding listening on 127.0.0.1 port 41240.\n";
    check(forward_readiness(listening_only, 41240, why) == ForwardReadiness::pending,
          "readiness: 'listening' alone is not ready (it precedes bind())");
    // ...and "Entering interactive session." with no forward is no forward
    // (a ClearAllForwardings in ssh_config, say).
    check(forward_readiness("debug1: Entering interactive session.\n", 41240, why)
              == ForwardReadiness::pending,
          "readiness: an interactive session without the forward is not ready");
    // Line at a time, CRLF from Windows OpenSSH included.
    ForwardReadinessScanner scanner(41240);
    check(scanner.feed("debug1: Local forwarding listening on 127.0.0.1 port 41240.\r")
                  == ForwardReadiness::pending
              && scanner.feed("debug1: Entering interactive session.")
                     == ForwardReadiness::ready,
          "readiness: the scanner, fed lines, reaches ready");

    check(is_ssh_debug_line("debug1: Entering interactive session.")
              && is_ssh_debug_line("debug3: x") && !is_ssh_debug_line("debug: x")
              && !is_ssh_debug_line("bind [127.0.0.1]:1: Address already in use")
              && !is_ssh_debug_line("Permission denied (publickey)."),
          "debug lines are told from the ones ssh prints by default");
}

#if !defined(_WIN32)


// --- captured stderr ------------------------------------------------------

void test_stderr_is_captured_bounded_and_drained()
{
    {
        ManagedProcess proc;
        std::string error;
        check(proc.spawn_capturing_stderr(
                  {"sh", "-c", "printf 'one\\ntwo\\n' >&2; printf 'out\\n'; exit 3"},
                  error),
              "stderr: a capturing spawn starts");
        for (int i = 0; i < 300 && proc.running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(proc.stderr_text() == "one\ntwo\n",
              "stderr: stderr_text() is exactly what the program wrote there");
        check(proc.read_stderr() == "one\ntwo\n" && proc.read_stderr().empty(),
              "stderr: read_stderr() hands each byte over once");
        check(!proc.stderr_open() && proc.stderr_fd() == -1,
              "stderr: once read to its end the stream is closed");
        check(proc.stderr_text() == "one\ntwo\n",
              "stderr: stderr_text() survives reading");
    }
    {
        // Bounded: 10 bytes too many, and the oldest 10 go.
        const std::size_t cap = process_stderr_capacity;
        ManagedProcess proc;
        std::string error;
        const std::string script = "printf 'AAAAAAAAAA' >&2; head -c "
            + std::to_string(cap) + " /dev/zero | tr '\\0' 'B' >&2";
        proc.spawn_capturing_stderr({"sh", "-c", script}, error);
        for (int i = 0; i < 500 && proc.running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const std::string expected =
            "[... 10 earlier bytes dropped ...]\n" + std::string(cap, 'B');
        check(proc.stderr_text() == expected,
              "stderr: past the capacity the oldest bytes go, and the text says"
              " how many");
        check(proc.read_stderr() == expected,
              "stderr: an unread backlog is bounded the same way");
    }
    {
        // Nobody reads, and the program writes 1 MiB -- sixteen times a pipe's
        // buffer. Without the drain it would block in write() forever.
        ManagedProcess proc;
        std::string error;
        proc.spawn_capturing_stderr(
            {"sh", "-c", "head -c 1048576 /dev/zero >&2; exit 0"}, error);
        bool exited = false;
        for (int i = 0; i < 500 && !exited; ++i) {
            exited = !proc.running();
            if (!exited)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(exited, "stderr: a program that floods stderr is never blocked by it");
    }
}

// --- SshTunnel against a scripted `ssh` on PATH ----------------------------

// Writes `body` as an executable `ssh` in a fresh directory and puts that
// first on PATH for the object's lifetime. The script also records its argv,
// one argument per line, in "argv".
class ScriptedSsh {
public:
    ScriptedSsh(const std::string& name, const std::string& body) : dir_(name)
    {
        const auto script = dir_.path / "ssh";
        std::ofstream(script) << "#!/bin/sh\nfor a in \"$@\"; do echo \"$a\"; done > '"
                              << (dir_.path / "argv").string() << "'\n"
                              << body;
        std::filesystem::permissions(script, std::filesystem::perms::owner_all);
        const char* path = std::getenv("PATH");
        saved_ = path != nullptr ? path : "";
        ::setenv("PATH", (dir_.path.string() + ":" + saved_).c_str(), 1);
    }
    ~ScriptedSsh() { ::setenv("PATH", saved_.c_str(), 1); }
    [[nodiscard]] std::vector<std::string> argv() const
    {
        std::vector<std::string> out;
        std::ifstream in(dir_.path / "argv");
        for (std::string line; std::getline(in, line);)
            out.push_back(line);
        return out;
    }
    [[nodiscard]] const std::filesystem::path& dir() const { return dir_.path; }

private:
    TestDir dir_;
    std::string saved_;
};

// A local port nothing listens on (bound, read, released).
std::uint16_t idle_port()
{
    std::string error;
    return pick_free_local_port(error);
}

void test_tunnel_readiness_never_connects_through_the_forward()
{
    const std::uint16_t port = idle_port();
    // Reports the forward exactly as OpenSSH does -- but listens on nothing.
    // Ready is the report; a probe through the forward would have found
    // nothing to connect to and timed out.
    ScriptedSsh ssh("ready",
                    "echo \"debug1: Local forwarding listening on 127.0.0.1 port "
                        + std::to_string(port) + ".\" >&2\n"
                    "echo \"debug1: Entering interactive session.\" >&2\n"
                    "echo \"a line ssh prints by default\" >&2\n"
                    "exec sleep 60\n");
    TestDir records("records-ready");
    SshConfig cfg;
    cfg.user = "baron";
    cfg.host = "h";
    SshTunnel tunnel(cfg, 10900, port);
    tunnel.set_record_dir(records.path);
    tunnel.set_ready_timeout_ms(5000);
    std::vector<std::string> lines;
    tunnel.set_stderr_sink([&](const std::string& line) { lines.push_back(line); });
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    const bool up = tunnel.start(error);
    const auto took = std::chrono::steady_clock::now() - started;
    check(up && error.empty(), "tunnel: ssh's report of the forward is readiness");
    check(took < std::chrono::seconds(3),
          "tunnel: ready as soon as ssh says so, not after a probe timeout");
    for (int i = 0; i < 100 && lines.empty(); ++i) {
        tunnel.drain_stderr();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(lines == std::vector<std::string> {"a line ssh prints by default"},
          "tunnel: the sink gets ssh's default-level lines, not its debug1 lines");
    check(contains_str(tunnel.stderr_text(), "debug1: Entering interactive session."),
          "tunnel: stderr_text() keeps the debug1 lines");
    const auto argv = ssh.argv();
    check(index_of(argv, "LogLevel=DEBUG1") >= 0
              && index_of(argv, "127.0.0.1:" + std::to_string(port) + ":127.0.0.1:10900")
                     >= 0,
          "tunnel: ssh was asked for DEBUG1 and the explicit 127.0.0.1 bind");
}

void test_tunnel_readiness_failures()
{
    SshConfig cfg;
    cfg.user = "baron";
    cfg.host = "h";
    {
        // The listening line, and then nothing: still pending at the timeout.
        const std::uint16_t port = idle_port();
        ScriptedSsh ssh("listening-only",
                        "echo \"debug1: Local forwarding listening on 127.0.0.1 port "
                            + std::to_string(port) + ".\" >&2\nexec sleep 60\n");
        TestDir records("records-listening");
        SshTunnel tunnel(cfg, 10900, port);
        tunnel.set_record_dir(records.path);
        tunnel.set_ready_timeout_ms(800);
        std::string error;
        check(!tunnel.start(error)
                  && error
                         == "timed out waiting for the ssh forward on 127.0.0.1:"
                                + std::to_string(port)
                                + " (ssh did not report it listening)",
              "tunnel: 'listening' alone times out -- it precedes bind()");
        check(!tunnel.running(), "tunnel: the timed-out ssh was reaped");
    }
    {
        // ssh up on [::1] only, as OpenSSH 9.9 is with no bind address and a
        // taken 127.0.0.1 port (kSshImplicitTaken): refused, and stopped,
        // although it stays alive.
        const std::uint16_t port = idle_port();
        const std::string p = std::to_string(port);
        ScriptedSsh ssh("partial",
                        "echo \"debug1: Local forwarding listening on ::1 port " + p
                            + ".\" >&2\n"
                            "echo \"debug1: Local forwarding listening on 127.0.0.1 port "
                            + p + ".\" >&2\n"
                            "echo \"bind [127.0.0.1]:" + p
                            + ": Address already in use\" >&2\n"
                              "echo \"debug1: Entering interactive session.\" >&2\n"
                              "exec sleep 60\n");
        TestDir records("records-partial");
        SshTunnel tunnel(cfg, 10900, port);
        tunnel.set_record_dir(records.path);
        tunnel.set_ready_timeout_ms(5000);
        std::string error;
        check(!tunnel.start(error)
                  && error == "ssh could not open the forward on 127.0.0.1:" + p
                                  + ": bind [127.0.0.1]:" + p
                                  + ": Address already in use",
              "tunnel: a forward that lost 127.0.0.1 fails with ssh's bind error");
        check(!tunnel.running(), "tunnel: and that ssh, still up on ::1, is stopped");
    }
    {
        // Early exit: the error says so, and ssh's own words reach the sink.
        ScriptedSsh ssh("authfail",
                        "echo \"baron@h: Permission denied (publickey).\" >&2\nexit 255\n");
        TestDir records("records-authfail");
        const std::uint16_t port = idle_port();
        SshTunnel tunnel(cfg, 10900, port);
        tunnel.set_record_dir(records.path);
        std::vector<std::string> lines;
        tunnel.set_stderr_sink([&](const std::string& line) { lines.push_back(line); });
        std::string error;
        check(!tunnel.start(error)
                  && error == "ssh exited before the forward on 127.0.0.1:"
                                  + std::to_string(port)
                                  + " came up (check the key, host, and that the"
                                    " remote port is listening)",
              "tunnel: an ssh that exits early is a failed start");
        check(lines == std::vector<std::string> {"baron@h: Permission denied (publickey)."},
              "tunnel: ssh's stderr reaches the sink, exactly");
        check(tunnel.stderr_text() == "baron@h: Permission denied (publickey).\n",
              "tunnel: and stderr_text() holds it, exactly");
        check(std::filesystem::is_empty(records.path),
              "tunnel: the record of an ssh that exited is gone");
    }
}

// --- verified orphans -------------------------------------------------------

// Fork/exec `argv` as a plain child of this test: no guard, no record.
pid_t spawn_plain(const std::vector<std::string>& argv)
{
    std::vector<char*> cargv;
    for (const auto& a : argv)
        cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execvp(cargv[0], cargv.data());
        _exit(127);
    }
    return pid;
}

void reap(pid_t pid)
{
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
}

// A pid that is certainly free: a child that has exited and been reaped.
long dead_pid()
{
    const pid_t pid = spawn_plain({"true"});
    int status = 0;
    ::waitpid(pid, &status, 0);
    return pid;
}

void write_record_file(const std::filesystem::path& file, const ProcessRecord& r)
{
    std::ofstream(file) << serialize_process_record(r);
}

#if defined(__linux__) || defined(__APPLE__)
// SshTunnel::spawn() sweeps the record directory before it starts ssh: a
// verified orphan of an earlier crashed run is stopped then, and a live
// process that merely has a record is not.
void test_tunnel_start_sweeps_verified_orphans()
{
    const std::uint16_t port = idle_port();
    ScriptedSsh ssh("sweep-on-start",
                    "echo \"debug1: Local forwarding listening on 127.0.0.1 port "
                        + std::to_string(port) + ".\" >&2\n"
                    "echo \"debug1: Entering interactive session.\" >&2\n"
                    "exec sleep 60\n");
    TestDir records("records-sweep-on-start");
    const pid_t orphan = spawn_plain({"sleep", "63"});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ProcessRecord r;
    r.owner_pid = dead_pid();
    r.owner_start = "a crashed run";
    r.program_pid = orphan;
    r.program_start = process_start_identity(orphan);
    r.argv = {"sleep", "63"};
    write_record_file(records.path / "process-crashed.json", r);

    SshConfig cfg;
    cfg.user = "baron";
    cfg.host = "h";
    SshTunnel tunnel(cfg, 10900, port);
    tunnel.set_record_dir(records.path);
    std::string error;
    const bool up = tunnel.start(error);
    int status = 0;
    const bool reaped = ::waitpid(orphan, &status, 0) == orphan;
    check(up && tunnel.last_sweep().stopped == std::vector<long> {orphan} && reaped,
          "tunnel: start() stopped the crashed run's verified orphan first");
    check(!std::filesystem::exists(records.path / "process-crashed.json"),
          "tunnel: and deleted its record");
    const auto files = std::distance(std::filesystem::directory_iterator(records.path),
                                     std::filesystem::directory_iterator {});
    check(files == 1, "tunnel: the running ssh has a record of its own");
}

#endif

void test_process_identity_and_records()
{
#if defined(__linux__) || defined(__APPLE__)
    const std::string self = process_start_identity(::getpid());
    check(!self.empty() && self == process_start_identity(::getpid()),
          "identity: this process has a stable start identity");
    const pid_t child = spawn_plain({"sleep", "60"});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::string other = process_start_identity(child);
    check(!other.empty() && other != self, "identity: another process has another");
    check(process_argv(child) == std::optional<std::vector<std::string>>(
                                     std::vector<std::string> {"sleep", "60"}),
          "identity: a process's argv reads back exactly");
    reap(child);
    check(process_start_identity(child).empty() && !process_argv(child),
          "identity: a reaped pid has none");
#else
    std::cout << "SKIP: process identity (this platform has no start identity;"
                 " records are never written or acted on here)\n";
#endif
    check(process_start_identity(0).empty() && process_start_identity(-1).empty(),
          "identity: no pid, no identity");

    // The record format, written out by hand.
    ProcessRecord r;
    r.owner_pid = 4100;
    r.owner_start = "boot:111";
    r.program_pid = 4102;
    r.program_start = "boot:222";
    r.argv = {"ssh", "-N", "-L", "127.0.0.1:1:127.0.0.1:2", "u@h"};
    const std::string golden = "{\n"
                               "  \"kind\": \"haiku-remote-process\",\n"
                               "  \"version\": 1,\n"
                               "  \"owner\": {\n"
                               "    \"pid\": 4100,\n"
                               "    \"start\": \"boot:111\"\n"
                               "  },\n"
                               "  \"program\": {\n"
                               "    \"pid\": 4102,\n"
                               "    \"start\": \"boot:222\",\n"
                               "    \"argv\": [\n"
                               "      \"ssh\",\n"
                               "      \"-N\",\n"
                               "      \"-L\",\n"
                               "      \"127.0.0.1:1:127.0.0.1:2\",\n"
                               "      \"u@h\"\n"
                               "    ]\n"
                               "  }\n"
                               "}\n";
    check(serialize_process_record(r) == golden, "record: the on-disk form");
    const auto back = parse_process_record(golden);
    check(back && back->owner_pid == 4100 && back->owner_start == "boot:111"
              && back->program_pid == 4102 && back->program_start == "boot:222"
              && back->argv == r.argv,
          "record: it parses back to the same record");
    for (const char* broken :
         {"{}", "not json",
          R"({"kind":"other","version":1,"owner":{"pid":1,"start":"a"},"program":{"pid":2,"start":"b","argv":["x"]}})",
          R"({"kind":"haiku-remote-process","version":1,"owner":{"pid":1,"start":"a"},"program":{"pid":2,"start":"","argv":["x"]}})",
          R"({"kind":"haiku-remote-process","version":1,"owner":{"pid":1,"start":"a"},"program":{"pid":2,"start":"b","argv":[]}})"})
        check(!parse_process_record(broken), std::string("record: refused: ") + broken);
}

#if defined(__linux__) || defined(__APPLE__)

// The sweep acts on a verified orphan and on nothing else.
void test_sweep_never_signals_an_unrelated_process()
{
    TestDir dir("sweep");
    const std::vector<std::string> argv = {"sleep", "61"};
    const pid_t bystander = spawn_plain(argv);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::string start = process_start_identity(bystander);

    ProcessRecord base;
    base.owner_pid = dead_pid();
    base.owner_start = "gone";
    base.program_pid = bystander;
    base.program_start = start;
    base.argv = argv;

    // 1. A recycled pid: right pid, another start identity.
    ProcessRecord recycled = base;
    recycled.program_start = start + "0";
    write_record_file(dir.path / "recycled.json", recycled);
    // 2. Right pid and start, another argv.
    ProcessRecord other_argv = base;
    other_argv.argv = {"sleep", "62"};
    write_record_file(dir.path / "argv.json", other_argv);
    // 3. Everything right, but its owner -- this process -- is alive.
    ProcessRecord live = base;
    live.owner_pid = ::getpid();
    live.owner_start = process_start_identity(::getpid());
    write_record_file(dir.path / "live.json", live);
    // 4. Not a record.
    std::ofstream(dir.path / "junk.json") << "{\"kind\":";

    const OrphanSweep first = sweep_orphaned_processes(dir.path);
    check(first.stopped.empty(), "sweep: nothing was stopped");
    check(::kill(bystander, 0) == 0, "sweep: the bystander is alive");
    check(first.stale == 2 && first.live_owner == 1 && first.unreadable == 1,
          "sweep: two mismatches, one live owner, one unreadable");
    check(!std::filesystem::exists(dir.path / "recycled.json")
              && !std::filesystem::exists(dir.path / "argv.json")
              && !std::filesystem::exists(dir.path / "junk.json")
              && std::filesystem::exists(dir.path / "live.json"),
          "sweep: dead records deleted, a live owner's record kept");

    // The positive control: the same process, verified on every count and
    // with its owner gone, IS stopped.
    std::filesystem::remove(dir.path / "live.json");
    write_record_file(dir.path / "orphan.json", base);
    const OrphanSweep second = sweep_orphaned_processes(dir.path);
    int status = 0;
    const bool reaped = ::waitpid(bystander, &status, 0) == bystander;
    check(second.stopped == std::vector<long> {bystander} && reaped
              && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
          "sweep: a verified orphan is stopped, by SIGTERM");
    check(std::filesystem::is_empty(dir.path), "sweep: and its record is gone");
}

// The residual case: the owner AND its guard are SIGKILLed together, the way
// `killall -9 haiku-remote-gui` does it (the guard is a fork of the client, so
// it bears the client's name). The program -- here one that ignores SIGTERM,
// which also defeats Linux's PR_SET_PDEATHSIG -- outlives both. Only the
// record finds it again, and a later run's sweep stops it. (The program loops
// on `sleep 1` rather than exec'ing sleep: an exec would replace the argv the
// record verifies, as no ssh ever does.)
void test_an_orphan_of_a_killed_owner_and_guard_is_swept()
{
    TestDir dir("orphan");
    int report[2];
    if (::pipe(report) != 0) {
        check(false, "orphan: the report pipe opens");
        return;
    }
    const pid_t holder = ::fork();
    if (holder == 0) {
        ::close(report[0]);
        ManagedProcess proc;
        proc.record_in(dir.path);
        std::string error;
        long pids[2] = {-1, -1};
        if (proc.spawn({"sh", "-c", "trap '' TERM; while :; do sleep 1; done"}, error)) {
            pids[0] = proc.pid();
            pids[1] = proc.program_pid();
        }
        (void)!::write(report[1], pids, sizeof(pids));
        for (;;)
            ::pause();
    }
    ::close(report[1]);
    long pids[2] = {-1, -1};
    const bool read_ok = ::read(report[0], pids, sizeof(pids))
        == static_cast<ssize_t>(sizeof(pids));
    ::close(report[0]);
    check(read_ok && pids[0] > 0 && pids[1] > 0, "orphan: a guarded program runs");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(std::distance(std::filesystem::directory_iterator(dir.path),
                        std::filesystem::directory_iterator {})
              == 1,
          "orphan: spawn() wrote one record");

    // Guard first, then the owner: the guard never sees its lifeline close.
    ::kill(static_cast<pid_t>(pids[0]), SIGKILL);
    ::kill(holder, SIGKILL);
    int status = 0;
    ::waitpid(holder, &status, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const bool survived = ::kill(static_cast<pid_t>(pids[1]), 0) == 0
        && !process_start_identity(pids[1]).empty();
    check(survived, "orphan: the program outlived its owner and its guard");

    const OrphanSweep sweep = sweep_orphaned_processes(dir.path);
    check(sweep.stopped == std::vector<long> {pids[1]},
          "orphan: a later run's sweep stops exactly that program");
    check(gone_within(pids[1], 3000), "orphan: it is gone (SIGKILL after the grace)");
    check(std::filesystem::is_empty(dir.path), "orphan: and its record deleted");
    if (!gone_within(pids[1], 0))
        ::kill(static_cast<pid_t>(pids[1]), SIGKILL);
}

// While the owner lives, a SIGKILLed guard is noticed at once: the program
// (verified as the one spawn() started) is stopped when the guard is reaped.
void test_a_killed_guard_takes_its_program_down_with_it()
{
    TestDir dir("guard");
    ManagedProcess proc;
    proc.record_in(dir.path);
    std::string error;
    proc.spawn({"sh", "-c", "trap '' TERM; while :; do sleep 1; done"}, error);
    const long program = proc.program_pid();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ::kill(static_cast<pid_t>(proc.pid()), SIGKILL);
    bool stopped = false;
    for (int i = 0; i < 300 && !stopped; ++i) {
        stopped = !proc.running();
        if (!stopped)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(stopped, "guard: a SIGKILLed guard is noticed");
    check(gone_within(program, 100), "guard: its program was stopped with it");
    check(std::filesystem::is_empty(dir.path), "guard: and the record deleted");
    if (!gone_within(program, 0))
        ::kill(static_cast<pid_t>(program), SIGKILL);
}

#endif // __linux__ || __APPLE__

#if defined(__linux__)
// Linux closes the residual case outright: PR_SET_PDEATHSIG gives an ordinary
// program (one that does not ignore SIGTERM) a SIGTERM when its guard dies.
void test_pdeathsig_stops_the_program_of_a_killed_guard()
{
    int report[2];
    if (::pipe(report) != 0)
        return;
    const pid_t holder = ::fork();
    if (holder == 0) {
        ::close(report[0]);
        ManagedProcess proc;
        std::string error;
        long pids[2] = {-1, -1};
        if (proc.spawn({"sleep", "60"}, error)) {
            pids[0] = proc.pid();
            pids[1] = proc.program_pid();
        }
        (void)!::write(report[1], pids, sizeof(pids));
        for (;;)
            ::pause();
    }
    ::close(report[1]);
    long pids[2] = {-1, -1};
    (void)!::read(report[0], pids, sizeof(pids));
    ::close(report[0]);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ::kill(static_cast<pid_t>(pids[0]), SIGKILL);
    ::kill(holder, SIGKILL);
    int status = 0;
    ::waitpid(holder, &status, 0);
    check(gone_within(pids[1], 3000),
          "pdeathsig: the program of a SIGKILLed owner+guard is gone (Linux)");
    if (!gone_within(pids[1], 0))
        ::kill(static_cast<pid_t>(pids[1]), SIGKILL);
}
#endif

#endif // !_WIN32

void test_pick_free_local_port()
{
    std::string error;
    const std::uint16_t a = pick_free_local_port(error);
    check(a != 0, "a free local port is found");
    check(error.empty(), "no error when a port is found");
}

#if defined(_WIN32)

// Windows counterparts of the POSIX lifecycle tests above. The child is
// `cmd.exe /c "ping -n 60 127.0.0.1 >NUL"`: a two-level tree (cmd, then ping),
// so these also prove the kill-on-close job takes down the whole tree, not only
// the process ManagedProcess spawned directly.

bool win_alive(DWORD pid)
{
    HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h == nullptr)
        return false; // no such process (or already fully gone)
    const DWORD rc = ::WaitForSingleObject(h, 0);
    ::CloseHandle(h);
    return rc == WAIT_TIMEOUT;
}

// The pid of a live child of `parent`, or 0. Polls for up to 3 s because cmd
// needs a moment to start ping.
DWORD win_wait_for_child(DWORD parent)
{
    for (int i = 0; i < 60; ++i) {
        HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W entry {};
        entry.dwSize = sizeof(entry);
        DWORD found = 0;
        if (snap != INVALID_HANDLE_VALUE && ::Process32FirstW(snap, &entry)) {
            do {
                if (entry.th32ParentProcessID == parent
                    && win_alive(entry.th32ProcessID))
                    found = entry.th32ProcessID;
            } while (found == 0 && ::Process32NextW(snap, &entry));
        }
        if (snap != INVALID_HANDLE_VALUE)
            ::CloseHandle(snap);
        if (found != 0)
            return found;
        ::Sleep(50);
    }
    return 0;
}

// Both pids gone, allowing the OS up to 2 s to finish tearing the tree down.
bool win_both_gone(DWORD a, DWORD b)
{
    for (int i = 0; i < 40; ++i) {
        if (!win_alive(a) && !win_alive(b))
            return true;
        ::Sleep(50);
    }
    return false;
}

const std::vector<std::string> kWinTree = {"cmd.exe", "/c",
                                           "ping -n 60 127.0.0.1 >NUL"};

void test_managed_process_is_reaped_win()
{
    ManagedProcess proc;
    std::string error;
    check(proc.spawn(kWinTree, error), "a benign long-lived child tree spawns");
    const DWORD pid = static_cast<DWORD>(proc.pid());
    check(pid > 0, "a real pid was assigned");
    const DWORD grandchild = win_wait_for_child(pid);
    check(grandchild != 0, "the child's own child (ping) is running");
    check(proc.running(), "the child is running before teardown");
    proc.terminate();
    check(!proc.running(), "the child is not running after terminate()");
    check(win_both_gone(pid, grandchild),
          "terminate() leaves neither the child nor its child alive");
}

void test_managed_process_reaped_by_destructor_win()
{
    DWORD pid = 0;
    DWORD grandchild = 0;
    {
        ManagedProcess proc;
        std::string error;
        proc.spawn(kWinTree, error);
        pid = static_cast<DWORD>(proc.pid());
        grandchild = win_wait_for_child(pid);
        check(grandchild != 0, "the tree is up before scope exit");
    } // destructor runs here
    check(win_both_gone(pid, grandchild),
          "the destructor alone takes down the whole tree");
}

// The guarantee that matters for a client that is killed outright (Task
// Manager, taskkill /F, a crash): ~ManagedProcess never runs, and only the
// kill-on-close job stands between the user and an orphaned ssh. A helper copy
// of this binary holds a ManagedProcess and is then TerminateProcess()ed.
void test_managed_process_survives_no_holder_kill_win(const char* self)
{
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    ::CreatePipe(&read_pipe, &write_pipe, &sa, 0);
    ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    std::string command = std::string("\"") + self + "\" --hold-child";
    STARTUPINFOA si {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi {};
    const BOOL ok = ::CreateProcessA(nullptr, command.data(), nullptr, nullptr,
                                     TRUE, 0, nullptr, nullptr, &si, &pi);
    ::CloseHandle(write_pipe);
    check(ok != FALSE, "the holder process starts");
    if (!ok) {
        ::CloseHandle(read_pipe);
        return;
    }

    // The holder prints the pid of the child it spawned, then sleeps forever.
    std::string line;
    char c = 0;
    DWORD got = 0;
    while (::ReadFile(read_pipe, &c, 1, &got, nullptr) && got == 1 && c != '\n')
        line.push_back(c);
    ::CloseHandle(read_pipe);
    const DWORD child = static_cast<DWORD>(std::strtoul(line.c_str(), nullptr, 10));
    check(child != 0 && win_alive(child), "the holder's child is running");
    const DWORD grandchild = child != 0 ? win_wait_for_child(child) : 0;
    check(grandchild != 0, "the holder's grandchild is running");

    ::TerminateProcess(pi.hProcess, 9); // no destructor, no cleanup code
    ::WaitForSingleObject(pi.hProcess, 5000);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    check(win_both_gone(child, grandchild),
          "killing the holder outright still takes down its child tree");
}

int hold_child_forever()
{
    ManagedProcess proc;
    std::string error;
    if (!proc.spawn(kWinTree, error))
        return 2;
    std::cout << proc.pid() << '\n' << std::flush;
    ::Sleep(INFINITE);
    return 0;
}


// --- Windows: helper modes of this binary ----------------------------------
//
// The POSIX checks script their children with sh; Windows has no such shell
// worth trusting for byte-exact output, so this binary is its own fixture.
// `--emit-stderr ...` and friends are argv modes; a copy of it named ssh.exe
// on PATH is a fake ssh (fake_ssh_main), steered by FAKE_SSH_MODE like
// connect_tests' shell fake.

std::filesystem::path win_self_path()
{
    std::vector<wchar_t> buffer(32768);
    const DWORD n = ::GetModuleFileNameW(nullptr, buffer.data(),
                                         static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), n));
}

// Raw bytes to a standard handle: no CRT text-mode \n -> \r\n rewriting, so
// what the parent captures is exactly what is written here.
void win_write(DWORD which, const std::string& bytes)
{
    HANDLE h = ::GetStdHandle(which);
    std::size_t done = 0;
    while (done < bytes.size()) {
        DWORD wrote = 0;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - done, 65536));
        if (!::WriteFile(h, bytes.data() + done, chunk, &wrote, nullptr) || wrote == 0)
            return;
        done += wrote;
    }
}

std::string win_env(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
}

int helper_main(int argc, char** argv)
{
    const std::string mode = argv[1];
    if (mode == "--emit-stderr" && argc >= 3 && std::string_view(argv[2]) == "lines") {
        win_write(STD_ERROR_HANDLE, "one\ntwo\n");
        return 3;
    }
    if (mode == "--emit-stderr" && argc >= 4 && std::string_view(argv[2]) == "flood") {
        // Ten 'A's, then N 'B's.
        win_write(STD_ERROR_HANDLE, "AAAAAAAAAA");
        win_write(STD_ERROR_HANDLE,
                  std::string(std::strtoull(argv[3], nullptr, 10), 'B'));
        return 0;
    }
    if (mode == "--emit-cookie") {
        // A cookie fetch's shape: the secret on stdout, ssh chatter on stderr
        // (one line LF-terminated, one CRLF, one with no newline at all).
        win_write(STD_OUTPUT_HANDLE, "c00kie-secret-value\n");
        win_write(STD_ERROR_HANDLE, "a warning on stderr\nthen a CRLF line\r\nand an unterminated one");
        return argc >= 3 ? std::atoi(argv[2]) : 0;
    }
    if (mode == "--print-pid-and-sleep") {
        win_write(STD_OUTPUT_HANDLE, std::to_string(::GetCurrentProcessId()) + "\n");
        ::Sleep(60000);
        return 0;
    }
    return -1; // not a helper mode
}

// The fake ssh. Records "<pid> <tunnel 0/1>" in $FAKE_SSH_PIDS and its argv,
// one per line, in $FAKE_SSH_ARGV; then by $FAKE_SSH_MODE:
//   authfail  what OpenSSH prints for a refused key, exit 255
//   stall     says nothing, sleeps (an unreachable network looks the same)
//   ready     a fetch (no -N) answers a cookie; a tunnel reports the -L
//             forward exactly as OpenSSH's DEBUG1 log does, then one line at
//             the default level, and sleeps as a live forward does
int fake_ssh_main(int argc, char** argv)
{
    bool tunnel = false;
    std::string local_port;
    std::string recorded;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        recorded += a + "\n";
        if (a == "-N")
            tunnel = true;
        if (a == "-L" && i + 1 < argc) {
            const std::string spec = argv[i + 1]; // 127.0.0.1:PORT:host:port
            const auto first = spec.find(':');
            const auto second = spec.find(':', first + 1);
            if (first != std::string::npos && second != std::string::npos)
                local_port = spec.substr(first + 1, second - first - 1);
        }
    }
    if (const std::string file = win_env("FAKE_SSH_ARGV"); !file.empty())
        std::ofstream(file, std::ios::binary) << recorded;
    if (const std::string file = win_env("FAKE_SSH_PIDS"); !file.empty())
        std::ofstream(file, std::ios::binary | std::ios::app)
            << ::GetCurrentProcessId() << ' ' << (tunnel ? 1 : 0) << '\n';
    const std::string mode = win_env("FAKE_SSH_MODE");
    if (mode == "authfail") {
        win_write(STD_ERROR_HANDLE,
                  "Warning: Permanently added '127.0.0.1' (ED25519) to the list"
                  " of known hosts.\r\n"
                  "nobody@127.0.0.1: Permission denied (publickey).\r\n");
        return 255;
    }
    if (mode == "stall") {
        ::Sleep(60000);
        return 0;
    }
    if (!tunnel) {
        win_write(STD_OUTPUT_HANDLE, "c00kie-secret-value\n");
        return 0;
    }
    win_write(STD_ERROR_HANDLE,
              "debug1: Local connections to 127.0.0.1:" + local_port
                  + " forwarded to remote address 127.0.0.1:10900\r\n"
                    "debug1: Local forwarding listening on 127.0.0.1 port "
                  + local_port + ".\r\n"
                    "debug1: channel 0: new port-listener [port listener]"
                    " (inactive timeout: 0)\r\n"
                    "debug1: Entering interactive session.\r\n"
                    "a line ssh prints by default\r\n");
    ::Sleep(60000);
    return 0;
}

// Every live process whose parent is this one and whose image is `exe`
// (case-insensitively), e.g. L"ssh.exe".
std::vector<DWORD> win_children_named(const wchar_t* exe)
{
    std::vector<DWORD> out;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ParentProcessID == ::GetCurrentProcessId()
                && ::_wcsicmp(entry.szExeFile, exe) == 0
                && win_alive(entry.th32ProcessID))
                out.push_back(entry.th32ProcessID);
        } while (::Process32NextW(snap, &entry));
    }
    ::CloseHandle(snap);
    return out;
}

bool win_gone(DWORD pid)
{
    for (int i = 0; i < 40; ++i) {
        if (!win_alive(pid))
            return true;
        ::Sleep(50);
    }
    return false;
}

void win_set_env(const char* name, const std::string& value)
{
    ::_putenv_s(name, value.c_str()); // "" removes it
}

// A copy of this binary as ssh.exe, first on PATH, for the object's lifetime.
class FakeSshWin {
public:
    explicit FakeSshWin(const std::string& mode) : dir_("fake-ssh-" + mode)
    {
        const auto self = win_self_path();
        std::error_code ec;
        std::filesystem::copy_file(self, dir_.path / "ssh.exe",
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        copied_ = !ec;
        saved_path_ = win_env("PATH");
        // This binary's own directory next, for any DLL it was linked against
        // that sits beside it rather than on PATH.
        win_set_env("PATH", dir_.path.string() + ";" + self.parent_path().string()
                                + ";" + saved_path_);
        win_set_env("FAKE_SSH_MODE", mode);
        win_set_env("FAKE_SSH_PIDS", (dir_.path / "pids").string());
        win_set_env("FAKE_SSH_ARGV", (dir_.path / "argv").string());
    }
    ~FakeSshWin()
    {
        win_set_env("PATH", saved_path_);
        win_set_env("FAKE_SSH_MODE", "");
        win_set_env("FAKE_SSH_PIDS", "");
        win_set_env("FAKE_SSH_ARGV", "");
    }
    FakeSshWin(const FakeSshWin&) = delete;
    FakeSshWin& operator=(const FakeSshWin&) = delete;

    [[nodiscard]] bool ok() const { return copied_; }
    // The pid of each invocation, in order.
    [[nodiscard]] std::vector<DWORD> pids() const
    {
        std::vector<DWORD> out;
        std::ifstream in(dir_.path / "pids");
        for (unsigned long pid = 0, tunnel = 0; in >> pid >> tunnel;)
            out.push_back(static_cast<DWORD>(pid));
        return out;
    }
    [[nodiscard]] std::vector<std::string> argv() const
    {
        std::vector<std::string> out;
        std::ifstream in(dir_.path / "argv");
        for (std::string line; std::getline(in, line);)
            out.push_back(line);
        return out;
    }
    // Polls up to 5 s for the n-th invocation to have recorded itself.
    [[nodiscard]] DWORD wait_for_pid(std::size_t n = 1) const
    {
        for (int i = 0; i < 100; ++i) {
            const auto all = pids();
            if (all.size() >= n)
                return all[n - 1];
            ::Sleep(50);
        }
        return 0;
    }

private:
    TestDir dir_;
    std::string saved_path_;
    bool copied_ = false;
};

// --- Windows: captured stderr (pipe HANDLE + reader thread) -----------------

void test_stderr_is_captured_bounded_and_drained_win()
{
    const std::string self = win_self_path().string();
    {
        ManagedProcess proc;
        std::string error;
        check(proc.spawn_capturing_stderr({self, "--emit-stderr", "lines"}, error),
              "stderr (win): a capturing spawn starts: " + error);
        for (int i = 0; i < 300 && proc.running(); ++i)
            ::Sleep(10);
        check(!proc.running(), "stderr (win): the helper exited");
        check(proc.stderr_text() == "one\ntwo\n",
              "stderr (win): stderr_text() is exactly what the program wrote");
        check(proc.read_stderr() == "one\ntwo\n" && proc.read_stderr().empty(),
              "stderr (win): read_stderr() hands each byte over once");
        check(!proc.stderr_open() && proc.stderr_fd() == -1,
              "stderr (win): once read to its end the stream is closed");
        check(proc.stderr_text() == "one\ntwo\n",
              "stderr (win): stderr_text() survives reading");
    }
    {
        // Bounded: 10 bytes too many, and the oldest 10 go.
        const std::size_t cap = process_stderr_capacity;
        ManagedProcess proc;
        std::string error;
        proc.spawn_capturing_stderr(
            {self, "--emit-stderr", "flood", std::to_string(cap)}, error);
        for (int i = 0; i < 500 && proc.running(); ++i)
            ::Sleep(10);
        const std::string expected =
            "[... 10 earlier bytes dropped ...]\n" + std::string(cap, 'B');
        check(proc.stderr_text() == expected,
              "stderr (win): past the capacity the oldest bytes go, and the"
              " text says how many");
        check(proc.read_stderr() == expected,
              "stderr (win): an unread backlog is bounded the same way");
    }
    {
        // Nobody reads, and the program writes 1 MiB -- far beyond an
        // anonymous pipe's buffer. Without the reader thread it would block in
        // WriteFile forever.
        ManagedProcess proc;
        std::string error;
        proc.spawn_capturing_stderr({self, "--emit-stderr", "flood", "1048576"}, error);
        bool exited = false;
        for (int i = 0; i < 500 && !exited; ++i) {
            exited = !proc.running();
            if (!exited)
                ::Sleep(10);
        }
        check(exited, "stderr (win): a program that floods stderr is never"
                      " blocked by it");
    }
}

// The child gets its three standard handles and no other inheritable handle
// of this process: a pipe left inheritable here -- another thread's child
// mid-spawn -- must still see EOF when its own writer closes, even while a
// long-lived child spawned in the meantime is running.
void test_children_inherit_only_their_own_handles_win()
{
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE stray_read = nullptr;
    HANDLE stray_write = nullptr;
    ::CreatePipe(&stray_read, &stray_write, &sa, 0); // both ends inheritable

    ManagedProcess plain;
    ManagedProcess capturing;
    std::string error;
    check(plain.spawn(kWinTree, error), "inherit (win): a plain spawn starts");
    check(capturing.spawn_capturing_stderr(kWinTree, error),
          "inherit (win): a capturing spawn starts");
    SystemCommandRunner runner(10);
    std::string out;
    runner.set_stderr_sink([](const std::string&) {});
    (void)runner.run({"cmd.exe", "/c", "exit 0"}, out, error);

    ::CloseHandle(stray_write);
    DWORD available = 0;
    const BOOL peek = ::PeekNamedPipe(stray_read, nullptr, 0, nullptr, &available,
                                      nullptr);
    const DWORD why = ::GetLastError();
    check(!peek && why == ERROR_BROKEN_PIPE,
          "inherit (win): no child holds another pipe's write end (broken pipe"
          " once ours closes)");
    ::CloseHandle(stray_read);
}

// --- Windows: SystemCommandRunner (the cookie / token fetch) ----------------

void test_command_runner_win()
{
    const std::string self = win_self_path().string();
    {
        SystemCommandRunner runner(10);
        std::vector<std::string> lines;
        runner.set_stderr_sink([&](const std::string& line) { lines.push_back(line); });
        std::string out;
        std::string error;
        const int rc = runner.run({self, "--emit-cookie", "7"}, out, error);
        check(rc == 7, "runner (win): the exit status is the program's ("
                           + std::to_string(rc) + ")");
        check(out == "c00kie-secret-value\n",
              "runner (win): stdout is captured byte for byte");
        check(lines
                  == std::vector<std::string> {"a warning on stderr",
                                               "then a CRLF line",
                                               "and an unterminated one"},
              "runner (win): stderr reaches the sink a line at a time, CR"
              " stripped, the last unterminated line flushed");
    }
    {
        SystemCommandRunner runner(1);
        std::string out;
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        const int rc = runner.run({self, "--print-pid-and-sleep"}, out, error);
        const auto took = std::chrono::steady_clock::now() - start;
        const DWORD pid = static_cast<DWORD>(std::strtoul(out.c_str(), nullptr, 10));
        check(rc == -1 && error == "command timed out after 1s",
              "runner (win): a hung command times out: " + error);
        check(took < std::chrono::seconds(5),
              "runner (win): ...at its deadline, not the program's");
        check(pid != 0 && win_gone(pid), "runner (win): the timed-out child is gone");
    }
    {
        SystemCommandRunner runner(30);
        CancelToken token;
        runner.set_cancel(&token);
        std::string out;
        std::string error;
        std::thread canceller([&] {
            ::Sleep(300);
            token.cancel();
        });
        const auto start = std::chrono::steady_clock::now();
        const int rc = runner.run({self, "--print-pid-and-sleep"}, out, error);
        const auto took = std::chrono::steady_clock::now() - start;
        canceller.join();
        const DWORD pid = static_cast<DWORD>(std::strtoul(out.c_str(), nullptr, 10));
        check(rc == -1 && error == "cancelled",
              "runner (win): a cancelled command reports \"cancelled\": " + error);
        check(took < std::chrono::seconds(3), "runner (win): ...promptly");
        check(pid != 0 && win_gone(pid), "runner (win): the cancelled child is gone");
    }
}

// --- Windows: SshTunnel against a fake ssh.exe ------------------------------

void test_tunnel_against_a_fake_ssh_win()
{
    SshConfig cfg;
    cfg.user = "baron";
    cfg.host = "h";
    {
        FakeSshWin ssh("ready");
        check(ssh.ok(), "tunnel (win): the fake ssh.exe is in place");
        TestDir records("records-win-ready");
        std::string port_error;
        const std::uint16_t port = pick_free_local_port(port_error);
        DWORD pid = 0;
        {
            SshTunnel tunnel(cfg, 10900, port);
            tunnel.set_record_dir(records.path);
            tunnel.set_ready_timeout_ms(5000);
            std::vector<std::string> lines;
            tunnel.set_stderr_sink([&](const std::string& line) { lines.push_back(line); });
            std::string error;
            const auto started = std::chrono::steady_clock::now();
            const bool up = tunnel.start(error);
            const auto took = std::chrono::steady_clock::now() - started;
            check(up && error.empty(),
                  "tunnel (win): ssh's report of the forward is readiness: " + error);
            check(took < std::chrono::seconds(3),
                  "tunnel (win): ready as soon as ssh says so");
            for (int i = 0; i < 100 && lines.empty(); ++i) {
                tunnel.drain_stderr();
                ::Sleep(10);
            }
            check(lines == std::vector<std::string> {"a line ssh prints by default"},
                  "tunnel (win): the sink gets the default-level line, CR"
                  " stripped, and no debug1 lines");
            check(contains_str(tunnel.stderr_text(),
                               "debug1: Entering interactive session.\r\n"),
                  "tunnel (win): stderr_text() keeps ssh's bytes, debug1 lines"
                  " and CRLF included");
            const auto argv = ssh.argv();
            check(index_of(argv, "LogLevel=DEBUG1") >= 0
                      && index_of(argv, "127.0.0.1:" + std::to_string(port)
                                            + ":127.0.0.1:10900")
                             >= 0,
                  "tunnel (win): ssh.exe was asked for DEBUG1 and the explicit"
                  " 127.0.0.1 bind");
            pid = ssh.wait_for_pid();
            check(pid != 0 && win_alive(pid) && tunnel.running(),
                  "tunnel (win): ssh.exe is running while the tunnel is up");
            check(win_children_named(L"ssh.exe").size() == 1,
                  "tunnel (win): exactly one ssh.exe child");
        } // disconnect: the tunnel goes out of scope
        check(pid != 0 && win_gone(pid),
              "tunnel (win): disconnecting (the tunnel's destructor) stops ssh.exe");
        check(win_children_named(L"ssh.exe").empty(),
              "tunnel (win): no ssh.exe child survives the disconnect");
    }
    {
        FakeSshWin ssh("authfail");
        TestDir records("records-win-authfail");
        SshTunnel tunnel(cfg, 10900, 0);
        tunnel.set_record_dir(records.path);
        tunnel.set_ready_timeout_ms(5000);
        std::vector<std::string> lines;
        tunnel.set_stderr_sink([&](const std::string& line) { lines.push_back(line); });
        std::string error;
        check(!tunnel.start(error), "tunnel (win): a refused key does not come up");
        check(contains_str(error, "ssh exited before the forward"),
              "tunnel (win): ...and says ssh exited: " + error);
        check(lines
                  == std::vector<std::string> {
                      "Warning: Permanently added '127.0.0.1' (ED25519) to the"
                      " list of known hosts.",
                      "nobody@127.0.0.1: Permission denied (publickey)."},
              "tunnel (win): ssh's last words reach the sink, exactly");
        check(contains_str(tunnel.stderr_text(),
                           "Permission denied (publickey).\r\n"),
              "tunnel (win): ...and stay in stderr_text()");
        const DWORD pid = ssh.wait_for_pid();
        check(pid != 0 && win_gone(pid) && !tunnel.running(),
              "tunnel (win): no ssh.exe is left after a failed start");
    }
    {
        FakeSshWin ssh("stall");
        TestDir records("records-win-stall");
        SshTunnel tunnel(cfg, 10900, 0);
        tunnel.set_record_dir(records.path);
        tunnel.set_ready_timeout_ms(800);
        std::string error;
        check(!tunnel.start(error) && contains_str(error, "timed out waiting"),
              "tunnel (win): ssh that never reports the forward times out: " + error);
        const DWORD pid = ssh.wait_for_pid();
        check(pid != 0 && win_gone(pid), "tunnel (win): the timed-out ssh.exe is gone");
    }
    {
        FakeSshWin ssh("stall");
        TestDir records("records-win-cancel");
        SshTunnel tunnel(cfg, 10900, 0);
        tunnel.set_record_dir(records.path);
        tunnel.set_ready_timeout_ms(20000);
        CancelToken token;
        tunnel.set_cancel(&token);
        std::thread canceller([&] {
            ::Sleep(300);
            token.cancel();
        });
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        const bool up = tunnel.start(error);
        const auto took = std::chrono::steady_clock::now() - start;
        canceller.join();
        check(!up && error == "cancelled" && took < std::chrono::seconds(3),
              "tunnel (win): Cancel ends the wait promptly: " + error);
        const DWORD pid = ssh.wait_for_pid();
        check(pid != 0 && win_gone(pid), "tunnel (win): the cancelled ssh.exe is gone");
    }
    {
        // The cookie fetch through the real runner and the fake ssh.exe.
        FakeSshWin ssh("ready");
        SystemCommandRunner runner(10);
        runner.set_stderr_sink([](const std::string&) {});
        std::string cookie;
        std::string error;
        check(fetch_session_cookie(runner, cfg, 10900, cookie, error)
                  && cookie == "c00kie-secret-value",
              "fetch (win): the session cookie comes back through ssh.exe: " + error);
    }
}

// A client that is killed outright, or exits without unwinding (ExitProcess),
// while its tunnel is up: no destructor runs, and the kill-on-close job alone
// must take ssh.exe down.
void test_a_dead_client_leaves_no_ssh_win()
{
    const std::string self = win_self_path().string();
    for (const char* how_name : {"--hold-tunnel-kill", "--hold-tunnel-exit"}) {
        const std::string how = how_name;
        FakeSshWin ssh("ready");
        SECURITY_ATTRIBUTES sa {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE read_pipe = nullptr;
        HANDLE write_pipe = nullptr;
        ::CreatePipe(&read_pipe, &write_pipe, &sa, 0);
        ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
        std::string command = "\"" + self + "\" " + how;
        STARTUPINFOA si {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write_pipe;
        si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi {};
        const BOOL ok = ::CreateProcessA(nullptr, command.data(), nullptr, nullptr,
                                         TRUE, 0, nullptr, nullptr, &si, &pi);
        ::CloseHandle(write_pipe);
        check(ok != FALSE, "dead client (win): the holder starts (" + how + ")");
        if (!ok) {
            ::CloseHandle(read_pipe);
            continue;
        }
        // The holder prints "up" once its tunnel is ready.
        std::string line;
        char c = 0;
        DWORD got = 0;
        while (::ReadFile(read_pipe, &c, 1, &got, nullptr) && got == 1 && c != '\n')
            line.push_back(c);
        ::CloseHandle(read_pipe);
        if (!line.empty() && line.back() == '\r')
            line.pop_back(); // std::cout is in text mode: "up\r\n"
        const DWORD pid = ssh.wait_for_pid();
        check(line == "up" && pid != 0 && win_alive(pid),
              "dead client (win): the holder's tunnel and ssh.exe are up (" + how
                  + ", holder said \"" + line + "\")");
        if (how == "--hold-tunnel-kill")
            ::TerminateProcess(pi.hProcess, 9); // no destructor, no cleanup code
        // else: the holder calls ExitProcess itself once it has said "up"
        ::WaitForSingleObject(pi.hProcess, 10000);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        check(pid != 0 && win_gone(pid),
              "dead client (win): ssh.exe dies with its client (" + how + ")");
    }
}

int hold_tunnel(bool exit_without_unwinding)
{
    SshConfig cfg;
    cfg.user = "baron";
    cfg.host = "h";
    SshTunnel tunnel(cfg, 10900, 0);
    tunnel.set_record_dir({});
    tunnel.set_ready_timeout_ms(5000);
    tunnel.set_stderr_sink([](const std::string&) {});
    std::string error;
    if (!tunnel.start(error)) {
        std::cout << "start failed: " << error << '\n' << std::flush;
        return 2;
    }
    std::cout << "up\n" << std::flush;
    if (exit_without_unwinding)
        ::ExitProcess(0); // the CRT and ~SshTunnel never run
    ::Sleep(INFINITE);
    return 0;
}

// --- real ssh.exe ------------------------------------------------------------

// The Windows built-in OpenSSH client, refused at the TCP connect: its own
// stderr line, through the Windows pipe, to the sink.
void test_real_ssh_refused_is_captured_win()
{
    SshConfig cfg;
    cfg.user = "nobody";
    cfg.host = "127.0.0.1";
    cfg.port = 1;
    SystemCommandRunner runner(30);
    std::vector<std::string> lines;
    runner.set_stderr_sink([&](const std::string& line) { lines.push_back(line); });
    std::string out;
    std::string error;
    const int rc = runner.run(build_ssh_exec_argv(cfg, "true"), out, error);
    if (rc == -1 && contains_str(error, "CreateProcess failed (2)")) {
        std::cerr << "SKIP: real ssh.exe checks (no ssh on PATH)\n";
        return;
    }
    check(rc == 255, "real ssh (win): a refused connect exits 255 ("
                         + std::to_string(rc) + ")");
    // Win32-OpenSSH connects asynchronously and reports the refusal only at
    // the banner exchange, in its own words (OpenSSH_for_Windows_9.5p2); the
    // portable OpenSSH wording is accepted too, exactly.
    const bool win32_wording =
        lines
        == std::vector<std::string> {
            "banner exchange: Connection to UNKNOWN port -1: Connection refused"};
    const bool portable_wording =
        lines
        == std::vector<std::string> {
            "ssh: connect to host 127.0.0.1 port 1: Connection refused"};
    check(win32_wording || portable_wording,
          "real ssh (win): ssh.exe's own refusal reaches the sink exactly"
              + std::string(lines.empty() ? " (no lines)" : ": " + lines[0]));
    std::string joined;
    for (const auto& line : lines)
        joined += line + "\n";
    check(classify_ssh_stderr(joined) == ConnectErrorCause::ssh_refused,
          "real ssh (win): and the classifier reads it as a refused SSH port");
    check(win_children_named(L"ssh.exe").empty(),
          "real ssh (win): no ssh.exe is left behind");
}

// With HAIKU_REMOTE_TEST_SSH_AUTHFAIL=user@host[:port] -- an sshd that will
// refuse this user's keys -- a real ssh.exe failing to authenticate, through
// the tunnel (reader thread) and the cookie fetch (runner) alike.
void test_real_ssh_authentication_failure_win()
{
    const std::string target = win_env("HAIKU_REMOTE_TEST_SSH_AUTHFAIL");
    if (target.empty()) {
        std::cerr << "SKIP: real ssh.exe authentication failure"
                     " (HAIKU_REMOTE_TEST_SSH_AUTHFAIL not set)\n";
        return;
    }
    SshConfig cfg;
    const auto at = target.find('@');
    const auto colon = target.rfind(':');
    cfg.user = target.substr(0, at);
    cfg.host = target.substr(at + 1, colon == std::string::npos || colon < at
                                         ? std::string::npos
                                         : colon - at - 1);
    cfg.port = colon == std::string::npos || colon < at
                   ? 22
                   : std::atoi(target.c_str() + colon + 1);
    TestDir scratch("real-authfail");
    cfg.known_hosts_file = (scratch.path / "known_hosts").string();
    {
        SshTunnel tunnel(cfg, 10900, 0);
        tunnel.set_record_dir(scratch.path / "records");
        std::vector<std::string> lines;
        tunnel.set_stderr_sink([&](const std::string& line) { lines.push_back(line); });
        std::string error;
        check(!tunnel.start(error), "real authfail (win): the tunnel does not come up");
        bool denied = false;
        for (const auto& line : lines)
            denied = denied || contains_str(line, "Permission denied");
        check(denied,
              "real authfail (win): ssh.exe's \"Permission denied\" reaches the"
              " tunnel's sink through the reader thread");
        check(contains_str(tunnel.stderr_text(), "Permission denied"),
              "real authfail (win): ...and stays in stderr_text()");
        check(!tunnel.running() && win_children_named(L"ssh.exe").empty(),
              "real authfail (win): no ssh.exe is left after the failed tunnel");
        std::string joined;
        for (const auto& line : lines) {
            joined += line + "\n";
            std::cerr << "      real ssh.exe (tunnel) said: [" << line << "]\n";
        }
        check(classify_ssh_stderr(joined) == ConnectErrorCause::ssh_auth_failed,
              "real authfail (win): the classifier reads it as an auth failure");
    }
    {
        SystemCommandRunner runner(30);
        std::vector<std::string> lines;
        runner.set_stderr_sink([&](const std::string& line) { lines.push_back(line); });
        std::string cookie;
        std::string error;
        check(!fetch_session_cookie(runner, cfg, 10900, cookie, error),
              "real authfail (win): the cookie fetch fails");
        bool denied = false;
        for (const auto& line : lines)
            denied = denied || contains_str(line, "Permission denied");
        check(denied, "real authfail (win): the fetch's sink gets \"Permission"
                      " denied\" from ssh.exe");
        for (const auto& line : lines)
            std::cerr << "      real ssh.exe (fetch) said: [" << line << "]\n";
        check(win_children_named(L"ssh.exe").empty(),
              "real authfail (win): no ssh.exe is left after the failed fetch");
    }
}


// HAIKU_REMOTE_TEST_SSH_TUNNEL=user@host[:port] (and, optionally,
// HAIKU_REMOTE_TEST_SSH_IDENTITY=<key>) names an sshd that accepts this key:
// a real ssh.exe -L forward, through every way a session ends.
std::optional<SshConfig> real_tunnel_config()
{
    const std::string target = win_env("HAIKU_REMOTE_TEST_SSH_TUNNEL");
    const auto at = target.find('@');
    if (target.empty() || at == std::string::npos)
        return std::nullopt;
    SshConfig cfg;
    const auto colon = target.rfind(':');
    const bool has_port = colon != std::string::npos && colon > at;
    cfg.user = target.substr(0, at);
    cfg.host = target.substr(at + 1, has_port ? colon - at - 1 : std::string::npos);
    cfg.port = has_port ? std::atoi(target.c_str() + colon + 1) : 22;
    cfg.identity_file = win_env("HAIKU_REMOTE_TEST_SSH_IDENTITY");
    cfg.known_hosts_file =
        (std::filesystem::temp_directory_path() / "haiku-remote-real-tunnel-known_hosts")
            .string();
    return cfg;
}

int hold_real_tunnel(bool exit_without_unwinding)
{
    const auto cfg = real_tunnel_config();
    if (!cfg)
        return 2;
    SshTunnel tunnel(*cfg, 10900, 0);
    tunnel.set_record_dir({});
    tunnel.set_stderr_sink([](const std::string&) {});
    std::string error;
    if (!tunnel.start(error)) {
        std::cout << "start failed: " << error << '\n' << std::flush;
        return 2;
    }
    std::cout << "up\n" << std::flush;
    if (exit_without_unwinding)
        ::ExitProcess(0);
    ::Sleep(INFINITE);
    return 0;
}

void test_real_ssh_tunnel_lifecycle_win()
{
    const auto cfg = real_tunnel_config();
    if (!cfg) {
        std::cerr << "SKIP: real ssh.exe tunnel lifecycle"
                     " (HAIKU_REMOTE_TEST_SSH_TUNNEL not set)\n";
        return;
    }
    {
        DWORD pid = 0;
        {
            SshTunnel tunnel(*cfg, 10900, 0);
            tunnel.set_record_dir({});
            std::vector<std::string> lines;
            tunnel.set_stderr_sink([&](const std::string& line) { lines.push_back(line); });
            std::string error;
            check(tunnel.start(error),
                  "real tunnel (win): ssh.exe reports the forward up: " + error);
            const auto children = win_children_named(L"ssh.exe");
            check(children.size() == 1 && tunnel.running(),
                  "real tunnel (win): one live ssh.exe child holds it");
            pid = children.empty() ? 0 : children[0];
            check(contains_str(tunnel.stderr_text(), "Local forwarding listening on 127.0.0.1 port "
                                                         + std::to_string(tunnel.local_port())),
                  "real tunnel (win): readiness came from ssh.exe's own DEBUG1 log");
        } // disconnect
        check(pid != 0 && win_gone(pid) && win_children_named(L"ssh.exe").empty(),
              "real tunnel (win): disconnecting stops ssh.exe");
    }
    const std::string self = win_self_path().string();
    for (const char* how_name : {"--hold-real-tunnel-kill", "--hold-real-tunnel-exit"}) {
        const std::string how = how_name;
        SECURITY_ATTRIBUTES sa {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE read_pipe = nullptr;
        HANDLE write_pipe = nullptr;
        ::CreatePipe(&read_pipe, &write_pipe, &sa, 0);
        ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
        std::string command = "\"" + self + "\" " + how;
        STARTUPINFOA si {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write_pipe;
        si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi {};
        const BOOL ok = ::CreateProcessA(nullptr, command.data(), nullptr, nullptr,
                                         TRUE, 0, nullptr, nullptr, &si, &pi);
        ::CloseHandle(write_pipe);
        check(ok != FALSE, "real tunnel (win): the holder starts (" + how + ")");
        if (!ok) {
            ::CloseHandle(read_pipe);
            continue;
        }
        // Its ssh.exe, found while the holder is alive and before it can exit.
        const DWORD ssh_pid = win_wait_for_child(pi.dwProcessId);
        std::string line;
        char c = 0;
        DWORD got = 0;
        while (::ReadFile(read_pipe, &c, 1, &got, nullptr) && got == 1 && c != '\n')
            line.push_back(c);
        ::CloseHandle(read_pipe);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        check(line == "up" && ssh_pid != 0,
              "real tunnel (win): the holder's real tunnel is up (" + how
                  + ", holder said \"" + line + "\")");
        if (how == "--hold-real-tunnel-kill")
            ::TerminateProcess(pi.hProcess, 9);
        ::WaitForSingleObject(pi.hProcess, 10000);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        check(ssh_pid != 0 && win_gone(ssh_pid),
              "real tunnel (win): the real ssh.exe dies with its client (" + how + ")");
    }
}

#endif // _WIN32

} // namespace

// --- the shared command-line launch arguments (launch_arguments.hpp) -------

struct Parsed {
    LaunchArguments arguments;
    LaunchTarget target = LaunchTarget::library;
    std::string error; // non-empty when parsing or resolving threw
};

Parsed parse_launch(std::vector<std::string> args)
{
    Parsed out;
    try {
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string argument = args[i];
            const auto value = [&]() -> std::string {
                if (++i >= args.size())
                    throw std::runtime_error("missing value for " + argument);
                return args[i];
            };
            if (!parse_launch_argument(out.arguments, argument, value))
                throw std::runtime_error("not a launch argument: " + argument);
        }
        out.target = resolve_launch_target(out.arguments);
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

void test_launch_arguments()
{
    // The documented backward-compatible invocation (issue #1): an endpoint
    // launch with exactly the host and port given, nothing else changed.
    {
        const Parsed p = parse_launch({"--host", "127.0.0.1", "--port", "10900"});
        check(p.error.empty() && p.target == LaunchTarget::endpoint,
              "--host/--port is an endpoint launch");
        check(p.arguments.transport.host == "127.0.0.1"
                  && p.arguments.transport.port == 10900
                  && p.arguments.transport.url.empty()
                  && p.arguments.transport.cookie.empty(),
              "--host/--port fill the transport exactly as before");
    }
    {
        const Parsed p = parse_launch({});
        check(p.error.empty() && p.target == LaunchTarget::library,
              "no connection argument opens the library");
    }
    {
        const Parsed p = parse_launch({"--direct", "--host", "10.1.2.3",
                                       "--remote-port", "2000"});
        check(p.error.empty() && p.target == LaunchTarget::endpoint
                  && p.arguments.transport.host == "10.1.2.3"
                  && p.arguments.transport.port == 2000,
              "--direct --remote-port N connects to host:N");
    }
    {
        const Parsed p = parse_launch({"--direct"});
        check(p.error.empty() && p.target == LaunchTarget::endpoint
                  && p.arguments.transport.host == "127.0.0.1"
                  && p.arguments.transport.port == 10900,
              "--direct alone is the default local endpoint");
    }
    {
        // --direct and a saved Direct TCP profile are one route: the same
        // host, port and cookie, and no ssh (an endpoint launch never runs it).
        TestDir dir("direct-cookie");
        const std::string cookie_file = (dir.path / "session_cookie.2000").string();
        std::ofstream(cookie_file) << "cli-direct-cookie\n";
        const Parsed p = parse_launch({"--direct", "--host", "10.1.2.3",
                                       "--remote-port", "2000", "--cookie-file",
                                       cookie_file});
        ConnectionProfile saved;
        saved.mode = ConnectionMode::direct;
        saved.host = "10.1.2.3";
        saved.remote_port = 2000;
        saved.cookie_source = cookie_file;
        const LaunchPlan plan = plan_launch(saved);
        const TransportOptions& cli = p.arguments.transport;
        check(p.error.empty() && p.target == LaunchTarget::endpoint
                  && cli.host == "10.1.2.3" && cli.port == 2000
                  && cli.cookie == "cli-direct-cookie" && cli.url.empty(),
              "--direct --cookie-file: TCP to host:N with the file's cookie");
        check(plan.steps.size() == 1 && plan.steps[0].kind == RouteKind::direct
                  && plan.steps[0].transport.host == cli.host
                  && plan.steps[0].transport.port == cli.port
                  && plan.steps[0].transport.cookie == cli.cookie
                  && plan.steps[0].transport.url.empty(),
              "--direct is the route a saved Direct TCP profile plans");
    }

    // --ssh-host: an unsaved ssh profile with every field from the flags.
    {
        const Parsed p = parse_launch(
            {"--ssh-host", "graviton.example", "--ssh-user", "felipe",
             "--ssh-port", "2222", "--identity", "~/.ssh/haiku",
             "--remote-port", "10901", "--local-port", "15900"});
        check(p.error.empty() && p.target == LaunchTarget::ssh_profile,
              "--ssh-host is a tunnel launch");
        const ConnectionProfile profile = ssh_launch_profile(p.arguments, 1024, 700);
        check(profile.mode == ConnectionMode::ssh, "--ssh-host -> ssh mode");
        check(profile.host == "graviton.example" && profile.name == "graviton.example",
              "--ssh-host names the host and the session");
        check(profile.ssh_user == "felipe", "--ssh-user");
        check(profile.ssh_port == 2222, "--ssh-port");
        check(profile.identity_file == "~/.ssh/haiku",
              "--identity is kept literal (expanded only when ssh runs)");
        check(profile.remote_port == 10901, "--remote-port");
        check(profile.local_port == std::optional<int>(15900), "--local-port");
        check(profile.cookie_source.empty(),
              "no --cookie-file: the cookie is fetched over SSH");
        check(profile.width == 1024 && profile.height == 700,
              "the session size is passed through");
        check(profile.route_summary()
                  == "client -> 127.0.0.1:15900 -> ssh -> graviton.example:2222"
                     " -> 127.0.0.1:10901",
              "the --ssh-host route reads as the editor would show it");
    }
    {
        const Parsed p = parse_launch({"--ssh-host", "h"});
        const ConnectionProfile profile = ssh_launch_profile(p.arguments, 1280, 800);
        check(profile.ssh_user == "baron" && profile.ssh_port == 22
                  && profile.remote_port == 10900 && !profile.local_port
                  && profile.identity_file.empty(),
              "--ssh-host alone takes the profile defaults"
              " (baron, 22, 10900, automatic local port, no key)");
    }

    // Mixed targets are refused, never silently resolved.
    const std::vector<std::vector<std::string>> refused = {
        {"--ssh-host", "h", "--host", "x"},
        {"--ssh-host", "h", "--port", "1"},
        {"--ssh-host", "h", "--direct"},
        {"--ssh-host", "h", "--cookie", "c"},
        {"--ssh-host", "h", "--token", "t"},
        {"--ssh-host", "h", "--insecure"},
        {"--ssh-user", "u"},
        {"--identity", "k"},
        {"--ssh-port", "22"},
        {"--local-port", "15900"},
        {"--direct", "--local-port", "15900"},
        {"--profile", "P", "--port", "1"},
        {"--profile", "P", "--ssh-host", "h"},
        {"--profile", "P", "--direct"},
        {"--profile", "P", "--remote-port", "1"},
        {"--port", "1", "--remote-port", "2"},
        {"--url", "tcp://h:1", "--remote-port", "2"},
        {"--ssh-host", "h", "--ssh-port", "0"},
        {"--ssh-host", "h", "--ssh-port", "65536"},
        {"--ssh-host", "h", "--local-port", "x"},
        {"--ssh-host", ""},
    };
    for (const auto& args : refused) {
        std::string joined;
        for (const auto& a : args)
            joined += (joined.empty() ? "" : " ") + a;
        check(!parse_launch(args).error.empty(), "refused: " + joined);
    }

    {
        const Parsed p = parse_launch({"--profile", "Lab", "--profiles", "/x.json"});
        check(p.error.empty() && p.target == LaunchTarget::saved_profile
                  && p.arguments.profile == "Lab"
                  && p.arguments.profiles_file == "/x.json",
              "--profile NAME --profiles FILE is a saved-profile launch");
    }
    {
        const Parsed p = parse_launch({"--profiles", "/x.json"});
        check(p.error.empty() && p.target == LaunchTarget::library,
              "--profiles alone opens that library");
    }

    // --cookie-file with --ssh-host: the PATH becomes the cookie source.
    {
        const std::filesystem::path file
            = std::filesystem::temp_directory_path() / "haiku-remote-launch-cookie";
        {
            std::ofstream out(file);
            out << "abc123\n";
        }
        const Parsed p = parse_launch({"--ssh-host", "h", "--cookie-file",
                                       file.string()});
        check(p.error.empty() && p.target == LaunchTarget::ssh_profile,
              "--ssh-host --cookie-file is a tunnel launch");
        check(ssh_launch_profile(p.arguments, 1280, 800).cookie_source
                  == file.string(),
              "--cookie-file's path is the profile's cookie source");
        // ...and an endpoint launch still gets the cookie's bytes, as before.
        const Parsed e = parse_launch({"--cookie-file", file.string()});
        check(e.error.empty() && e.target == LaunchTarget::endpoint
                  && e.arguments.transport.cookie == "abc123",
              "--cookie-file alone is still an endpoint launch with that cookie");
        std::filesystem::remove(file);
    }

    std::vector<ConnectionProfile> profiles(2);
    profiles[0].id = "lab";
    profiles[0].name = "first";
    profiles[1].id = "second-id";
    profiles[1].name = "lab";
    check(find_profile(profiles, "lab") == &profiles[0],
          "an id match wins over a name match");
    check(find_profile(profiles, "first") == &profiles[0], "a name match");
    check(find_profile(profiles, "nope") == nullptr, "no match is nullptr");
}

int main(int argc, char** argv)
{
#if defined(_WIN32)
    // A copy of this binary named ssh.exe is the fake ssh.
    if (win_self_path().stem() == "ssh")
        return fake_ssh_main(argc, argv);
    if (argc > 1 && std::string_view(argv[1]) == "--hold-child")
        return hold_child_forever();
    if (argc > 1 && std::string_view(argv[1]) == "--hold-tunnel-kill")
        return hold_tunnel(false);
    if (argc > 1 && std::string_view(argv[1]) == "--hold-tunnel-exit")
        return hold_tunnel(true);
    if (argc > 1 && std::string_view(argv[1]) == "--hold-real-tunnel-kill")
        return hold_real_tunnel(false);
    if (argc > 1 && std::string_view(argv[1]) == "--hold-real-tunnel-exit")
        return hold_real_tunnel(true);
    if (argc > 1) {
        const int rc = helper_main(argc, argv);
        if (rc != -1)
            return rc;
    }
#else
    (void)argc;
    (void)argv;
#endif
    test_ssh_argv_builders();
    test_launch_arguments();
    test_broker_credentials_parse();
    test_session_cookie_parse();
    test_open_connection_broker();
    test_open_connection_broker_failure_is_terminal_for_wss();
#if !defined(_WIN32)
    test_managed_process_is_reaped();
    test_managed_process_reaped_by_destructor();
    test_managed_process_dies_with_a_sigkilled_owner();
    test_a_program_that_exits_is_noticed();
#else
    test_managed_process_is_reaped_win();
    test_managed_process_reaped_by_destructor_win();
    test_managed_process_survives_no_holder_kill_win(argv[0]);
    test_stderr_is_captured_bounded_and_drained_win();
    test_children_inherit_only_their_own_handles_win();
    test_command_runner_win();
    test_tunnel_against_a_fake_ssh_win();
    test_a_dead_client_leaves_no_ssh_win();
    test_real_ssh_refused_is_captured_win();
    test_real_ssh_authentication_failure_win();
    test_real_ssh_tunnel_lifecycle_win();
#endif
    test_ssh_tunnel_unreachable_leaves_no_child();
    test_pick_free_local_port();
    test_forward_readiness_from_ssh_stderr();
#if !defined(_WIN32)
    test_stderr_is_captured_bounded_and_drained();
    test_tunnel_readiness_never_connects_through_the_forward();
    test_tunnel_readiness_failures();
    test_process_identity_and_records();
#if defined(__linux__) || defined(__APPLE__)
    test_tunnel_start_sweeps_verified_orphans();
    test_sweep_never_signals_an_unrelated_process();
    test_an_orphan_of_a_killed_owner_and_guard_is_swept();
    test_a_killed_guard_takes_its_program_down_with_it();
#endif
#if defined(__linux__)
    test_pdeathsig_stops_the_program_of_a_killed_guard();
#endif
#endif

    if (failures == 0) {
        std::cout << "PASS - " << checks << " tunnel/transport checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " tunnel/transport checks FAILED\n";
    return 1;
}
