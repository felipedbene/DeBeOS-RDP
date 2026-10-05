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
// is complete. The Windows implementation is a clean, loud stub -- every entry
// point fails with a "not implemented (Part 4)" message rather than silently
// doing nothing -- so a Windows build links and the seam is unmistakable.

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/profile_launch.hpp"
#include "haiku_remote/transport.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace haiku_remote {

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
    // app's stderr).
    bool spawn(const std::vector<std::string>& argv, std::string& error);

    [[nodiscard]] bool spawned() const { return pid_ > 0 && !reaped_; }
    [[nodiscard]] long pid() const { return pid_; }

    // True while the child is still alive. Non-blocking; reaps the child if it
    // has already exited so there is no zombie.
    [[nodiscard]] bool running();

    // Signal the child (SIGTERM, a short grace period, then SIGKILL) and reap
    // it. Idempotent. Called by the destructor; safe to call early.
    void terminate();

private:
    long pid_ = -1;
    bool reaped_ = false;
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
class SystemCommandRunner : public CommandRunner {
public:
    explicit SystemCommandRunner(int timeout_seconds = 20)
        : timeout_seconds_(timeout_seconds)
    {
    }
    int run(const std::vector<std::string>& argv, std::string& out,
            std::string& error) override;

private:
    int timeout_seconds_;
};

// A self-deleting temp file, used to materialise the broker's PEM certificate
// so it can be passed to the transport as ca_file. Move-only; the destructor
// removes the file.
class TempFile {
public:
    TempFile() = default;
    ~TempFile();
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&& other) noexcept;
    TempFile& operator=(TempFile&& other) noexcept;

    // Write `contents` to a fresh temp file with the given suffix. Returns false
    // and sets `error` on failure.
    bool write(const std::string& contents, const std::string& suffix,
               std::string& error);
    [[nodiscard]] const std::string& path() const { return path_; }

private:
    std::string path_;
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

    bool start(std::string& error);
    [[nodiscard]] std::uint16_t local_port() const { return local_port_; }
    [[nodiscard]] bool running() { return process_.running(); }
    [[nodiscard]] std::string describe() const;

private:
    SshConfig cfg_;
    std::string remote_host_;
    std::uint16_t remote_port_;
    std::uint16_t local_port_;
    ManagedProcess process_;
};

// The result of standing up a route. Holds whatever children/temp files the
// route needs alive for the life of the session: keep the ManagedConnection in
// scope for as long as the session runs, and drop it to tear the route down.
struct ManagedConnection {
    bool ok = false;
    RouteKind kind = RouteKind::broker;
    TransportOptions transport; // ready for make_transport().
    std::string note;           // what actually happened.
    std::string error;          // why, when ok == false.

    // Owned substrate. Non-null only for the route that was taken.
    std::unique_ptr<SshTunnel> tunnel;
    std::unique_ptr<TempFile> broker_cert;
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
    contacting_broker, // broker route: fetching the token + certificate over SSH.
    fetching_cookie,   // tunnel route: reading app_server's session cookie over SSH.
    opening_tunnel,    // tunnel route: spawning and waiting on the ssh -L forward.
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
};

// Walk a profile's launch plan and stand up the first route whose substrate can
// be established, returning a ManagedConnection that owns it. For the broker it
// fetches the token + certificate; for the tunnel it fetches the cookie (unless
// the profile supplied one as a file) and spawns the forward. `runner` is for
// tests; when null a SystemCommandRunner is used. `observer`, when non-null, is
// notified of each step/phase for progress UI. Never throws.
ManagedConnection open_connection(const ConnectionProfile& profile,
                                  CommandRunner* runner = nullptr,
                                  ConnectObserver* observer = nullptr);

} // namespace haiku_remote
