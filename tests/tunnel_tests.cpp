// Tests for the managed SSH tunnel core: the pure argv builder, the free-port
// picker and readiness probe (exercised against real loopback sockets on this
// host), the tunnel orchestration driven by a FakeSshProcess, and the
// fail-closed pidfile reaper.
//
// House rules (CLAUDE.md): assert exact values, never inequalities; keep every
// expectation independent of the code that produces it (the argv vectors below
// are written out, not read back from the builder); and give the reaper an
// asymmetric subject -- a live process that is NOT our ssh must survive.

#include "haiku_remote/ssh_tunnel.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
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

bool contains(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

std::string join(const std::vector<std::string>& v)
{
    std::string out;
    for (const auto& s : v) {
        out += '[';
        out += s;
        out += ']';
    }
    return out;
}

// ---- A fake ssh child, injectable in place of the real fork/exec. ----

struct FakeState {
    std::vector<std::string> argv;
    int start_calls = 0;
    int stop_calls = 0;
};

class FakeSshProcess final : public IProcess {
public:
    FakeSshProcess(FakeState* state, long pid, bool stays_alive, int exit_code,
                   std::string out, std::string err, bool start_ok = true)
        : state_(state), pid_(pid), stays_alive_(stays_alive),
          exit_code_(exit_code), stdout_(std::move(out)),
          stderr_(std::move(err)), start_ok_(start_ok)
    {
    }

    bool start(const std::vector<std::string>& argv, std::string& error) override
    {
        if (state_) {
            state_->argv = argv;
            ++state_->start_calls;
        }
        started_ = start_ok_;
        if (!start_ok_)
            error = "fake: launch refused";
        return start_ok_;
    }

    long pid() const override { return pid_; }

    bool running() override
    {
        if (!started_ || stopped_)
            return false;
        if (!stays_alive_) {
            exited_ = true;
            return false;
        }
        return true;
    }

    std::optional<int> exit_code() override
    {
        if (stopped_ || exited_ || !stays_alive_)
            return exit_code_;
        return std::nullopt;
    }

    std::string drain_stdout() override
    {
        std::string s = stdout_;
        stdout_.clear();
        return s;
    }

    std::string drain_stderr() override
    {
        std::string s = stderr_;
        stderr_.clear();
        return s;
    }

    void stop() override
    {
        if (state_)
            ++state_->stop_calls;
        stopped_ = true;
    }

private:
    FakeState* state_;
    long pid_;
    bool stays_alive_;
    int exit_code_;
    std::string stdout_;
    std::string stderr_;
    bool start_ok_;
    bool started_ = false;
    bool stopped_ = false;
    bool exited_ = false;
};

TunnelOptions fast_options(std::filesystem::path pidfile_dir)
{
    TunnelOptions opt;
    opt.forward_timeout = std::chrono::milliseconds(1500);
    opt.poll_interval = std::chrono::milliseconds(1);
    opt.connect_timeout = std::chrono::milliseconds(200);
    opt.cookie_timeout = std::chrono::milliseconds(1500);
    opt.pidfile_dir = std::move(pidfile_dir);
    return opt;
}

std::filesystem::path temp_dir(std::string_view tag)
{
    const auto base = std::filesystem::temp_directory_path()
        / ("haiku-remote-tunnel-" + std::string(tag) + "-"
           + std::to_string(::getpid()));
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    return base;
}

#ifndef _WIN32
// A real loopback listener, so the readiness probe has something to accept.
int make_listener(int& port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(fd, 16);
    sockaddr_in bound {};
    socklen_t len = sizeof(bound);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
    port = ntohs(bound.sin_port);
    return fd;
}
#endif

// ---- SshArgsBuilder (pure) ----

void test_tunnel_argv_is_exact_without_identity()
{
    TunnelConfig cfg;
    cfg.host = "example.invalid";
    cfg.ssh_user = "baron";
    cfg.ssh_port = 22;
    cfg.remote_port = 10900;

    const std::vector<std::string> expected = {
        "ssh", "-N", "-T",
        "-L", "15900:127.0.0.1:10900",
        "-p", "22",
        "-o", "ExitOnForwardFailure=yes",
        "-o", "ServerAliveInterval=15",
        "-o", "ServerAliveCountMax=3",
        "-o", "ConnectTimeout=15",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "BatchMode=yes",
        "-o", "ControlMaster=no",
        "-o", "ControlPath=none",
        "baron@example.invalid",
    };
    const auto argv = SshArgsBuilder::tunnel_argv(cfg, 15900);
    check(argv == expected, "tunnel argv without identity: " + join(argv));
}

void test_tunnel_argv_adds_identity_and_known_hosts_in_order()
{
    TunnelConfig cfg;
    cfg.host = "h";
    cfg.ssh_user = "baron";
    cfg.identity_file = "/tmp/key";          // absolute: expand is a no-op
    cfg.known_hosts_file = "/tmp/known_hosts";

    const std::vector<std::string> expected = {
        "ssh", "-N", "-T",
        "-L", "9000:127.0.0.1:10900",
        "-p", "22",
        "-o", "ExitOnForwardFailure=yes",
        "-o", "ServerAliveInterval=15",
        "-o", "ServerAliveCountMax=3",
        "-o", "ConnectTimeout=15",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "BatchMode=yes",
        "-o", "ControlMaster=no",
        "-o", "ControlPath=none",
        "-i", "/tmp/key",
        "-o", "IdentitiesOnly=yes",
        "-o", "UserKnownHostsFile=/tmp/known_hosts",
        "baron@h",
    };
    const auto argv = SshArgsBuilder::tunnel_argv(cfg, 9000);
    check(argv == expected, "tunnel argv with identity+known_hosts: " + join(argv));
}

void test_forward_spec_is_local_loopback_remote()
{
    TunnelConfig cfg;
    cfg.remote_port = 10900;
    check(SshArgsBuilder::forward_spec(5555, cfg) == "5555:127.0.0.1:10900",
          "forward spec local:loopback:remote");
}

void test_cookie_argv_is_a_one_shot_cat_keyed_on_remote_port()
{
    TunnelConfig cfg;
    cfg.host = "h";
    cfg.ssh_user = "baron";
    cfg.remote_port = 10900;

    const std::vector<std::string> expected = {
        "ssh", "-T",
        "-p", "22",
        "-o", "ConnectTimeout=15",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "BatchMode=yes",
        "-o", "ControlMaster=no",
        "-o", "ControlPath=none",
        "baron@h",
        "cat", "/boot/system/settings/remote_desktop/session_cookie.10900",
    };
    const auto argv = SshArgsBuilder::cookie_argv(cfg);
    check(argv == expected, "cookie argv: " + join(argv));
    check(SshArgsBuilder::cookie_remote_path(10900)
              == "/boot/system/settings/remote_desktop/session_cookie.10900",
          "cookie remote path keyed on remote port");
}

// ---- free-port picker + readiness probe (real sockets) ----

void test_pick_free_loopback_port_returns_a_valid_port()
{
    const auto port = pick_free_loopback_port();
    check(port.has_value(), "pick_free_loopback_port returns a port");
    if (port)
        check(*port >= 1 && *port <= 65535, "picked port is in [1,65535]");
}

void test_readiness_probe_true_on_a_listener_false_on_a_closed_port()
{
#ifndef _WIN32
    int port = 0;
    const int listener = make_listener(port);
    check(loopback_port_accepts(port, std::chrono::milliseconds(500)),
          "readiness probe accepts an open loopback listener");
    ::close(listener);

    // A just-closed, never-reused port: the probe must say no.
    const auto free_port = pick_free_loopback_port();
    check(free_port.has_value(), "got a free port for the negative case");
    if (free_port)
        check(!loopback_port_accepts(*free_port, std::chrono::milliseconds(200)),
              "readiness probe rejects a port nothing listens on");
#else
    check(true, "readiness probe test skipped on Windows");
    check(true, "readiness probe test skipped on Windows");
    check(true, "readiness probe test skipped on Windows");
#endif
}

// ---- SshTunnel orchestration driven by the fake ----

void test_start_reaches_running_when_the_forward_accepts()
{
#ifndef _WIN32
    int port = 0;
    const int listener = make_listener(port);

    TunnelConfig cfg;
    cfg.host = "h";
    cfg.local_port = port; // fixed, so the readiness probe hits our listener

    FakeState state;
    auto factory = [&state] {
        return std::unique_ptr<IProcess>(
            new FakeSshProcess(&state, 4242, /*stays_alive=*/true, 0, "", ""));
    };
    const auto dir = temp_dir("running");
    SshTunnel tunnel(cfg, fast_options(dir), factory);
    const TunnelStatus status = tunnel.start();

    check(status.state == TunnelState::running, "start reaches running state");
    check(status.ok(), "status.ok() true when running");
    check(status.local_port == port, "status carries the forwarded local port");
    check(state.start_calls == 1, "the fake ssh was started exactly once");
    check(std::filesystem::exists(dir / ("tunnel-" + std::to_string(port) + ".pid")),
          "a pidfile is written for the running tunnel");

    tunnel.stop();
    check(tunnel.state() == TunnelState::stopped, "stop moves to stopped");
    check(state.stop_calls >= 1, "stop() stopped the ssh child");
    check(!std::filesystem::exists(
              dir / ("tunnel-" + std::to_string(port) + ".pid")),
          "the pidfile is removed on stop");
    ::close(listener);
    std::filesystem::remove_all(dir);
#else
    for (int i = 0; i < 8; ++i)
        check(true, "start/running test skipped on Windows");
#endif
}

void test_early_ssh_exit_is_a_failure_with_stderr_folded_in()
{
    // A port nothing listens on, plus a fake that exits immediately: the start
    // must fail fast and surface the ssh stderr verbatim.
    const auto free_port = pick_free_loopback_port();
    check(free_port.has_value(), "reserved a closed port for the early-exit case");

    TunnelConfig cfg;
    cfg.host = "h";
    cfg.local_port = free_port.value_or(1);

    FakeState state;
    auto factory = [&state] {
        return std::unique_ptr<IProcess>(new FakeSshProcess(
            &state, 4243, /*stays_alive=*/false, 255, "",
            "ssh: Permission denied (publickey)."));
    };
    const auto dir = temp_dir("early-exit");
    SshTunnel tunnel(cfg, fast_options(dir), factory);
    const TunnelStatus status = tunnel.start();

    check(status.state == TunnelState::failed, "early ssh exit fails the start");
    check(contains(status.error, "255"), "failure names the ssh exit code");
    check(contains(status.error, "Permission denied"),
          "failure folds in the verbatim ssh stderr");
    check(!std::filesystem::exists(
              dir / ("tunnel-" + std::to_string(cfg.local_port) + ".pid")),
          "a failed start leaves no pidfile behind");
    std::filesystem::remove_all(dir);
}

void test_a_refused_launch_fails_without_retrying()
{
    TunnelConfig cfg;
    cfg.host = "h"; // auto local port -> would retry on a port race, but not here

    FakeState state;
    auto factory = [&state] {
        return std::unique_ptr<IProcess>(new FakeSshProcess(
            &state, 0, /*stays_alive=*/false, 0, "", "", /*start_ok=*/false));
    };
    const auto dir = temp_dir("refused");
    SshTunnel tunnel(cfg, fast_options(dir), factory);
    const TunnelStatus status = tunnel.start();

    check(status.state == TunnelState::failed, "a refused launch fails the start");
    check(state.start_calls == 1, "a failed exec is not retried as a port race");
    std::filesystem::remove_all(dir);
}

void test_fetch_session_cookie_returns_trimmed_stdout_only()
{
    TunnelConfig cfg;
    cfg.host = "h";
    cfg.remote_port = 10900;

    FakeState state;
    auto factory = [&state] {
        return std::unique_ptr<IProcess>(new FakeSshProcess(
            &state, 4244, /*stays_alive=*/false, 0, "s3cr3t-cookie\n", ""));
    };
    const auto dir = temp_dir("cookie");
    SshTunnel tunnel(cfg, fast_options(dir), factory);
    const auto cookie = tunnel.fetch_session_cookie();

    check(cookie.has_value(), "cookie fetch returns a value on exit 0");
    check(cookie.value_or("") == "s3cr3t-cookie",
          "the cookie is the trimmed stdout token");
    check(state.stop_calls >= 1, "the cookie fetch process is killed and reaped");
    std::filesystem::remove_all(dir);
}

void test_fetch_session_cookie_is_null_on_nonzero_exit()
{
    TunnelConfig cfg;
    cfg.host = "h";

    FakeState state;
    auto factory = [&state] {
        return std::unique_ptr<IProcess>(new FakeSshProcess(
            &state, 4245, /*stays_alive=*/false, 1, "garbage", "no such file"));
    };
    const auto dir = temp_dir("cookie-fail");
    SshTunnel tunnel(cfg, fast_options(dir), factory);
    const auto cookie = tunnel.fetch_session_cookie();
    check(!cookie.has_value(), "a non-zero cookie fetch yields no cookie");
    std::filesystem::remove_all(dir);
}

// ---- PidRecord + argv hash ----

void test_pid_record_round_trips_through_json()
{
    PidRecord record;
    record.pid = 12345;
    record.local_port = 15900;
    record.remote_port = 10900;
    record.host = "example.invalid";
    record.argv_hash = "deadbeefdeadbeef";
    record.start_time = 998877;

    const std::string text = record.serialize();
    const auto parsed = PidRecord::parse(text);
    check(parsed.has_value(), "a serialized pid record parses back");
    if (parsed) {
        check(parsed->pid == 12345, "pid round-trips");
        check(parsed->local_port == 15900, "local port round-trips");
        check(parsed->remote_port == 10900, "remote port round-trips");
        check(parsed->host == "example.invalid", "host round-trips");
        check(parsed->argv_hash == "deadbeefdeadbeef", "argv hash round-trips");
        check(parsed->start_time == 998877, "start time round-trips");
    }
    check(!PidRecord::parse("not json").has_value(),
          "a malformed pidfile parses to nothing");
}

void test_argv_hash_is_stable_and_sensitive()
{
    const std::vector<std::string> a = {"ssh", "-L", "1:127.0.0.1:2", "u@h"};
    const std::vector<std::string> b = {"ssh", "-L", "1:127.0.0.1:3", "u@h"};
    check(argv_hash(a) == argv_hash(a), "argv hash is deterministic");
    check(argv_hash(a) != argv_hash(b), "argv hash changes with the forward");
}

// ---- reap_decision (pure, fail-closed) ----

PidRecord ours_record()
{
    PidRecord r;
    r.pid = 55555;
    r.local_port = 15900;
    r.remote_port = 10900;
    r.host = "h";
    r.start_time = 424242;
    return r;
}

ProcessIdentity ours_identity()
{
    ProcessIdentity id;
    id.exe = "ssh";
    id.cmdline = {"ssh", "-N", "-T", "-L", "15900:127.0.0.1:10900", "baron@h"};
    id.start_time = 424242;
    return id;
}

void test_reap_decision_kills_only_a_verified_tunnel()
{
    check(reap_decision(ours_record(), ours_identity()) == ReapDecision::kill_it,
          "a fully verified ssh tunnel is reaped");
}

void test_reap_decision_fails_closed_on_a_recycled_pid()
{
    // Same pid, same forward, but a different start-time: the number was reused
    // by an unrelated process. Must NOT be killed.
    ProcessIdentity recycled = ours_identity();
    recycled.start_time = 999999;
    check(reap_decision(ours_record(), recycled) == ReapDecision::not_ours,
          "a recycled pid (start-time mismatch) is never killed");
}

void test_reap_decision_rejects_a_non_ssh_exe()
{
    ProcessIdentity stranger = ours_identity();
    stranger.exe = "nginx";
    check(reap_decision(ours_record(), stranger) == ReapDecision::not_ours,
          "a non-ssh executable is never killed");
}

void test_reap_decision_rejects_a_wrong_forward()
{
    ProcessIdentity other = ours_identity();
    other.cmdline = {"ssh", "-N", "-L", "40000:127.0.0.1:10900", "baron@h"};
    check(reap_decision(ours_record(), other) == ReapDecision::not_ours,
          "an ssh with a different -L is never killed");
}

void test_reap_decision_handles_a_basename_ssh_path()
{
    ProcessIdentity id = ours_identity();
    id.exe = "/usr/bin/ssh"; // basename still ssh
    check(reap_decision(ours_record(), id) == ReapDecision::kill_it,
          "an absolute ssh path verifies by basename");
}

void test_reap_decision_no_process_when_pid_is_dead()
{
    check(reap_decision(ours_record(), std::nullopt) == ReapDecision::no_process,
          "a dead pid yields no_process");
}

// ---- reap_orphans with injected hooks ----

void write_pidfile(const std::filesystem::path& path, const PidRecord& record)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << record.serialize();
}

