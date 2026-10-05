#include "haiku_remote/managed_transport.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

#if defined(_WIN32)
// Windows process management is deferred to Part 4 (see the header). The POSIX
// path below is #ifdef'd out and every entry point reports the gap loudly.
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace haiku_remote {

namespace {

#if !defined(_WIN32)
// Open a TCP connection to host:port with a timeout; returns true if the port
// accepts. Used both to probe tunnel readiness and (indirectly) nothing else.
bool loopback_accepts(std::uint16_t port, int timeout_ms)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    // Non-blocking connect so the probe honours the timeout.
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    bool accepted = false;
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        accepted = true;
    } else if (errno == EINPROGRESS) {
        pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (::poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                accepted = true;
        }
    }
    ::close(fd);
    return accepted;
}
#endif

// Trim surrounding ASCII whitespace.
std::string trim(const std::string& value)
{
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(begin, end - begin);
}

// The marker fetch_broker_credentials uses to split the token from the PEM in a
// single SSH round trip. It is a line the remote command emits between them.
constexpr const char* kBrokerPemMarker = "===BROKER_PEM===";

// Remote settings paths, matching tools/haiku-remote-connect.sh.
std::string remote_settings_dir()
{
    return "/boot/system/settings/remote_desktop";
}

} // namespace

// ---------------------------------------------------------------------------
// ManagedProcess
// ---------------------------------------------------------------------------

ManagedProcess::~ManagedProcess() { terminate(); }

ManagedProcess::ManagedProcess(ManagedProcess&& other) noexcept
    : pid_(other.pid_), reaped_(other.reaped_)
{
    other.pid_ = -1;
    other.reaped_ = true;
}

ManagedProcess& ManagedProcess::operator=(ManagedProcess&& other) noexcept
{
    if (this != &other) {
        terminate();
        pid_ = other.pid_;
        reaped_ = other.reaped_;
        other.pid_ = -1;
        other.reaped_ = true;
    }
    return *this;
}

#if !defined(_WIN32)

bool ManagedProcess::spawn(const std::vector<std::string>& argv, std::string& error)
{
    if (argv.empty()) {
        error = "cannot spawn an empty command";
        return false;
    }
    // argv must outlive the child's execvp; build a NUL-terminated char* vector
    // from the caller's strings (which stay alive for the duration of this call).
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& arg : argv)
        cargv.push_back(const_cast<char*>(arg.c_str()));
    cargv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        // Child: detach stdin so ssh cannot grab the terminal for a password
        // prompt (BatchMode already forbids one, but belt and suspenders).
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            if (devnull > STDIN_FILENO)
                ::close(devnull);
        }
        ::execvp(cargv[0], cargv.data());
        // execvp only returns on failure.
        _exit(127);
    }
    pid_ = pid;
    reaped_ = false;
    return true;
}

bool ManagedProcess::running()
{
    if (pid_ <= 0 || reaped_)
        return false;
    int status = 0;
    const pid_t rc = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (rc == 0)
        return true; // still alive
    // rc == pid (exited) or rc < 0 with ECHILD (already reaped elsewhere).
    reaped_ = true;
    return false;
}

void ManagedProcess::terminate()
{
    if (pid_ <= 0 || reaped_) {
        pid_ = -1;
        return;
    }
    const pid_t pid = static_cast<pid_t>(pid_);
    ::kill(pid, SIGTERM);
    // Grace period: poll for a clean exit for up to ~2s before escalating.
    for (int i = 0; i < 200; ++i) {
        int status = 0;
        const pid_t rc = ::waitpid(pid, &status, WNOHANG);
        if (rc == pid || (rc < 0 && errno == ECHILD)) {
            reaped_ = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!reaped_) {
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0); // blocking: SIGKILL cannot be caught
        reaped_ = true;
    }
    pid_ = -1;
}

