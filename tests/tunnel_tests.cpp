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

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/profile_launch.hpp"

#include <chrono>
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
#include <cstdlib>
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

    const auto base = build_ssh_base_argv(cfg);
    check(base.front() == "ssh", "base argv starts with ssh");
    check(contains(base, "BatchMode=yes"), "base is non-interactive (BatchMode)");
    check(contains(base, "StrictHostKeyChecking=accept-new"),
          "base pins host keys TOFU-style");
    check(contains(base, "ConnectTimeout=10"), "base bounds the TCP connect");
    check(contains(base, "-p") && contains(base, "2222"),
          "base passes a non-default ssh port");
    check(contains(base, "-i") && contains(base, "/keys/id_ed25519"),
          "base passes the identity file");
    check(contains(base, "IdentitiesOnly=yes"),
          "an explicit key suppresses agent keys");
    check(contains(base, "UserKnownHostsFile=/keys/known_hosts"),
          "base passes a per-profile known_hosts file");

    const auto tunnel = build_ssh_tunnel_argv(cfg, 10900, "127.0.0.1", 10901);
    check(contains(tunnel, "-N"), "a tunnel runs no remote command (-N)");
    check(contains(tunnel, "-L"), "a tunnel requests a local forward (-L)");
    check(contains(tunnel, "10900:127.0.0.1:10901"),
          "the forward maps the chosen local port to the remote port");
    check(contains(tunnel, "ExitOnForwardFailure=yes"),
          "a tunnel fails loudly if the forward cannot be set up");
    check(!contains(tunnel, "-f"),
          "the tunnel is NOT backgrounded by ssh -- we own it in the foreground");
    check(tunnel.back() == "baron@graviton.example",
          "the tunnel destination is user@host and comes last");

    const auto exec = build_ssh_exec_argv(cfg, "cat /some/file");
    check(exec.back() == "cat /some/file", "the remote command is the last arg");
    check(exec[exec.size() - 2] == "baron@graviton.example",
          "user@host precedes the remote command");

    SshConfig plain;
    plain.host = "h";
    plain.port = 22;
    const auto plain_base = build_ssh_base_argv(plain);
    check(!contains(plain_base, "-p"), "the default ssh port is not spelled out");
    check(!contains(plain_base, "-i"), "no -i without an identity file");
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
    std::string error;
    const bool ok = tunnel.start(error);
    check(!ok, "a tunnel to a closed ssh port does not come up");
    check(!error.empty(), "the failure is reported with a message");
    check(!tunnel.running(),
          "no ssh child is left running after a failed tunnel start");
}

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

#endif // _WIN32

} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    if (argc > 1 && std::string_view(argv[1]) == "--hold-child")
        return hold_child_forever();
#else
    (void)argc;
    (void)argv;
#endif
    test_ssh_argv_builders();
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
#endif
    test_ssh_tunnel_unreachable_leaves_no_child();
    test_pick_free_local_port();

    if (failures == 0) {
        std::cout << "PASS - " << checks << " tunnel/transport checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " tunnel/transport checks FAILED\n";
    return 1;
}