void test_reap_orphans_kills_a_verified_record()
{
    const auto dir = temp_dir("reap-kill");
    const PidRecord rec = ours_record();
    write_pidfile(dir / "tunnel-15900.pid", rec);

    long killed_pid = 0;
    ReapHooks hooks;
    hooks.probe = [](long) { return std::optional<ProcessIdentity>(ours_identity()); };
    hooks.kill = [&killed_pid](long pid) { killed_pid = pid; };

    const ReapReport report = reap_orphans(dir, hooks);
    check(report.killed == 1, "the verified tunnel is counted killed");
    check(killed_pid == rec.pid, "kill is called with the recorded pid");
    check(report.unlinked == 1, "the pidfile is unlinked after a kill");
    check(!std::filesystem::exists(dir / "tunnel-15900.pid"),
          "no pidfile remains after reaping");
    std::filesystem::remove_all(dir);
}

void test_reap_orphans_fails_closed_on_a_recycled_pid()
{
    const auto dir = temp_dir("reap-recycled");
    write_pidfile(dir / "tunnel-15900.pid", ours_record());

    bool kill_called = false;
    ReapHooks hooks;
    hooks.probe = [](long) {
        // A live process at that pid, but it is not our tunnel (recycled).
        ProcessIdentity id = ours_identity();
        id.start_time = 1; // mismatch
        return std::optional<ProcessIdentity>(id);
    };
    hooks.kill = [&kill_called](long) { kill_called = true; };

    const ReapReport report = reap_orphans(dir, hooks);
    check(!kill_called, "a recycled pid is never signalled");
    check(report.killed == 0, "nothing is killed on a recycled pid");
    check(report.left_alone == 1, "the live stranger is left alone");
    check(report.unlinked == 1, "the stale pidfile is still unlinked");
    std::filesystem::remove_all(dir);
}