int SystemCommandRunner::run(const std::vector<std::string>& argv, std::string& out,
                             std::string& error)
{
    out.clear();
    if (argv.empty()) {
        error = "cannot run an empty command";
        return -1;
    }
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        error = std::string("pipe failed: ") + std::strerror(errno);
        return -1;
    }

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& arg : argv)
        cargv.push_back(const_cast<char*>(arg.c_str()));
    cargv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        if (pipefd[1] > STDOUT_FILENO)
            ::close(pipefd[1]);
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            if (devnull > STDIN_FILENO)
                ::close(devnull);
        }
        ::execvp(cargv[0], cargv.data());
        _exit(127);
    }
    ::close(pipefd[1]);
    const int read_fd = pipefd[0];

    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(timeout_seconds_);
    bool timed_out = false;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timed_out = true;
            break;
        }
        const auto remaining_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                .count();
        pollfd pfd {};
        pfd.fd = read_fd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, static_cast<int>(remaining_ms));
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0) {
            timed_out = true;
            break;
        }
        char buffer[4096];
        const ssize_t n = ::read(read_fd, buffer, sizeof(buffer));
        if (n > 0) {
            out.append(buffer, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break; // EOF: child closed stdout
        } else {
            if (errno == EINTR)
                continue;
            break;
        }
    }
    ::close(read_fd);

    if (timed_out) {
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
        error = "command timed out after " + std::to_string(timeout_seconds_) + "s";
        return -1;
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    error = "command terminated by signal";
    return -1;
}

std::uint16_t pick_free_local_port(std::string& error)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::string("socket failed: ") + std::strerror(errno);
        return 0;
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0; // let the kernel pick
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = std::string("bind failed: ") + std::strerror(errno);
        ::close(fd);
        return 0;
    }
    sockaddr_in bound {};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        error = std::string("getsockname failed: ") + std::strerror(errno);
        ::close(fd);
        return 0;
    }
    const std::uint16_t port = ntohs(bound.sin_port);
    ::close(fd);
    return port;
}

#else // _WIN32: deferred to Part 4.

bool ManagedProcess::spawn(const std::vector<std::string>&, std::string& error)
{
    error = "process spawn not implemented on Windows (DeBeOS-RDP issue #1 Part 4)";
    return false;
}
bool ManagedProcess::running() { return false; }
void ManagedProcess::terminate() { pid_ = -1; }

int SystemCommandRunner::run(const std::vector<std::string>&, std::string&,
                             std::string& error)
{
    error = "command execution not implemented on Windows (issue #1 Part 4)";
    return -1;
}

std::uint16_t pick_free_local_port(std::string& error)
{
    error = "local port selection not implemented on Windows (issue #1 Part 4)";
    return 0;
}

#endif

// ---------------------------------------------------------------------------
// TempFile
// ---------------------------------------------------------------------------

TempFile::~TempFile()
{
    if (!path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
}

TempFile::TempFile(TempFile&& other) noexcept : path_(std::move(other.path_))
{
    other.path_.clear();
}

TempFile& TempFile::operator=(TempFile&& other) noexcept
{
    if (this != &other) {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
        path_ = std::move(other.path_);
        other.path_.clear();
    }
    return *this;
}

bool TempFile::write(const std::string& contents, const std::string& suffix,
                     std::string& error)
{
    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec);
    if (ec) {
        error = "no temp directory: " + ec.message();
        return false;
    }
    static int counter = 0;
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path =
        base / ("haiku-remote-" + std::to_string(stamp) + "-"
                + std::to_string(counter++) + suffix);
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        error = "could not create temp file " + path.string();
        return false;
    }
    file << contents;
    file.close();
    if (!file) {
        error = "could not write temp file " + path.string();
        std::filesystem::remove(path, ec);
        return false;
    }
    path_ = path.string();
    return true;
}

// ---------------------------------------------------------------------------
// SSH argv builders
// ---------------------------------------------------------------------------

std::vector<std::string> build_ssh_base_argv(const SshConfig& cfg)
{
    std::vector<std::string> argv = {
        "ssh",
        // Non-interactive by construction: never prompt, never hang on a
        // password or an unknown-host question, and bound the TCP connect.
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=10",
    };
    if (cfg.port != 22 && cfg.port > 0) {
        argv.push_back("-p");
        argv.push_back(std::to_string(cfg.port));
    }
    if (!cfg.identity_file.empty()) {
        argv.push_back("-i");
        argv.push_back(cfg.identity_file);
        // With an explicit key, do not also offer the agent's keys.
        argv.push_back("-o");
        argv.push_back("IdentitiesOnly=yes");
    }
    if (!cfg.known_hosts_file.empty()) {
        argv.push_back("-o");
        argv.push_back("UserKnownHostsFile=" + cfg.known_hosts_file);
    }
    return argv;
}

