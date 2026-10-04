#include "haiku_remote/ssh_tunnel.hpp"

#include "haiku_remote/connection_profile.hpp" // expand_user_path
#include "haiku_remote/json.hpp"
#include "haiku_remote/profile_store.hpp" // ProfileStore::config_dir

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace haiku_remote {

namespace {

#ifdef _WIN32
struct WinsockGuard {
    WinsockGuard() { WSADATA d {}; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WinsockGuard() { WSACleanup(); }
};
void close_socket(SOCKET fd) { closesocket(fd); }
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
void close_socket(int fd) { ::close(fd); }
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

sockaddr_in loopback_addr(int port)
{
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

} // namespace

std::optional<int> pick_free_loopback_port()
{
#ifdef _WIN32
    WinsockGuard guard;
#endif
    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket)
        return std::nullopt;
    sockaddr_in addr = loopback_addr(0);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(fd);
        return std::nullopt;
    }
    sockaddr_in bound {};
#ifdef _WIN32
    int len = sizeof(bound);
#else
    socklen_t len = sizeof(bound);
#endif
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        close_socket(fd);
        return std::nullopt;
    }
    close_socket(fd);
    return static_cast<int>(ntohs(bound.sin_port));
}

bool loopback_port_accepts(int port, std::chrono::milliseconds timeout)
{
#ifdef _WIN32
    WinsockGuard guard;
#endif
    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket)
        return false;

    // Non-blocking connect so a dead port fails fast and a slow one is bounded
    // by `timeout` rather than the kernel's default connect timeout.
#ifdef _WIN32
    u_long nonblocking = 1;
    ioctlsocket(fd, FIONBIO, &nonblocking);
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

    sockaddr_in addr = loopback_addr(port);
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    bool accepted = false;
    if (rc == 0) {
        accepted = true;
    } else {
#ifdef _WIN32
        const bool in_progress = (WSAGetLastError() == WSAEWOULDBLOCK);
#else
        const bool in_progress = (errno == EINPROGRESS);
#endif
        if (in_progress) {
#ifdef _WIN32
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(fd, &writable);
            timeval tv {};
            tv.tv_sec = static_cast<long>(timeout.count() / 1000);
            tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
            const int ready = ::select(0, nullptr, &writable, nullptr, &tv);
#else
            pollfd pfd {};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int ready = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
#endif
            if (ready > 0) {
                int err = 0;
#ifdef _WIN32
                int errlen = sizeof(err);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char*>(&err), &errlen);
#else
                socklen_t errlen = sizeof(err);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
#endif
                accepted = (err == 0);
            }
        }
    }
    close_socket(fd);
    return accepted;
}

std::string_view tunnel_state_name(TunnelState state)
{
    switch (state) {
    case TunnelState::idle: return "idle";
    case TunnelState::starting: return "starting";
    case TunnelState::waiting_for_forward: return "waiting_for_forward";
    case TunnelState::running: return "running";
    case TunnelState::failed: return "failed";
    case TunnelState::stopped: return "stopped";
    }
    return "idle";
}

std::string SshArgsBuilder::forward_spec(int local_port, const TunnelConfig& cfg)
{
    return std::to_string(local_port) + ":" + cfg.remote_host + ":"
        + std::to_string(cfg.remote_port);
}

std::vector<std::string> SshArgsBuilder::tunnel_argv(const TunnelConfig& cfg,
                                                     int local_port)
{
    std::vector<std::string> argv = {
        "ssh",
        "-N",
        "-T",
        "-L", forward_spec(local_port, cfg),
        "-p", std::to_string(cfg.ssh_port),
        "-o", "ExitOnForwardFailure=yes",
        "-o", "ServerAliveInterval=15",
        "-o", "ServerAliveCountMax=3",
        "-o", "ConnectTimeout=15",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "BatchMode=yes",
        "-o", "ControlMaster=no",
        "-o", "ControlPath=none",
    };
    if (!cfg.identity_file.empty()) {
        argv.push_back("-i");
        argv.push_back(expand_user_path(cfg.identity_file));
        argv.push_back("-o");
        argv.push_back("IdentitiesOnly=yes");
    }
    if (!cfg.known_hosts_file.empty()) {
        argv.push_back("-o");
        argv.push_back("UserKnownHostsFile=" + expand_user_path(cfg.known_hosts_file));
    }
    argv.push_back(cfg.ssh_user + "@" + cfg.host);
    return argv;
}