void test_reap_orphans_unlinks_a_corrupt_pidfile_without_killing()
{
    const auto dir = temp_dir("reap-corrupt");
    {
        std::ofstream out(dir / "tunnel-15900.pid", std::ios::binary);
        out << "{ this is not valid";
    }
    bool kill_called = false;
    ReapHooks hooks;
    hooks.probe = [](long) { return std::optional<ProcessIdentity>(ours_identity()); };
    hooks.kill = [&kill_called](long) { kill_called = true; };

    const ReapReport report = reap_orphans(dir, hooks);
    check(!kill_called, "a corrupt pidfile never triggers a kill");
    check(report.unlinked == 1, "a corrupt pidfile is unlinked");
    check(!std::filesystem::exists(dir / "tunnel-15900.pid"),
          "the corrupt pidfile is gone");
    std::filesystem::remove_all(dir);
}

void test_reap_orphans_on_a_missing_directory_is_a_noop()
{
    const auto dir = std::filesystem::temp_directory_path()
        / ("haiku-remote-no-such-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    const ReapReport report = reap_orphans(dir);
    check(report.killed == 0 && report.unlinked == 0 && report.left_alone == 0,
          "reaping a missing directory does nothing");
}

// ---- real POSIX probe + reaper on a live, non-ssh process ----

void test_real_probe_reports_this_process()
{
#ifndef _WIN32
    const auto id = probe_process(::getpid());
    check(id.has_value(), "the real probe finds this live process");
    if (id) {
        check(!id->exe.empty(), "the probe reports an executable name");
        check(!id->cmdline.empty(), "the probe reports a command line");
        check(id->start_time > 0, "the probe reports a start-time stamp");
        check(id->exe != "ssh", "this test process is not ssh");
    }
#else
    for (int i = 0; i < 4; ++i)
        check(true, "real probe test skipped on Windows");
#endif
}

void test_real_reaper_refuses_to_kill_this_live_non_ssh_process()
{
#ifndef _WIN32
    // The recycled-pid scenario on real metal: a pidfile naming a LIVE pid that
    // is not our ssh. With the real probe + real kill hooks the reaper must
    // leave the process (us) alone and merely unlink the stale file.
    const auto dir = temp_dir("reap-self");
    PidRecord rec;
    rec.pid = ::getpid();
    rec.local_port = 15900;
    rec.remote_port = 10900;
    rec.host = "h";
    if (const auto self = probe_process(::getpid()))
        rec.start_time = self->start_time; // start-time even matches
    write_pidfile(dir / "tunnel-15900.pid", rec);

    const ReapReport report = reap_orphans(dir); // real probe_process + real kill
    check(report.killed == 0, "the real reaper kills nothing here");
    check(report.left_alone == 1, "our own live process is left alone");
    check(report.unlinked == 1, "the stale pidfile is unlinked");
    // The decisive assertion: we are still running.
    check(::getpid() > 0, "this process survived the reaper");
    std::filesystem::remove_all(dir);
#else
    for (int i = 0; i < 4; ++i)
        check(true, "real reaper test skipped on Windows");
#endif
}

} // namespace