std::vector<std::string> build_ssh_tunnel_argv(const SshConfig& cfg,
                                               std::uint16_t local_port,
                                               const std::string& remote_host,
                                               std::uint16_t remote_port)
{
    std::vector<std::string> argv = build_ssh_base_argv(cfg);
    // Fail loudly if the forward cannot be set up, rather than connecting with
    // no tunnel and silently waiting.
    argv.push_back("-o");
    argv.push_back("ExitOnForwardFailure=yes");
    // No remote command, just the forward: -N keeps the session open with no
    // shell. We own this child and manage it in the foreground (no -f).
    argv.push_back("-N");
    argv.push_back("-L");
    argv.push_back(std::to_string(local_port) + ":" + remote_host + ":"
                   + std::to_string(remote_port));
    argv.push_back(cfg.user.empty() ? cfg.host : cfg.user + "@" + cfg.host);
    return argv;
}

std::vector<std::string> build_ssh_exec_argv(const SshConfig& cfg,
                                             const std::string& remote_command)
{
    std::vector<std::string> argv = build_ssh_base_argv(cfg);
    argv.push_back(cfg.user.empty() ? cfg.host : cfg.user + "@" + cfg.host);
    argv.push_back(remote_command);
    return argv;
}

// ---------------------------------------------------------------------------
// Broker / cookie fetch
// ---------------------------------------------------------------------------

bool fetch_broker_credentials(CommandRunner& runner, const SshConfig& cfg,
                              std::uint16_t broker_port, BrokerCredentials& out,
                              std::string& error)
{
    (void)broker_port; // the broker binds its own configured port on the host
    const std::string dir = remote_settings_dir();
    // One round trip: make sure remote_broker is up, print the token, a marker,
    // then the certificate. Mirrors haiku-remote-connect.sh's --wss path.
    const std::string remote_command =
        "if ! ps 2>/dev/null | grep -q '[r]emote_broker'; then "
        "nohup /system/servers/remote_broker >/tmp/remote_broker.log 2>&1 & "
        "sleep 2; fi; "
        "cat " + dir + "/token 2>/dev/null; "
        "echo '" + std::string(kBrokerPemMarker) + "'; "
        "cat " + dir + "/broker.pem 2>/dev/null";

    const std::vector<std::string> argv = build_ssh_exec_argv(cfg, remote_command);
    std::string stdout_text;
    const int rc = runner.run(argv, stdout_text, error);
    if (rc != 0) {
        if (error.empty())
            error = "ssh to " + cfg.host + " exited " + std::to_string(rc);
        return false;
    }

    const std::size_t marker = stdout_text.find(kBrokerPemMarker);
    if (marker == std::string::npos) {
        error = "broker response missing the certificate marker";
        return false;
    }
    out.token = trim(stdout_text.substr(0, marker));
    std::size_t pem_begin = marker + std::strlen(kBrokerPemMarker);
    if (pem_begin < stdout_text.size() && stdout_text[pem_begin] == '\n')
        ++pem_begin;
    out.cert_pem = stdout_text.substr(pem_begin);
    // Trim only the trailing whitespace of the PEM; its internal newlines matter.
    while (!out.cert_pem.empty()
           && std::isspace(static_cast<unsigned char>(out.cert_pem.back())))
        out.cert_pem.pop_back();

    if (out.token.empty()) {
        error = "broker returned no token (is remote_broker present and app_server up?)";
        return false;
    }
    if (out.cert_pem.find("BEGIN CERTIFICATE") == std::string::npos) {
        error = "broker returned no certificate (expected broker.pem)";
        return false;
    }
    return true;
}

