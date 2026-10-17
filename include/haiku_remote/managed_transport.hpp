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
// The Windows implementation (CreateProcess + a kill-on-close Job Object, the
// stderr pipe drained by the same reader thread) keeps the same move-only RAII
// contract behind #ifdef _WIN32. It is built and run on Windows Server 2022
// (MSYS2 UCRT64) by tests/tunnel_tests.cpp's Windows checks: stderr capture,
// the runner's timeout and cancel, a fake and a real ssh.exe, and no ssh.exe
// left after a disconnect, an ExitProcess or a TerminateProcess of its client.

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/transport.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace haiku_remote {

namespace detail {
class StderrCapture; // managed_transport.cpp
}

// How much of a child's stderr ManagedProcess keeps for stderr_text(): the
// most recent this-many bytes. ssh's last words ("Permission denied
// (publickey).", "Could not request local forwarding.") are what a diagnosis
// needs, and they come last. The tunnel runs ssh at LogLevel=DEBUG1 (its
// readiness signal, see forward_readiness()): a few KiB up to the forward, a
// line or two per forwarded connection or rekey after it.
inline constexpr std::size_t process_stderr_capacity = 32 * 1024;

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
    // instead of being inherited (a pipe on POSIX, an anonymous pipe HANDLE on
    // Windows). A reader thread drains it from the moment of the spawn until
    // the last writer closes it, so the program can never block on a full
    // pipe -- however long it runs, and whether or not anyone reads.
    bool spawn_capturing_stderr(const std::vector<std::string>& argv,
                                std::string& error);
    // Everything the program wrote to stderr since the last call, without
    // blocking. Empty unless spawned with spawn_capturing_stderr(). Bounded:
    // if more than process_stderr_capacity bytes pile up unread, the oldest go
    // and the text starts with "[... N earlier bytes dropped ...]\n".
    [[nodiscard]] std::string read_stderr();
    // POSIX: a descriptor that polls readable when read_stderr() has something
    // new (or the stream ended), for a poll() set; -1 once the stream ended and
    // was read to the end, and on Windows.
    [[nodiscard]] int stderr_fd() const;
    // The whole captured stderr so far, independent of read_stderr(): the most
    // recent process_stderr_capacity bytes, with the same "[... N earlier
    // bytes dropped ...]" first line when older ones had to go. Still readable
    // after the program is reaped, and then complete (reaping drains the pipe
    // to its end first). Thread-safe; "" unless capturing.
    [[nodiscard]] std::string stderr_text() const;
    // False once the captured stream has ended and read_stderr() has returned
    // its last byte (and when nothing was captured). Portable, unlike
    // stderr_fd() < 0.
    [[nodiscard]] bool stderr_open() const;

    // Write a verified-orphan record (see sweep_orphaned_processes()) for the
    // program spawn() starts, into `dir`, and delete it once the program is
    // reaped. Call before spawn(). Empty (the default) records nothing; so
    // does a platform where process_start_identity() is "", because a record
    // that cannot be verified must never be acted on.
    void record_in(std::filesystem::path dir) { record_dir_ = std::move(dir); }
    // The record spawn() wrote, or empty.
    [[nodiscard]] const std::filesystem::path& record_file() const
    {
        return record_file_;
    }

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
    // Once the guard is reaped: make sure the program went with it (a SIGKILLed
    // guard leaves it running -- stopped here, verified, while we still know
    // exactly which process it is), delete the record, and drain the stderr
    // pipe to its end.
    void after_reap();
    void write_record();

    long pid_ = -1;
    long program_pid_ = -1;
    bool reaped_ = false;
    std::shared_ptr<detail::StderrCapture> capture_;
    // The program's identity (process_start_identity()) and argv, kept from
    // spawn() so after_reap() and the record can verify it. Empty start = the
    // platform cannot identify it, and nothing is ever signalled on its basis.
    std::string program_start_;
    std::vector<std::string> argv_;
    std::filesystem::path record_dir_;
    std::filesystem::path record_file_;
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
// The child's stderr is captured (POSIX pipe, Windows pipe HANDLE) and handed
// line by line to the sink set with set_stderr_sink(), or passed through to
// this process's stderr when there is none. A cancelled token kills the child and run() returns -1 with
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
    // The QUIC stream layouts (ALPNs) the running broker serves, from its
    // broker.quic advertisement (DeBeOS #766); empty when it serves no QUIC.
    std::vector<std::string> quic_alpns;

    [[nodiscard]] bool offers_quic(std::string_view alpn) const;
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
// at the instant it is probed -- but with the explicit 127.0.0.1 bind address
// ssh fails loudly via ExitOnForwardFailure if it was taken in the meantime,
// and SshTunnel surfaces that.)
std::uint16_t pick_free_local_port(std::string& error);

// --- tunnel readiness --------------------------------------------------------
//
// Whether `ssh -o LogLevel=DEBUG1 -o ExitOnForwardFailure=yes -N
// -L 127.0.0.1:P:...` has its forward up, read from ssh's own stderr instead of
// connecting through the forward (every such probe reached app_server as one
// empty candidate connection). The two lines and why each is needed, as
// OpenSSH writes them (channels.c channel_setup_fwd_listener_tcpip(),
// clientloop.c client_loop(); checked against OpenSSH 9.9p1):
//
//   debug1: Local forwarding listening on 127.0.0.1 port P.
//       ssh is ABOUT to bind 127.0.0.1:P. Not readiness on its own: it is
//       logged before bind(), so a port that is taken prints it and then
//       "bind [127.0.0.1]:P: Address already in use".
//   debug1: Entering interactive session.
//       client_loop() started, which ssh reaches only after
//       ssh_init_forwarding() returned. With ExitOnForwardFailure=yes and the
//       single explicit bind address 127.0.0.1, that means the bind and listen
//       on 127.0.0.1:P succeeded: a failure fatal()s ("Could not request local
//       forwarding.") before it.
//
// ready   both lines, in that order, for this port.
// failed  "bind [127.0.0.1]:P: ..." or "Could not request local forwarding.";
//         `why` is that line.
// pending neither yet.
//
// The explicit bind address matters as much as the lines. With `-L P:...`
// ssh binds every loopback address (::1 and 127.0.0.1) and counts the forward
// a success if ANY bind works, so when another program holds 127.0.0.1:P, ssh
// stays up on ::1 and the client, which dials 127.0.0.1, reaches that other
// program -- and hands it the session cookie.
enum class ForwardReadiness { pending, ready, failed };

// Line-at-a-time form, for a reader that sees the lines as they come.
class ForwardReadinessScanner {
public:
    explicit ForwardReadinessScanner(std::uint16_t local_port);
    // Feed one stderr line (without its newline). Returns the state after it.
    ForwardReadiness feed(std::string_view line);
    [[nodiscard]] ForwardReadiness state() const { return state_; }
    [[nodiscard]] const std::string& why() const { return why_; }

private:
    std::string listening_;
    std::string bind_failed_;
    bool listening_seen_ = false;
    ForwardReadiness state_ = ForwardReadiness::pending;
    std::string why_;
};

// Whole-transcript form of the scanner.
[[nodiscard]] ForwardReadiness forward_readiness(std::string_view ssh_stderr,
                                                 std::uint16_t local_port,
                                                 std::string& why);

// True for an ssh debug line ("debug1: ...", "debug2: ...", "debug3: ...").
// The tunnel's LogLevel=DEBUG1 output is for readiness and stderr_text(); a
// StderrSink, and this process's own stderr when there is no sink, get only the
// lines ssh prints at its default log level, exactly as before.
[[nodiscard]] bool is_ssh_debug_line(std::string_view line);

// --- verified-orphan cleanup --------------------------------------------------
//
// The guard (ManagedProcess) stops a program when its owner dies, however the
// owner dies. What it cannot cover is the guard dying in the same instant:
// `killall -9 haiku-remote-gui` (the guard is a fork of the client, so it has
// the client's name and is matched too), `pkill -9 -f`, an OOM kill of both.
// The program (ssh) is then reparented to init with its forward still bound.
// On Linux the program also gets PR_SET_PDEATHSIG, so it is signalled when the
// guard dies; macOS has no equivalent, and on both a record on disk is what
// lets a later run find it. Windows needs none of this: the kill-on-close job
// object is enforced by the kernel.
//
// A record names the owner (pid + start identity) and the program (pid + start
// identity + its exact argv). sweep_orphaned_processes() acts on a record only
// if ALL of these hold, and re-checks them before every signal:
//   - the owner is gone (its pid is free, or names a process that started at
//     another time) -- a live client's tunnel is never touched;
//   - the program's pid names a process with the recorded start identity -- a
//     recycled pid is never signalled;
//   - that process's argv is exactly the recorded argv.
// On Linux the signal goes through a pidfd opened BEFORE the final check, so a
// pid recycled between that check and the signal cannot be hit either.

// An opaque, comparable identity for the start of process `pid`, or "" when
// the process does not exist, is a zombie, or this platform cannot say (only
// Linux and macOS can). Linux: boot id + start time in clock ticks since boot;
// macOS: the start time in microseconds since the epoch.
[[nodiscard]] std::string process_start_identity(long pid);
// The argv of a running process, or nullopt if it cannot be read.
[[nodiscard]] std::optional<std::vector<std::string>> process_argv(long pid);

struct ProcessRecord {
    long owner_pid = -1;
    std::string owner_start;
    long program_pid = -1;
    std::string program_start;
    std::vector<std::string> argv;
};

// JSON form of a record. parse returns nullopt for anything that is not a
// complete record.
[[nodiscard]] std::string serialize_process_record(const ProcessRecord& record);
[[nodiscard]] std::optional<ProcessRecord> parse_process_record(std::string_view text);

struct OrphanSweep {
    std::vector<long> stopped;  // verified orphans, stopped
    std::size_t live_owner = 0; // records left alone: their owner is alive
    std::size_t stale = 0;      // records deleted: the program was already gone
    std::size_t unreadable = 0; // records deleted: not a valid record
};

// Where SshTunnel keeps its records: $XDG_RUNTIME_DIR/haiku-remote/processes
// when that is set (per user, emptied at boot), else
// ProfileStore::config_dir()/processes.
[[nodiscard]] std::filesystem::path default_process_record_dir();

// Check every record in `dir`: stop each verified orphan (SIGTERM, a 2 s
// grace, SIGKILL -- each signal re-verified first) and delete every record
// whose owner is gone. Never throws; a missing directory is an empty sweep.
// Always empty on Windows and wherever process_start_identity() is "".
OrphanSweep sweep_orphaned_processes(const std::filesystem::path& dir);

// A live SSH -L forward this process owns. start() sweeps verified orphans of
// an earlier crashed run, spawns ssh, and blocks until ssh reports the forward
// listening (ForwardReadinessScanner), the child dies, or a timeout elapses.
// It never connects through the forward. The destructor tears the forward
// down by reaping the child.
class SshTunnel {
public:
    SshTunnel(SshConfig cfg, std::uint16_t remote_port,
              std::uint16_t local_port = 0,
              std::string remote_host = "127.0.0.1");

    // spawn() then wait_ready(); kept as one call for existing callers.
    bool start(std::string& error);
    // Sweep verified orphans, pick the local port if it is automatic, and
    // spawn ssh -L (stderr always captured, an orphan record written).
    bool spawn(std::string& error);
    // Wait until ssh reports the forward listening (ForwardReadinessScanner),
    // reports it failed, exits, the timeout passes, or the cancel token fires.
    // Never connects through the forward. On every failure the child has been
    // reaped before this returns.
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
    // ssh's whole stderr so far (ManagedProcess::stderr_text()), its DEBUG1
    // lines included. ssh's stderr is always captured, sink or not: it is the
    // readiness signal.
    [[nodiscard]] std::string stderr_text() const { return process_.stderr_text(); }
    // A place in stderr_text(), and the lines ssh printed at its default log
    // level (is_ssh_debug_line() dropped) after it, one per line: what ssh
    // said during one phase -- the transport connect and the handshake, or a
    // whole session -- read from the reader thread's transcript, with no sink
    // to drain. Once the transcript has been trimmed to its last 32 KiB, a
    // mark taken before the trim no longer lines up, and every line still
    // retained is returned instead.
    [[nodiscard]] std::size_t stderr_mark() const { return stderr_text().size(); }
    [[nodiscard]] std::string messages_since(std::size_t mark) const;

    // Test seams, set before spawn(). The record directory
    // (default_process_record_dir() unless set; an empty path disables both
    // the record and the sweep) and how long wait_ready() waits.
    void set_record_dir(std::filesystem::path dir) { record_dir_ = std::move(dir); }
    void set_ready_timeout_ms(int ms) { ready_timeout_ms_ = ms; }
    // What spawn()'s sweep found.
    [[nodiscard]] const OrphanSweep& last_sweep() const { return last_sweep_; }

private:
    void feed(const std::string& chunk);

    SshConfig cfg_;
    std::string remote_host_;
    std::uint16_t remote_port_;
    std::uint16_t local_port_;
    ManagedProcess process_;
    const CancelToken* cancel_ = nullptr;
    StderrSink stderr_sink_;
    std::string stderr_partial_;
    ForwardReadinessScanner readiness_ {0};
    std::optional<std::filesystem::path> record_dir_;
    // ssh's own ConnectTimeout=15 plus an authentication round trip: the
    // forward is bound only after both, so a shorter wait gives up on a
    // slow-but-healthy host before ssh would.
    int ready_timeout_ms_ = 20000;
    OrphanSweep last_sweep_;
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
// the profile supplied one as a file) and spawns the forward; for direct there
// is nothing to stand up -- the cookie is the plan's (cookie_source) and the
// transport connects straight to host:remotePort, refusing locally when there
// is no cookie. `runner` is for
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
