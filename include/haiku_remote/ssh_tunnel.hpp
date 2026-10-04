#pragma once

// A managed `ssh -L` tunnel for the connection coordinator, with no UI and no
// SDL. This lives in haiku_remote_core (which has no SDL on its link line) so
// every front end -- SDL, X11, headless -- owns a tunnel the same way, and the
// orphan-avoidance rules are written once rather than per front end.
//
// Orphan avoidance is the whole point. The tool runs unattended on flaky
// networks, so a tunnel that outlives the app would hold the local port and
// make the next connect fail confusingly. Three layers, mirroring the retired
// Swift prototype:
//
//   1. stop() escalates SIGTERM -> SIGKILL to the child's own process group, and
//      exit/signal handlers call it (POSIX); a Job Object kills the child when
//      the app dies (Windows).
//   2. The child pid, forward, and a kernel start-time stamp are persisted to a
//      pidfile, so a *later* launch can reap a tunnel left behind by a crash or
//      SIGKILL (which no in-process handler can catch).
//   3. The reaper is FAIL-CLOSED: it kills a recorded pid only when it is
//      positively verified to still be our ssh (live pid + exe is ssh + cmdline
//      carries our exact -L + the kernel start-time matches). An unverifiable
//      pid is never signalled -- the stale pidfile is merely unlinked -- so a
//      recycled pid can never make us kill a stranger.
//
// The seams (IProcess, the reaper's probe/kill hooks) are injectable so the
// platform-neutral logic is unit-tested with a fake, while the real POSIX path
// is exercised directly on the build host.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

// Everything ssh needs to open the forward. Paths are stored literally (tilde
// kept); SshArgsBuilder expands them only when it emits argv, so a stored
// profile survives a home-directory move (see expand_user_path).
struct TunnelConfig {
    std::string host;                 // SSH destination (required)
    std::string ssh_user = "baron";   // image default
    int ssh_port = 22;
    std::string identity_file;        // optional; empty lets ssh-agent work
    std::string known_hosts_file;     // optional per-profile TOFU pin
    int remote_port = 10900;          // remote app_server port
    std::string remote_host = "127.0.0.1"; // bound loopback-only on the server
    // Concrete local port, or 0 to pick a free loopback port at start().
    int local_port = 0;
};

// The exact ssh argv. Pure: no sockets, no spawn, no clock. argv[0] is "ssh".
// Unit-tested character for character because a drifted option is the kind of
// bug that only shows up against a real server.
struct SshArgsBuilder {
    // "<local>:127.0.0.1:<remote>" -- the -L forward spec.
    [[nodiscard]] static std::string forward_spec(int local_port,
                                                   const TunnelConfig& cfg);

    // ssh -N -T -L <spec> -p <sshPort> -o ... [-i <expanded> -o
    // IdentitiesOnly=yes] [-o UserKnownHostsFile=<expanded>] <user>@<host>
    [[nodiscard]] static std::vector<std::string>
    tunnel_argv(const TunnelConfig& cfg, int local_port);

    // One-shot, no forward: ssh -T -p <sshPort> -o ... <user>@<host>
    //   cat /boot/system/settings/remote_desktop/session_cookie.<remotePort>
    // Keyed on the REMOTE listener port, over the SAME identity as the tunnel.
    [[nodiscard]] static std::vector<std::string>
    cookie_argv(const TunnelConfig& cfg);

    // The remote cookie path for a given listener port.
    [[nodiscard]] static std::string cookie_remote_path(int remote_port);
};

// A spawned child process. The real implementation forks/execs ssh directly
// (never system()/popen()) with stdin on /dev/null; the test fake records argv
// and replays canned output and exit behaviour.
class IProcess {
public:
    virtual ~IProcess() = default;

    // Spawn argv[0] (resolved on PATH) with the full argv. Returns false and
    // sets `error` if the child could not be launched. stdin is /dev/null.
    [[nodiscard]] virtual bool start(const std::vector<std::string>& argv,
                                     std::string& error) = 0;

    // The child's pid (POSIX) or process id (Windows); 0 before a successful
    // start().
    [[nodiscard]] virtual long pid() const = 0;

    // Non-blocking: has the child exited? Reaps it (waitpid) when it has.
    [[nodiscard]] virtual bool running() = 0;

    // Set once the child has exited; nullopt while it is still running.
    [[nodiscard]] virtual std::optional<int> exit_code() = 0;

    // Non-blocking read of whatever stdout/stderr is available since last call.
    [[nodiscard]] virtual std::string drain_stdout() = 0;
    [[nodiscard]] virtual std::string drain_stderr() = 0;

    // Graceful then forced stop of the child's whole process group, always
    // reaping it. Re-verifies the recorded pid before signalling so a recycled
    // pid is never hit. Idempotent.
    virtual void stop() = 0;
};

using ProcessFactory = std::function<std::unique_ptr<IProcess>()>;

// The real fork/exec (POSIX) or CreateProcess+Job Object (Windows) process.
[[nodiscard]] ProcessFactory default_process_factory();

// Bind 127.0.0.1:0, read the kernel-assigned port, close, and hand it to -L.
// ExitOnForwardFailure plus a bounded retry in start() covers the TOCTOU gap
// between closing here and ssh binding. nullopt if no socket could be bound.
[[nodiscard]] std::optional<int> pick_free_loopback_port();

// Attempt a loopback TCP connect to `port`, returning true once something
// accepts. Never opens the protocol socket -- readiness only.
[[nodiscard]] bool loopback_port_accepts(int port,
                                         std::chrono::milliseconds timeout);

enum class TunnelState {
    idle,
    starting,
    waiting_for_forward,
    running,
    failed,
    stopped,
};