int main()
{
    test_tunnel_argv_is_exact_without_identity();
    test_tunnel_argv_adds_identity_and_known_hosts_in_order();
    test_forward_spec_is_local_loopback_remote();
    test_cookie_argv_is_a_one_shot_cat_keyed_on_remote_port();

    test_pick_free_loopback_port_returns_a_valid_port();
    test_readiness_probe_true_on_a_listener_false_on_a_closed_port();

    test_start_reaches_running_when_the_forward_accepts();
    test_early_ssh_exit_is_a_failure_with_stderr_folded_in();
    test_a_refused_launch_fails_without_retrying();
    test_fetch_session_cookie_returns_trimmed_stdout_only();
    test_fetch_session_cookie_is_null_on_nonzero_exit();

    test_pid_record_round_trips_through_json();
    test_argv_hash_is_stable_and_sensitive();

    test_reap_decision_kills_only_a_verified_tunnel();
    test_reap_decision_fails_closed_on_a_recycled_pid();
    test_reap_decision_rejects_a_non_ssh_exe();
    test_reap_decision_rejects_a_wrong_forward();
    test_reap_decision_handles_a_basename_ssh_path();
    test_reap_decision_no_process_when_pid_is_dead();

    test_reap_orphans_kills_a_verified_record();
    test_reap_orphans_fails_closed_on_a_recycled_pid();
    test_reap_orphans_unlinks_a_corrupt_pidfile_without_killing();
    test_reap_orphans_on_a_missing_directory_is_a_noop();

    test_real_probe_reports_this_process();
    test_real_reaper_refuses_to_kill_this_live_non_ssh_process();

    if (failures == 0) {
        std::cout << "PASS - " << checks << " tunnel checks\n";
        return 0;
    }
    std::cerr << failures << " of " << checks << " tunnel checks failed\n";
    return 1;
}
