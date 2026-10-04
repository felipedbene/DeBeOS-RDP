#include "haiku_remote/ssh_tunnel.hpp"

#ifndef _WIN32

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/sysctl.h>
#endif

#if defined(__linux__) || defined(__HAIKU__)
#include <cstdio>
#include <fstream>
#include <sstream>
#endif

namespace haiku_remote {

namespace {

void set_nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Drain a non-blocking fd of whatever is currently buffered.
std::string drain_fd(int fd)
{
    std::string out;
    if (fd < 0)
        return out;
    std::array<char, 4096> buffer {};
    for (;;) {
        const ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n > 0) {
            out.append(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }
        break; // 0 (EOF) or -1 (EAGAIN/EINTR): nothing more right now
    }
    return out;
}

class PosixProcess final : public IProcess {
public:
    ~PosixProcess() override { stop(); }

    bool start(const std::vector<std::string>& argv, std::string& error) override
    {
        if (argv.empty()) {
            error = "empty argv";
            return false;
        }
        argv_ = argv;

        int out_pipe[2] = {-1, -1};
        int err_pipe[2] = {-1, -1};
        if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
            error = std::string("pipe: ") + std::strerror(errno);
            return false;
        }

        const pid_t pid = ::fork();
        if (pid < 0) {
            error = std::string("fork: ") + std::strerror(errno);
            ::close(out_pipe[0]); ::close(out_pipe[1]);
            ::close(err_pipe[0]); ::close(err_pipe[1]);
            return false;
        }

        if (pid == 0) {
            // Child. Own process group so stop() can signal the whole group and
            // never reaches back into the parent.
            ::setpgid(0, 0);
#if defined(__linux__)
            // Die if the parent dies without a clean stop() -- the last-ditch
            // orphan guard the pidfile reaper backstops.
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
            // ssh with BatchMode never prompts, but give it a closed stdin so it
            // can never block on one.
            const int devnull = ::open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDIN_FILENO);
                if (devnull != STDIN_FILENO)
                    ::close(devnull);
            }
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::dup2(err_pipe[1], STDERR_FILENO);
            ::close(out_pipe[0]); ::close(out_pipe[1]);
            ::close(err_pipe[0]); ::close(err_pipe[1]);

            std::vector<char*> raw;
            raw.reserve(argv_.size() + 1);
            for (auto& arg : argv_)
                raw.push_back(const_cast<char*>(arg.c_str()));
            raw.push_back(nullptr);
            // exec ssh DIRECTLY -- never system()/popen().
            ::execvp(raw[0], raw.data());
            ::_exit(127); // exec failed
        }

        // Parent.
        pid_ = pid;
        ::setpgid(pid, pid); // race-free group assignment from both sides
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);
        stdout_fd_ = out_pipe[0];
        stderr_fd_ = err_pipe[0];
        set_nonblocking(stdout_fd_);
        set_nonblocking(stderr_fd_);

        if (const std::optional<ProcessIdentity> self = probe_process(pid_))
            start_time_ = self->start_time;
        return true;
    }

    long pid() const override { return pid_; }

    bool running() override
    {
        if (pid_ <= 0 || exited_)
            return false;
        int status = 0;
        const pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == 0)
            return true; // still alive
        if (r == pid_) {
            exited_ = true;
            exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status)
                                           : 128 + WTERMSIG(status);
        } else {
            // -1/ECHILD: already reaped elsewhere; treat as exited.
            exited_ = true;
        }
        return false;
    }

    std::optional<int> exit_code() override
    {
        running(); // refresh
        if (!exited_)
            return std::nullopt;
        return exit_code_;
    }

    std::string drain_stdout() override { return drain_fd(stdout_fd_); }
    std::string drain_stderr() override { return drain_fd(stderr_fd_); }

    void stop() override
    {
        if (pid_ > 0 && !exited_) {
            // Re-verify the recorded pid+argv before signalling: a pid recycled
            // since we forked must never be hit. Our child's /proc cmdline is
            // exactly the argv we exec'd, and its start-time is the one we
            // stamped -- require both.
            if (process_is_still_ours()) {
                ::kill(-pid_, SIGTERM);
                // Escalate if it ignores SIGTERM, so quitting never leaves a
                // tunnel up.
                if (!wait_for_exit(std::chrono::milliseconds(2000))) {
                    ::kill(-pid_, SIGKILL);
                    wait_for_exit(std::chrono::milliseconds(2000));
                }
            }
            // Always reap, whatever path we took.
            int status = 0;
            ::waitpid(pid_, &status, WNOHANG);
            exited_ = true;
        }
        if (stdout_fd_ >= 0) { ::close(stdout_fd_); stdout_fd_ = -1; }
        if (stderr_fd_ >= 0) { ::close(stderr_fd_); stderr_fd_ = -1; }
        pid_ = 0;
    }