bool fetch_session_cookie(CommandRunner& runner, const SshConfig& cfg,
                          std::uint16_t session_port, std::string& out,
                          std::string& error)
{
    const std::string remote_command =
        "cat " + remote_settings_dir() + "/session_cookie."
        + std::to_string(session_port) + " 2>/dev/null";
    const std::vector<std::string> argv = build_ssh_exec_argv(cfg, remote_command);
    std::string stdout_text;
    const int rc = runner.run(argv, stdout_text, error);
    if (rc != 0) {
        if (error.empty())
            error = "ssh to " + cfg.host + " exited " + std::to_string(rc);
        return false;
    }
    out = trim(stdout_text);
    if (out.empty()) {
        error = "could not read session_cookie." + std::to_string(session_port)
                + " (is app_server up?)";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// SshTunnel
// ---------------------------------------------------------------------------

SshTunnel::SshTunnel(SshConfig cfg, std::uint16_t remote_port,
                     std::uint16_t local_port, std::string remote_host)
    : cfg_(std::move(cfg)), remote_host_(std::move(remote_host)),
      remote_port_(remote_port), local_port_(local_port)
{
}

std::string SshTunnel::describe() const
{
    return "127.0.0.1:" + std::to_string(local_port_) + " -> " + cfg_.user + "@"
           + cfg_.host + " -> " + remote_host_ + ":" + std::to_string(remote_port_);
}

bool SshTunnel::start(std::string& error)
{
#if defined(_WIN32)
    error = "SSH tunnel not implemented on Windows (DeBeOS-RDP issue #1 Part 4)";
    return false;
#else
    if (local_port_ == 0) {
        local_port_ = pick_free_local_port(error);
        if (local_port_ == 0)
            return false;
    }
    const std::vector<std::string> argv =
        build_ssh_tunnel_argv(cfg_, local_port_, remote_host_, remote_port_);
    if (!process_.spawn(argv, error))
        return false;

    // Wait for the forward to accept a connection. ssh binds the listener
    // before the remote channel is confirmed, so a successful probe means the
    // local end is live; ExitOnForwardFailure guarantees ssh exits (which we
    // detect) rather than lingering if the bind or channel failed.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!process_.running()) {
            error = "ssh exited before the forward on 127.0.0.1:"
                    + std::to_string(local_port_) + " came up (check the key, "
                    "host, and that the remote port is listening)";
            return false; // process_ already reaped by running()
        }
        if (loopback_accepts(local_port_, 200))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    error = "timed out waiting for the ssh forward on 127.0.0.1:"
            + std::to_string(local_port_);
    process_.terminate(); // never orphan the child on a timeout
    return false;
#endif
}

// ---------------------------------------------------------------------------
// open_connection
// ---------------------------------------------------------------------------

namespace {

SshConfig config_from_profile(const ConnectionProfile& profile)
{
    SshConfig cfg;
    cfg.user = profile.ssh_user;
    cfg.host = profile.host;
    cfg.port = profile.ssh_port;
    cfg.identity_file = expand_user_path(profile.identity_file);
    cfg.known_hosts_file = expand_user_path(profile.known_hosts_file);
    return cfg;
}

std::uint16_t clamp_remote_port(int port)
{
    if (port < 1)
        return 1;
    if (port > 65535)
        return 65535;
    return static_cast<std::uint16_t>(port);
}

} // namespace

ManagedConnection open_connection(const ConnectionProfile& profile,
                                  CommandRunner* runner)
{
    SystemCommandRunner system_runner;
    CommandRunner& use = runner ? *runner : system_runner;

    const LaunchPlan plan = plan_launch(profile);
    const SshConfig cfg = config_from_profile(profile);
    const std::uint16_t remote_port = clamp_remote_port(profile.remote_port);

    ManagedConnection conn;
    std::string last_error;

    for (const RouteStep& step : plan.steps) {
        if (step.kind == RouteKind::broker) {
            BrokerCredentials creds;
            std::string e;
            if (!fetch_broker_credentials(use, cfg, default_broker_port, creds, e)) {
                last_error = "broker route: " + e;
                continue;
            }
            auto cert = std::make_unique<TempFile>();
            std::string we;
            if (!cert->write(creds.cert_pem, ".pem", we)) {
                last_error = "broker route: " + we;
                continue;
            }
            TransportOptions t = step.transport;
            t.token = creds.token;
            t.ca_file = cert->path();
            t.cookie.clear(); // the broker presents app_server's cookie itself
            conn.ok = true;
            conn.kind = RouteKind::broker;
            conn.transport = std::move(t);
            conn.broker_cert = std::move(cert);
            conn.note = "broker (wss): " + conn.transport.url
                        + " with a fetched token and pinned certificate";
            return conn;
        }

        // Tunnel route.
        TransportOptions t = step.transport;
        if (t.cookie.empty()) {
            std::string ck;
            std::string e;
            if (!fetch_session_cookie(use, cfg, remote_port, ck, e)) {
                last_error = "tunnel route: " + e;
                continue;
            }
            t.cookie = ck;
        }
        auto tunnel = std::make_unique<SshTunnel>(
            cfg, remote_port, step.transport.port /*0 = auto*/);
        std::string e;
        if (!tunnel->start(e)) {
            last_error = "tunnel route: " + e;
            continue;
        }
        t.url.clear();
        t.host = "127.0.0.1";
        t.port = tunnel->local_port();
        conn.ok = true;
        conn.kind = RouteKind::tunnel;
        conn.note = "ssh tunnel: " + tunnel->describe();
        conn.transport = std::move(t);
        conn.tunnel = std::move(tunnel);
        return conn;
    }

    conn.ok = false;
    conn.error = last_error.empty() ? "no route could be established" : last_error;
    return conn;
}

} // namespace haiku_remote
