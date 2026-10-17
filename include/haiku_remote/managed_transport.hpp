#pragma once

// The side-effecting half of the launch bridge: it takes the pure plan from
// profile_launch.hpp and actually stands up a route -- spawning and OWNING the
// SSH child that forwards app_server's loopback port, or fetching the broker's
// session token and self-signed certificate over SSH -- then hands back a
// ManagedConnection whose destructor tears all of that back down. Nothing here
// touches the protocol, renderer, or transport core; it only produces the
// TransportOptions those already consume.
//
// The one invariant that matters most (an explicit DeBeOS-RDP issue #1 goal):
// an SSH child is NEVER orphaned. Every process is owned by a ManagedProcess
// whose destructor terminates and reaps it, and the SshTunnel / ManagedConnection
// that own one are move-only RAII handles. Drop the handle -- on disconnect or
// on app exit -- and the ssh child is gone before the handle's destructor
// returns.
//
// Portability: process spawn/kill is abstracted behind ManagedProcess. The
// POSIX implementation (fork/exec + waitpid, used on Linux, macOS, and Haiku)
// is complete and is the only path built and tested on this project's Linux CI.
// The Windows implementation (CreateProcess + a kill-on-close Job Object) is
// written in Part 4 behind #ifdef _WIN32 to the same move-only RAII contract,
// but is UNVERIFIED on this host -- it has never been compiled or run here
// because this builder has no Windows toolchain. It needs a real Windows build
// before it can be trusted; see docs/client.md "Windows and SDL verification
// gaps". The POSIX path is unchanged and remains the default everywhere else.

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/transport.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace haiku_remote {

// Cooperative cancellation for a connect attempt running on a worker thread
// (DeBeOS-RDP issue #1, "Connecting state": the progress screen's Cancel).
//
// cancel() is called from the UI thread; everything that blocks on the worker
// either waits on wait_fd() alongside its own descriptors (the ssh exec runner,
// the tunnel's forward wait) or registers an on_cancel() hook that makes the
// blocking call return at once (a Transport's abort(), which shuts its socket
// down under a blocking connect or receive). Process control never crosses
// threads: a cancelled wait returns on the worker, and the worker kills and
// reaps its own child -- so the ManagedProcess guarantees are unchanged.
class CancelToken {
public:
    CancelToken();
    ~CancelToken();
    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;

    // Thread-safe and idempotent. Runs every registered hook, on the calling
    // thread, before it returns.
    void cancel();
    [[nodiscard]] bool cancelled() const { return cancelled_.load(); }
    // POSIX: a descriptor that polls readable from the moment cancel() runs,
    // for including in a poll() set. -1 on Windows, where waits check
    // cancelled() between bounded slices instead.
    [[nodiscard]] int wait_fd() const { return pipe_[0]; }

    // Unregisters on destruction. Once a Hook is destroyed its action is not
    // running and never will, which is what makes it safe to capture a
    // reference to a short-lived object (a transport) in it.
    class Hook {
    public:
        Hook() = default;
        Hook(CancelToken* token, std::uint64_t id) : token_(token), id_(id) {}
        ~Hook() { reset(); }
        Hook(Hook&& other) noexcept : token_(other.token_), id_(other.id_)
        {
            other.token_ = nullptr;
        }
        Hook& operator=(Hook&& other) noexcept;
        Hook(const Hook&) = delete;
        Hook& operator=(const Hook&) = delete;
        void reset();

    private:
        CancelToken* token_ = nullptr;
        std::uint64_t id_ = 0;
    };
    // Register `action` to run when cancel() is called -- or right now, on
    // this thread, if it already was.
    [[nodiscard]] Hook on_cancel(std::function<void()> action);

private:
    std::atomic<bool> cancelled_ {false};
    std::mutex mutex_;
    std::map<std::uint64_t, std::function<void()>> hooks_;
    std::uint64_t next_id_ = 1;
    int pipe_[2] = {-1, -1};
};

// Delivers a child's stderr one line at a time (without the newline). ssh's
// diagnostics are the only place it says *why* it failed -- "Permission denied
// (publickey).", "Host key verification failed." -- so they are captured for
// the failure classifier and the diagnostic log instead of being lost on the
// terminal the GUI was launched from.
using StderrSink = std::function<void(const std::string& line)>;

// Everything needed to talk to a host over SSH, derived from a profile. Paths
// are already tilde-expanded (expand_user_path) by the time they land here.
struct SshConfig {
    std::string user;
    std::string host;
    int port = 22;
    std::string identity_file;    // -i; empty means ssh's own default keys.
    std::string known_hosts_file; // -o UserKnownHostsFile=...; empty means default.
};

// A single owned child process. Move-only; the destructor guarantees the child
// is signalled and reaped. The whole point of the type is that there is no way
// to leak one: if a ManagedProcess exists and owns a pid, that pid is this
// handle's responsibility and dies with it.
class ManagedProcess {
public:
    ManagedProcess() = default;
    ~ManagedProcess();

    ManagedProcess(const ManagedProcess&) = delete;
    ManagedProcess& operator=(const ManagedProcess&) = delete;
    ManagedProcess(ManagedProcess&& other) noexcept;
    ManagedProcess& operator=(ManagedProcess&& other) noexcept;

    // Fork/exec argv[0] with argv as its arguments. Returns false and sets
    // `error` if the spawn failed. On POSIX the child's stdin is attached to
    // /dev/null; stdout/stderr are left inherited (ssh diagnostics reach the
    // app's stderr). On POSIX the program runs under a parent-death guard, so
    // it does not outlive this process even when this process is SIGKILLed.
    bool spawn(const std::vector<std::string>& argv, std::string& error);
    // As spawn(), but the program's stderr goes to a pipe this handle owns
    // instead of being inherited: read it with read_stderr(). POSIX only; on
    // Windows this is spawn() and stderr stays inherited (UNVERIFIED there).
    bool spawn_capturing_stderr(const std::vector<std::string>& argv,
                                std::string& error);
    // Everything the program wrote to stderr since the last call, without
    // blocking. Empty unless spawned with spawn_capturing_stderr().
    [[nodiscard]] std::string read_stderr();
    // The read end of the captured stderr pipe, for a poll() set; -1 if none.
    [[nodiscard]] int stderr_fd() const { return stderr_fd_; }

    [[nodiscard]] bool spawned() const { return pid_ > 0 && !reaped_; }
    [[nodiscard]] long pid() const { return pid_; }
    // The pid of the program argv names. On Windows that is pid(); on POSIX
    // pid() is the parent-death guard (see spawn()) and this is its child.
    [[nodiscard]] long program_pid() const { return program_pid_; }

    // True while the child is still alive. Non-blocking; reaps the child if it
    // has already exited so there is no zombie.
    [[nodiscard]] bool running();

    // Signal the child (SIGTERM, a short grace period, then SIGKILL) and reap
    // it. Idempotent. Called by the destructor; safe to call early. On Windows
    // this is TerminateProcess plus closing the job object -- there is no clean
    // SIGTERM equivalent for a windowless child, so the grace step is skipped
    // and the kill-on-close job guarantees the whole tree dies regardless.
    void terminate();

private:
    bool spawn_impl(const std::vector<std::string>& argv, bool capture_stderr,
                    std::string& error);
    void close_stderr();

    long pid_ = -1;
    long program_pid_ = -1;
    bool reaped_ = false;
    int stderr_fd_ = -1;
#if !defined(_WIN32)
    // Write end of the lifeline pipe, held by this process alone. POSIX has no
    // kill-on-close job object and no portable parent-death signal (Linux has
    // PR_SET_PDEATHSIG, macOS nothing), so a SIGKILLed client used to leave its
    // ssh running. The child spawn() forks is a small guard that execs nothing:
    // it forks again to run argv, and when this end closes -- however this
    // process ends, SIGKILL included, because the kernel closes it -- it reads
    // EOF, takes the program down and exits.
    int lifeline_fd_ = -1;
    void close_lifeline();
#endif
#if defined(_WIN32)
    // Opaque Win32 HANDLEs, kept as void* so this header never pulls in
    // <windows.h>. The job object owns the child with
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, so even if this process is killed
    // without running ~ManagedProcess, the OS reaps the child when the last
    // job handle closes -- the "never orphan an ssh child" invariant, enforced
    // by the kernel rather than by our cleanup running.
    void* process_handle_ = nullptr; // HANDLE to the child process.
    void* job_handle_ = nullptr;     // HANDLE to the owning job object.
#endif
};

// Build the argv for the two SSH invocations this module makes. Pure and
// deterministic so they can be unit-tested without spawning anything.
//
// base: ssh with the shared hardening/timeout options (and -i/-p/known_hosts
//   when the config sets them), ending just before any user@host or command.
[[nodiscard]] std::vector<std::string> build_ssh_base_argv(const SshConfig& cfg);
// tunnel: base + a -N -L forward of local_port -> remote_host:remote_port and
//   the user@host. No -f: the forward runs in the foreground child we own.
[[nodiscard]] std::vector<std::string> build_ssh_tunnel_argv(
    const SshConfig& cfg, std::uint16_t local_port, const std::string& remote_host,
    std::uint16_t remote_port);
// exec: base + user@host + a single remote command string.
[[nodiscard]] std::vector<std::string> build_ssh_exec_argv(
    const SshConfig& cfg, const std::string& remote_command);

// Abstraction over "run a command, capture its stdout" so the broker/cookie
// fetch logic can be exercised with a fake in tests. run() returns the child's
// exit status (0 on success), -1 when the command could not be launched or
// timed out, and writes captured stdout to `out`.
class CommandRunner {
public:
    virtual ~CommandRunner() = default;
    virtual int run(const std::vector<std::string>& argv, std::string& out,
                    std::string& error) = 0;
};

// The real runner: spawns the command and reads its stdout to completion under
// a wall-clock deadline (so an unreachable host cannot wedge the UI).
//
// The child's stderr is captured (POSIX) and handed line by line to the sink
// set with set_stderr_sink(), or passed through to this process's stderr when
// there is none. A cancelled token kills the child and run() returns -1 with
// error "cancelled".
class SystemCommandRunner : public CommandRunner {
public:
    explicit SystemCommandRunner(int timeout_seconds = 20)
        : timeout_seconds_(timeout_seconds)
    {
    }
    int run(const std::vector<std::string>& argv, std::string& out,
            std::string& error) override;

    void set_cancel(const CancelToken* cancel) { cancel_ = cancel; }
    void set_stderr_sink(StderrSink sink) { stderr_sink_ = std::move(sink); }

private:
    int timeout_seconds_;
    const CancelToken* cancel_ = nullptr;
    StderrSink stderr_sink_;
};

struct BrokerCredentials {
    std::string token;    // RP_AUTHENTICATE token.
    std::string cert_pem; // The broker's self-signed certificate, PEM.
};

// Fetch the broker's token and certificate over SSH: ensure remote_broker is
// running on the host, then read its token and broker.pem. Pure over `runner`
// -- it builds the argv and parses stdout -- so a test can drive it with a fake
// runner and never open a socket. Returns false and sets `error` when the SSH
// command fails or either field comes back empty.
bool fetch_broker_credentials(CommandRunner& runner, const SshConfig& cfg,
                              std::uint16_t broker_port,
                              BrokerCredentials& out, std::string& error);

// Fetch app_server's per-boot session cookie for `session_port` over SSH. Same
// testability contract as fetch_broker_credentials.
bool fetch_session_cookie(CommandRunner& runner, const SshConfig& cfg,
                          std::uint16_t session_port, std::string& out,
                          std::string& error);

// Choose a free local TCP port on the loopback interface for an SSH forward.
// Returns 0 and sets `error` on failure. (Inherently racy -- the port is free
// at the instant it is probed -- but ssh fails loudly via ExitOnForwardFailure
// if it was taken in the meantime, and SshTunnel surfaces that.)
std::uint16_t pick_free_local_port(std::string& error);

// A live SSH -L forward this process owns. start() spawns ssh and blocks until
// the local port actually accepts a connection (or the child dies, or a
// timeout elapses). The destructor tears the forward down by reaping the child.
class SshTunnel {
public:
    SshTunnel(SshConfig cfg, std::uint16_t remote_port,
              std::uint16_t local_port = 0,
              std::string remote_host = "127.0.0.1");

    // spawn() then wait_ready(); kept as one call for existing callers.
    bool start(std::string& error);
    // Pick the local port if it is automatic, and spawn ssh -L.
    bool spawn(std::string& error);
    // Wait until the forward accepts a connection, ssh exits, the forward
    // timeout passes, or the cancel token fires. On every failure the child
    // has been reaped before this returns.
    bool wait_ready(std::string& error);
    [[nodiscard]] std::uint16_t local_port() const { return local_port_; }
    [[nodiscard]] bool running() { return process_.running(); }
    [[nodiscard]] std::string describe() const;

    // Optional, set before spawn(). The sink receives ssh's stderr lines --
    // during wait_ready(), and afterwards whenever drain_stderr() is called
    // (ssh reports a forward whose remote end refused only when a connection
    // first goes through it). Without a sink, stderr stays inherited.
    void set_cancel(const CancelToken* cancel) { cancel_ = cancel; }
    void set_stderr_sink(StderrSink sink) { stderr_sink_ = std::move(sink); }
    void drain_stderr();

private:
    SshConfig cfg_;
    std::string remote_host_;
    std::uint16_t remote_port_;
    std::uint16_t local_port_;
    ManagedProcess process_;
    const CancelToken* cancel_ = nullptr;
    StderrSink stderr_sink_;
    std::string stderr_partial_;
};

// The result of standing up a route. Holds whatever child process the
// route needs alive for the life of the session: keep the ManagedConnection in
// scope for as long as the session runs, and drop it to tear the route down.
struct ManagedConnection {
    bool ok = false;
    RouteKind kind = RouteKind::broker;
    TransportOptions transport; // ready for make_transport().
    std::string note;           // what actually happened.
    std::string error;          // why, when ok == false.

    // Owned substrate. Non-null only for the tunnel route. (The broker route
    // owns nothing: its SSH-fetched certificate becomes a known_brokers seed,
    // TransportOptions::known_broker_fingerprint, rather than a temp ca_file.)
    std::unique_ptr<SshTunnel> tunnel;
};

// --- Part 3: connection-progress observation -------------------------------
//
// open_connection() reports which route step it is attempting and which phase
// of that step is running, so a UI can step the user through the plan rather
// than freeze on a black window (DeBeOS-RDP issue #1 Part 3). This is purely a
// reporting seam bolted onto the existing walk: it changes neither the routing
// (which is plan_launch's) nor the tunnel lifecycle. A null observer (the
// default) is a no-op, so every existing caller and test is unaffected.
enum class ConnectPhase {
    contacting_broker,   // broker route: fetching the token + certificate over SSH.
    fetching_cookie,     // tunnel route: reading app_server's session cookie over SSH.
    opening_tunnel,      // tunnel route: spawning the ssh -L forward.
    waiting_for_forward, // tunnel route: ssh is up; waiting for the local end to accept.
};

class ConnectObserver {
public:
    virtual ~ConnectObserver() = default;
    // A plan step (0-based) is about to be attempted.
    virtual void on_step_begin(std::size_t index, RouteKind kind)
    {
        (void)index;
        (void)kind;
    }
    // Progress within the current step.
    virtual void on_phase(std::size_t index, ConnectPhase phase)
    {
        (void)index;
        (void)phase;
    }
    // The current step's substrate could not be established; open_connection
    // will try the next step if the plan has one (the broker->tunnel fallback).
    virtual void on_step_failed(std::size_t index, RouteKind kind,
                                const std::string& error)
    {
        (void)index;
        (void)kind;
        (void)error;
    }
    // The current step's substrate is up; this is the route that will be used.
    virtual void on_step_ready(std::size_t index, RouteKind kind)
    {
        (void)index;
        (void)kind;
    }
    // A line ssh wrote to stderr while working on step `index`. Overriding
    // this is what makes open_connection() capture ssh's stderr at all; the
    // default observer leaves it inherited, as before.
    virtual bool wants_ssh_stderr() const { return false; }
    virtual void on_ssh_stderr(std::size_t index, const std::string& line)
    {
        (void)index;
        (void)line;
    }
    // A credential open_connection() just obtained (the broker token, the
    // session cookie), so a diagnostic log can redact it from lines it already
    // holds -- ssh's stderr is logged before the step returns, and a failed
    // step returns no credentials at all.
    virtual void on_secret(const std::string& secret) { (void)secret; }
    // When non-null, open_connection() stops between steps, and its ssh waits
    // return, once this is cancelled.
    virtual const CancelToken* cancel_token() const { return nullptr; }
};

// Walk a profile's launch plan and stand up the first route whose substrate can
// be established, returning a ManagedConnection that owns it. For the broker it
// fetches the token + certificate; for the tunnel it fetches the cookie (unless
// the profile supplied one as a file) and spawns the forward. `runner` is for
// tests; when null a SystemCommandRunner is used. `observer`, when non-null, is
// notified of each step/phase for progress UI. `first_step` skips the plan's
// earlier steps (indices stay plan_launch's, so an observer's view lines up);
// connect_with_progress() uses it to fall back once a route whose substrate
// came up still fails to connect. Never throws.
ManagedConnection open_connection(const ConnectionProfile& profile,
                                  CommandRunner* runner = nullptr,
                                  ConnectObserver* observer = nullptr,
                                  std::size_t first_step = 0);

} // namespace haiku_remote