std::string SshArgsBuilder::cookie_remote_path(int remote_port)
{
    return "/boot/system/settings/remote_desktop/session_cookie."
        + std::to_string(remote_port);
}

std::vector<std::string> SshArgsBuilder::cookie_argv(const TunnelConfig& cfg)
{
    // No -N and no -L: this is a one-shot remote command over the same identity,
    // not a forward. Short ConnectTimeout keeps a wedged host from stalling the
    // connect flow.
    std::vector<std::string> argv = {
        "ssh",
        "-T",
        "-p", std::to_string(cfg.ssh_port),
        "-o", "ConnectTimeout=15",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "BatchMode=yes",
        "-o", "ControlMaster=no",
        "-o", "ControlPath=none",
    };
    if (!cfg.identity_file.empty()) {
        argv.push_back("-i");
        argv.push_back(expand_user_path(cfg.identity_file));
        argv.push_back("-o");
        argv.push_back("IdentitiesOnly=yes");
    }
    if (!cfg.known_hosts_file.empty()) {
        argv.push_back("-o");
        argv.push_back("UserKnownHostsFile=" + expand_user_path(cfg.known_hosts_file));
    }
    argv.push_back(cfg.ssh_user + "@" + cfg.host);
    argv.push_back("cat");
    argv.push_back(cookie_remote_path(cfg.remote_port));
    return argv;
}

std::string argv_hash(const std::vector<std::string>& argv)
{
    // FNV-1a over argv joined by NUL. Not security-sensitive -- it only has to
    // change when the launched command changes.
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](char c) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ull;
    };
    for (const auto& arg : argv) {
        for (char c : arg)
            mix(c);
        mix('\0');
    }
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(hash));
    return std::string(buffer);
}

std::string PidRecord::serialize() const
{
    json::Value root;
    root.set("pid", json::Value(static_cast<std::int64_t>(pid)));
    root.set("local", json::Value(local_port));
    root.set("remote", json::Value(remote_port));
    root.set("host", json::Value(host));
    root.set("argvHash", json::Value(argv_hash));
    root.set("startTime", json::Value(static_cast<std::int64_t>(start_time)));
    return json::serialize(root, /*pretty=*/true);
}

std::optional<PidRecord> PidRecord::parse(std::string_view text)
{
    const json::ParseResult parsed = json::parse(text);
    if (!parsed.ok || !parsed.value.is_object())
        return std::nullopt;
    const json::Value& root = parsed.value;
    const json::Value* pid = root.find("pid");
    const json::Value* local = root.find("local");
    const json::Value* remote = root.find("remote");
    const json::Value* start = root.find("startTime");
    if (!pid || !local || !remote || !start)
        return std::nullopt;
    PidRecord record;
    record.pid = static_cast<long>(pid->as_int());
    record.local_port = static_cast<int>(local->as_int());
    record.remote_port = static_cast<int>(remote->as_int());
    record.host = root.find("host") ? root.find("host")->as_string() : std::string();
    record.argv_hash
        = root.find("argvHash") ? root.find("argvHash")->as_string() : std::string();
    record.start_time = static_cast<long long>(start->as_int());
    return record;
}