[[nodiscard]] std::string_view tunnel_state_name(TunnelState state);

struct TunnelStatus {
    TunnelState state = TunnelState::idle;
    int local_port = 0;
    std::string error; // actionable, verbatim ssh stderr folded in on failure
    [[nodiscard]] bool ok() const { return state == TunnelState::running; }
};

// What a pidfile records about a running tunnel. No secret is representable: a
// cookie is never written here, only the forward's shape and a start-time stamp
// that pins the record to one process incarnation.
struct PidRecord {
    long pid = 0;
    int local_port = 0;
    int remote_port = 0;
    std::string host;
    std::string argv_hash;    // stable hash of the launched argv (diagnostic)
    long long start_time = 0; // kernel start-time stamp (recycled-pid guard)

    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static std::optional<PidRecord> parse(std::string_view text);
};

// A stable, dependency-free hash of an argv (FNV-1a, hex). Used to stamp the
// pidfile; not security-sensitive.
[[nodiscard]] std::string argv_hash(const std::vector<std::string>& argv);

// A live process as the OS reports it, for reaper verification.
struct ProcessIdentity {
    std::string exe;                  // executable name (comm / basename)
    std::vector<std::string> cmdline; // argv as the kernel reports it
    long long start_time = 0;         // same unit as PidRecord::start_time
};

// Probe a live pid. nullopt means "no live, inspectable process with that pid"
// -- which the reaper treats as unverifiable and therefore never kills.
[[nodiscard]] std::optional<ProcessIdentity> probe_process(long pid);

// Force-terminate a pid (SIGKILL / TerminateProcess). The reaper's default kill.
void force_kill_process(long pid);

enum class ReapDecision {
    kill_it,    // positively verified as our ssh tunnel
    not_ours,   // live but unverifiable -> leave the process, unlink the file
    no_process, // nothing live at that pid -> unlink the stale file
};

// PURE, fail-closed: decide what to do with a recorded tunnel given the live
// identity probed for its pid. kill_it requires ALL of: a live process, exe is
// ssh, cmdline carries the exact "-L <local>:127.0.0.1:<remote>", and the
// start-time matches the record. Anything short of that is not_ours.
[[nodiscard]] ReapDecision reap_decision(const PidRecord& record,
                                         const std::optional<ProcessIdentity>& live);

// Injectable seams for reap_orphans so the fail-closed logic is testable
// without the real OS.
struct ReapHooks {
    std::function<std::optional<ProcessIdentity>(long)> probe = probe_process;
    std::function<void(long)> kill = force_kill_process;
};

struct ReapReport {
    int killed = 0;      // verified-ours tunnels we terminated
    int left_alone = 0;  // live but unverifiable pids we refused to touch
    int unlinked = 0;    // pidfiles removed (stale, dead, or after a kill)
};

// Scan a directory of pidfiles, reaping only verified-ours ssh tunnels and
// unlinking every pidfile it inspects. Safe to call at launch and before each
// connect. A missing directory is a no-op.
ReapReport reap_orphans(const std::filesystem::path& dir, const ReapHooks& hooks = {});

struct TunnelOptions {
    std::chrono::milliseconds forward_timeout{20000};
    std::chrono::milliseconds poll_interval{250};
    std::chrono::milliseconds connect_timeout{1000};
    std::chrono::milliseconds cookie_timeout{8000};
    int free_port_retries = 5; // retries covering the free-port TOCTOU
    // Directory for pidfiles; empty selects default_pidfile_dir().
    std::filesystem::path pidfile_dir;
};

// The interface the connection coordinator depends on.
class ISshTunnel {
public:
    virtual ~ISshTunnel() = default;
    virtual TunnelStatus start() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual TunnelState state() const = 0;
    [[nodiscard]] virtual int local_port() const = 0;
    [[nodiscard]] virtual const std::string& stderr_log() const = 0;
    // One-shot cookie fetch over the same ssh identity. In-memory only; the
    // returned value is never logged or persisted. nullopt on any failure.
    [[nodiscard]] virtual std::optional<std::string> fetch_session_cookie() = 0;
};

class SshTunnel : public ISshTunnel {
public:
    explicit SshTunnel(TunnelConfig cfg, TunnelOptions options = {},
                       ProcessFactory factory = default_process_factory());
    ~SshTunnel() override;

    SshTunnel(const SshTunnel&) = delete;
    SshTunnel& operator=(const SshTunnel&) = delete;

    // Reap any verified orphan, pick a free local port if none was fixed, build
    // argv, exec ssh, record the pidfile, then poll the forward to readiness --
    // failing fast on an early ssh exit with its stderr folded into the error.
    TunnelStatus start() override;
    void stop() override;

    [[nodiscard]] TunnelState state() const override { return state_; }
    [[nodiscard]] int local_port() const override { return local_port_; }
    [[nodiscard]] const std::string& stderr_log() const override { return stderr_; }
    [[nodiscard]] std::optional<std::string> fetch_session_cookie() override;

    // The argv of the last start(), for diagnostics and tests.
    [[nodiscard]] const std::vector<std::string>& argv() const { return argv_; }

    // <config_dir>/tunnels -- one pidfile per local port.
    [[nodiscard]] static std::filesystem::path default_pidfile_dir();

private:
    [[nodiscard]] std::filesystem::path pidfile_path() const;
    void write_pidfile();
    void remove_pidfile();
    TunnelStatus fail(std::string message);

    TunnelConfig config_;
    TunnelOptions options_;
    ProcessFactory factory_;
    std::unique_ptr<IProcess> process_;
    std::vector<std::string> argv_;
    TunnelState state_ = TunnelState::idle;
    int local_port_ = 0;
    long pid_ = 0;
    std::string stderr_;
};

} // namespace haiku_remote