private:
    bool process_is_still_ours() const
    {
        const std::optional<ProcessIdentity> live = probe_process(pid_);
        if (!live)
            return false;
        return live->cmdline == argv_ && live->start_time == start_time_;
    }

    bool wait_for_exit(std::chrono::milliseconds budget)
    {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            const pid_t r = ::waitpid(pid_, &status, WNOHANG);
            if (r == pid_ || r < 0) {
                exited_ = true;
                return true;
            }
            ::usleep(50 * 1000);
        }
        return false;
    }

    std::vector<std::string> argv_;
    pid_t pid_ = 0;
    int stdout_fd_ = -1;
    int stderr_fd_ = -1;
    bool exited_ = false;
    int exit_code_ = 0;
    long long start_time_ = 0;
};

} // namespace

ProcessFactory default_process_factory()
{
    return [] { return std::unique_ptr<IProcess>(new PosixProcess()); };
}

void force_kill_process(long pid)
{
    if (pid > 1)
        ::kill(static_cast<pid_t>(pid), SIGKILL);
}

#if defined(__linux__) || defined(__HAIKU__)

std::optional<ProcessIdentity> probe_process(long pid)
{
    if (pid <= 0)
        return std::nullopt;
    const std::string base = "/proc/" + std::to_string(pid);

    // comm + start-time from stat. comm is parenthesised and may contain spaces
    // and ')', so split on the LAST ')' before tokenising the numeric fields.
    std::string stat;
    {
        std::ifstream in(base + "/stat", std::ios::binary);
        if (!in)
            return std::nullopt;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        stat = buffer.str();
    }
    const auto open_paren = stat.find('(');
    const auto close_paren = stat.rfind(')');
    if (open_paren == std::string::npos || close_paren == std::string::npos
        || close_paren < open_paren)
        return std::nullopt;

    ProcessIdentity id;
    id.exe = stat.substr(open_paren + 1, close_paren - open_paren - 1);

    // Fields after the ')': field 3 (state) onward. starttime is field 22, i.e.
    // index 19 of the post-')' tokens (field N -> token N-3).
    std::istringstream rest(stat.substr(close_paren + 1));
    std::vector<std::string> tokens;
    std::string token;
    while (rest >> token)
        tokens.push_back(token);
    if (tokens.size() > 19) {
        try {
            id.start_time = std::stoll(tokens[19]);
        } catch (...) {
            return std::nullopt;
        }
    }

    // cmdline: NUL-separated argv.
    std::ifstream cmd(base + "/cmdline", std::ios::binary);
    if (cmd) {
        std::string raw((std::istreambuf_iterator<char>(cmd)),
                        std::istreambuf_iterator<char>());
        std::string arg;
        for (char c : raw) {
            if (c == '\0') {
                if (!arg.empty() || !id.cmdline.empty())
                    id.cmdline.push_back(arg);
                arg.clear();
            } else {
                arg.push_back(c);
            }
        }
        if (!arg.empty())
            id.cmdline.push_back(arg);
    }
    return id;
}

#elif defined(__APPLE__)

std::optional<ProcessIdentity> probe_process(long pid)
{
    if (pid <= 0)
        return std::nullopt;

    struct proc_bsdinfo info {};
    const int n = ::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0,
                                 &info, sizeof(info));
    if (n < static_cast<int>(sizeof(info)))
        return std::nullopt; // no such process / not inspectable

    ProcessIdentity id;
    id.exe = info.pbi_name[0] ? info.pbi_name : info.pbi_comm;
    id.start_time = static_cast<long long>(info.pbi_start_tvsec);

    // argv via KERN_PROCARGS2: [argc:int][exec_path\0][padding][argv0\0...].
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, static_cast<int>(pid)};
    size_t size = 0;
    if (::sysctl(mib, 3, nullptr, &size, nullptr, 0) == 0 && size > sizeof(int)) {
        std::string buffer(size, '\0');
        if (::sysctl(mib, 3, buffer.data(), &size, nullptr, 0) == 0) {
            int argc = 0;
            std::memcpy(&argc, buffer.data(), sizeof(argc));
            size_t pos = sizeof(argc);
            // Skip the exec path and any NUL padding before argv[0].
            while (pos < size && buffer[pos] != '\0') ++pos;
            while (pos < size && buffer[pos] == '\0') ++pos;
            for (int i = 0; i < argc && pos < size; ++i) {
                const size_t startpos = pos;
                while (pos < size && buffer[pos] != '\0') ++pos;
                id.cmdline.emplace_back(buffer.data() + startpos, pos - startpos);
                while (pos < size && buffer[pos] == '\0') ++pos;
            }
        }
    }
    return id;
}

#else

std::optional<ProcessIdentity> probe_process(long)
{
    // No inspection mechanism on this POSIX variant: fail-closed. The reaper
    // treats nullopt as unverifiable and never kills.
    return std::nullopt;
}

#endif

} // namespace haiku_remote

#endif // !_WIN32