ReapDecision reap_decision(const PidRecord& record,
                           const std::optional<ProcessIdentity>& live)
{
    if (!live)
        return ReapDecision::no_process;

    // exe is ssh: compare the basename so an absolute path ("/usr/bin/ssh") and
    // a bare "ssh" both verify, but "sshd" or "notssh" do not.
    std::string exe = live->exe;
    const auto slash = exe.find_last_of("/\\");
    if (slash != std::string::npos)
        exe = exe.substr(slash + 1);
    if (exe != "ssh")
        return ReapDecision::not_ours;

    // cmdline carries our exact "-L <local>:127.0.0.1:<remote>" as two adjacent
    // tokens. Rebuilding the spec from the record (not from live) is what makes
    // this an identity check rather than a tautology.
    const std::string spec = std::to_string(record.local_port) + ":127.0.0.1:"
        + std::to_string(record.remote_port);
    bool has_forward = false;
    for (std::size_t i = 0; i + 1 < live->cmdline.size(); ++i) {
        if (live->cmdline[i] == "-L" && live->cmdline[i + 1] == spec) {
            has_forward = true;
            break;
        }
    }
    if (!has_forward)
        return ReapDecision::not_ours;

    // start-time pins the record to this process incarnation: a recycled pid
    // reuses the number but never the kernel's start-time stamp.
    if (live->start_time != record.start_time)
        return ReapDecision::not_ours;

    return ReapDecision::kill_it;
}

ReapReport reap_orphans(const std::filesystem::path& dir, const ReapHooks& hooks)
{
    ReapReport report;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return report;

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec)
            break;
        if (!entry.is_regular_file())
            continue;
        if (entry.path().extension() != ".pid")
            continue;

        std::string text;
        {
            std::ifstream in(entry.path(), std::ios::binary);
            std::ostringstream buffer;
            buffer << in.rdbuf();
            text = buffer.str();
        }
        const std::optional<PidRecord> record = PidRecord::parse(text);
        if (!record) {
            // A pidfile we cannot read is stale by definition: unlink it.
            std::filesystem::remove(entry.path(), ec);
            ++report.unlinked;
            continue;
        }

        const std::optional<ProcessIdentity> live
            = hooks.probe ? hooks.probe(record->pid) : std::nullopt;
        switch (reap_decision(*record, live)) {
        case ReapDecision::kill_it:
            if (hooks.kill)
                hooks.kill(record->pid);
            ++report.killed;
            std::filesystem::remove(entry.path(), ec);
            ++report.unlinked;
            break;
        case ReapDecision::not_ours:
            // Fail-closed: a live but unverifiable pid is never signalled. Only
            // the stale pidfile goes.
            ++report.left_alone;
            std::filesystem::remove(entry.path(), ec);
            ++report.unlinked;
            break;
        case ReapDecision::no_process:
            std::filesystem::remove(entry.path(), ec);
            ++report.unlinked;
            break;
        }
    }
    return report;
}

std::filesystem::path SshTunnel::default_pidfile_dir()
{
    return ProfileStore::config_dir() / "tunnels";
}

SshTunnel::SshTunnel(TunnelConfig cfg, TunnelOptions options, ProcessFactory factory)
    : config_(std::move(cfg)), options_(std::move(options)),
      factory_(std::move(factory))
{
    if (options_.pidfile_dir.empty())
        options_.pidfile_dir = default_pidfile_dir();
}

SshTunnel::~SshTunnel()
{
    stop();
}

std::filesystem::path SshTunnel::pidfile_path() const
{
    return options_.pidfile_dir
        / ("tunnel-" + std::to_string(local_port_) + ".pid");
}

void SshTunnel::write_pidfile()
{
    std::error_code ec;
    std::filesystem::create_directories(options_.pidfile_dir, ec);
    PidRecord record;
    record.pid = pid_;
    record.local_port = local_port_;
    record.remote_port = config_.remote_port;
    record.host = config_.host;
    record.argv_hash = argv_hash(argv_);
    // Stamp with the kernel start-time of our own child so a later reaper can
    // tell this incarnation from a recycled pid.
    if (const std::optional<ProcessIdentity> self = probe_process(pid_))
        record.start_time = self->start_time;
    std::ofstream out(pidfile_path(), std::ios::binary | std::ios::trunc);
    out << record.serialize();
}

void SshTunnel::remove_pidfile()
{
    std::error_code ec;
    std::filesystem::remove(pidfile_path(), ec);
}

TunnelStatus SshTunnel::fail(std::string message)
{
    state_ = TunnelState::failed;
    if (process_)
        process_->stop();
    remove_pidfile();
    return TunnelStatus{TunnelState::failed, local_port_, std::move(message)};
}

TunnelStatus SshTunnel::start()
{
    stop();
    state_ = TunnelState::starting;
    stderr_.clear();

    // Reap a verified orphan from a previous run before every connect, so a
    // crashed tunnel can never hold the local port against us.
    reap_orphans(options_.pidfile_dir);

    const bool auto_port = (config_.local_port == 0);
    const int attempts = auto_port ? std::max(1, options_.free_port_retries) : 1;

    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (auto_port) {
            const std::optional<int> picked = pick_free_loopback_port();
            if (!picked)
                return fail("could not reserve a free local port");
            local_port_ = *picked;
        } else {
            local_port_ = config_.local_port;
        }

        argv_ = SshArgsBuilder::tunnel_argv(config_, local_port_);
        process_ = factory_();
        std::string launch_error;
        if (!process_ || !process_->start(argv_, launch_error)) {
            // A failed exec is not a port race; do not retry.
            return fail(launch_error.empty() ? "could not launch ssh"
                                             : launch_error);
        }
        pid_ = process_->pid();
        write_pidfile();
        state_ = TunnelState::waiting_for_forward;

        // Poll the forward to readiness. ssh gives no "forward is up" signal, so
        // a loopback connect is the readiness probe -- and an early ssh exit
        // (auth failure, forward refused) is caught the same loop.
        const auto deadline = std::chrono::steady_clock::now() + options_.forward_timeout;
        bool early_exit = false;
        while (std::chrono::steady_clock::now() < deadline) {
            stderr_ += process_->drain_stderr();
            if (!process_->running()) {
                early_exit = true;
                break;
            }
            if (loopback_port_accepts(local_port_, options_.connect_timeout)) {
                state_ = TunnelState::running;
                return TunnelStatus{TunnelState::running, local_port_, {}};
            }
            std::this_thread::sleep_for(options_.poll_interval);
        }
        stderr_ += process_->drain_stderr();

        if (early_exit) {
            const std::optional<int> code = process_->exit_code();
            std::string message = "ssh exited";
            if (code)
                message += " (" + std::to_string(*code) + ")";
            if (!stderr_.empty())
                message += ": " + stderr_;
            // ExitOnForwardFailure on a busy local port looks exactly like this;
            // when the port was auto-picked, try a fresh one before giving up.
            if (auto_port && attempt + 1 < attempts) {
                process_->stop();
                remove_pidfile();
                continue;
            }
            return fail(std::move(message));
        }

        return fail("forward did not open within timeout"
                    + (stderr_.empty() ? std::string() : ": " + stderr_));
    }

    return fail("could not establish the tunnel");
}

void SshTunnel::stop()
{
    if (process_) {
        process_->stop();
        process_.reset();
        remove_pidfile();
    }
    if (state_ == TunnelState::running || state_ == TunnelState::waiting_for_forward
        || state_ == TunnelState::starting)
        state_ = TunnelState::stopped;
    pid_ = 0;
}

std::optional<std::string> SshTunnel::fetch_session_cookie()
{
    // A one-shot `cat` over the same identity. Captured in memory, trimmed, and
    // returned -- never logged, never written to the pidfile or anywhere on
    // disk. The process is always killed and reaped before we return.
    std::unique_ptr<IProcess> fetch = factory_();
    if (!fetch)
        return std::nullopt;
    const std::vector<std::string> argv = SshArgsBuilder::cookie_argv(config_);
    std::string launch_error;
    if (!fetch->start(argv, launch_error))
        return std::nullopt;

    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + options_.cookie_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        out += fetch->drain_stdout();
        (void)fetch->drain_stderr(); // consume but never surface a cookie error body
        if (!fetch->running())
            break;
        std::this_thread::sleep_for(options_.poll_interval);
    }
    out += fetch->drain_stdout();
    const std::optional<int> code = fetch->exit_code();
    fetch->stop(); // kill + reap regardless of how the wait ended

    if (code && *code != 0)
        return std::nullopt;

    // Trim surrounding whitespace/newlines; the cookie itself is a single token.
    const auto first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::nullopt;
    const auto last = out.find_last_not_of(" \t\r\n");
    return out.substr(first, last - first + 1);
}

} // namespace haiku_remote
