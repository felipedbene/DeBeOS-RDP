#include "haiku_remote/managed_transport.hpp"

#include "haiku_remote/json.hpp"
#include "haiku_remote/profile_store.hpp" // atomic_write_file, config_dir

#include <atomic>
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
// Windows process + socket management (Part 4). winsock2.h must precede
// windows.h; ws2tcpip.h brings inet_pton/getaddrinfo. UNVERIFIED on this host.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
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
#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/sysctl.h>
#endif
#endif

namespace haiku_remote {

namespace {

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
// CancelToken
// ---------------------------------------------------------------------------

CancelToken::CancelToken()
{
#if !defined(_WIN32)
    if (::pipe(pipe_) == 0) {
        for (const int fd : pipe_) {
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        }
    } else {
        pipe_[0] = pipe_[1] = -1;
    }
#endif
}

CancelToken::~CancelToken()
{
#if !defined(_WIN32)
    for (const int fd : pipe_)
        if (fd >= 0)
            ::close(fd);
#endif
}

void CancelToken::cancel()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_.exchange(true))
        return;
#if !defined(_WIN32)
    // One byte, never read: the descriptor stays readable for every later
    // poll(), so a wait that starts after cancel() still returns at once.
    if (pipe_[1] >= 0) {
        const char byte = 1;
        const ssize_t written = ::write(pipe_[1], &byte, 1);
        (void)written;
    }
#endif
    // Under the lock, so a Hook being destroyed concurrently waits for its
    // action to finish instead of racing it.
    for (auto& entry : hooks_)
        if (entry.second)
            entry.second();
}

CancelToken::Hook CancelToken::on_cancel(std::function<void()> action)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_.load()) {
        if (action)
            action();
        return Hook {};
    }
    const std::uint64_t id = next_id_++;
    hooks_.emplace(id, std::move(action));
    return Hook {this, id};
}

CancelToken::Hook& CancelToken::Hook::operator=(Hook&& other) noexcept
{
    if (this != &other) {
        reset();
        token_ = other.token_;
        id_ = other.id_;
        other.token_ = nullptr;
    }
    return *this;
}

void CancelToken::Hook::reset()
{
    if (token_ == nullptr)
        return;
    std::lock_guard<std::mutex> lock(token_->mutex_);
    token_->hooks_.erase(id_);
    token_ = nullptr;
}

namespace {

// Split a byte stream into lines for a StderrSink, carrying an unfinished last
// line in `partial` until its newline (or flush_lines) arrives.
void feed_lines(std::string& partial, const std::string& bytes,
                const StderrSink& sink)
{
    partial += bytes;
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = partial.find('\n', start);
        if (nl == std::string::npos)
            break;
        std::string line = partial.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (sink)
            sink(line);
        else
            std::fprintf(stderr, "%s\n", line.c_str());
        start = nl + 1;
    }
    partial.erase(0, start);
}

void flush_lines(std::string& partial, const StderrSink& sink)
{
    if (!partial.empty()) {
        if (sink)
            sink(partial);
        else
            std::fprintf(stderr, "%s\n", partial.c_str());
    }
    partial.clear();
}

} // namespace

// ---------------------------------------------------------------------------
// ManagedProcess
// ---------------------------------------------------------------------------

namespace detail {

// A child's captured stderr: a reader thread drains the pipe into a bounded
// log (stderr_text()) and a bounded queue of unread bytes (read_stderr()), so
// the child never blocks on a full pipe, whoever reads and however rarely.
class StderrCapture {
public:
    StderrCapture() = default;
    ~StderrCapture() { finish(); }
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

#if !defined(_WIN32)
    // Takes ownership of `fd`, the pipe's read end.
    bool start(int fd, std::string& error)
    {
        if (::pipe(notify_) != 0) {
            error = std::string("pipe failed: ") + std::strerror(errno);
            ::close(fd);
            notify_[0] = notify_[1] = -1;
            return false;
        }
        for (const int n : notify_) {
            ::fcntl(n, F_SETFD, FD_CLOEXEC);
            ::fcntl(n, F_SETFL, ::fcntl(n, F_GETFL, 0) | O_NONBLOCK);
        }
        fd_ = fd;
        ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
        thread_ = std::thread([this] { run(); });
        return true;
    }
#else
    // Takes ownership of `handle`, the anonymous pipe's read end.
    bool start(HANDLE handle, std::string& error)
    {
        (void)error;
        handle_ = handle;
        thread_ = std::thread([this] { run(); });
        return true;
    }
#endif

    // The unread bytes, oldest first, and forget them.
    std::string take()
    {
#if !defined(_WIN32)
        // The wake-ups first, then the bytes: a byte appended after this drain
        // writes a new wake-up, so no appended byte is ever left without one.
        if (notify_[0] >= 0) {
            char sink[256];
            for (;;) {
                const ssize_t n = ::read(notify_[0], sink, sizeof(sink));
                if (n > 0 || (n < 0 && errno == EINTR))
                    continue;
                break; // drained (EAGAIN), or the reader closed its end
            }
        }
#endif
        std::string out;
        bool ended = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            out = with_marker(pending_, pending_dropped_);
            pending_.clear();
            pending_dropped_ = 0;
            ended = ended_;
            if (ended)
                drained_ = true; // nothing can be appended after ended_
        }
#if !defined(_WIN32)
        // The stream is over and read to its end: a descriptor left at EOF
        // would make every later poll() return at once.
        if (ended && notify_[0] >= 0) {
            ::close(notify_[0]);
            notify_[0] = -1;
        }
#endif
        return out;
    }

    std::string text() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return with_marker(log_, log_dropped_);
    }

#if !defined(_WIN32)
    int notify_fd() const { return notify_[0]; }
#endif

    // True until the stream has ended AND every byte of it was taken.
    bool open() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return !(ended_ && drained_);
    }

    // Stop the reader -- after it drained whatever the pipe still holds -- and
    // close the pipe. Idempotent. What was captured stays readable.
    void finish()
    {
        stop_.store(true);
        if (thread_.joinable())
            thread_.join();
#if !defined(_WIN32)
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (notify_[1] >= 0) {
            ::close(notify_[1]);
            notify_[1] = -1;
        }
#else
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
        }
#endif
        std::lock_guard<std::mutex> lock(mutex_);
        ended_ = true;
    }

private:
    static void bounded_append(std::string& into, std::uint64_t& dropped,
                               const char* data, std::size_t size)
    {
        into.append(data, size);
        if (into.size() > process_stderr_capacity) {
            const std::size_t excess = into.size() - process_stderr_capacity;
            into.erase(0, excess);
            dropped += excess;
        }
    }

    static std::string with_marker(const std::string& text, std::uint64_t dropped)
    {
        if (dropped == 0)
            return text;
        return "[... " + std::to_string(dropped) + " earlier bytes dropped ...]\n"
               + text;
    }

    void append(const char* data, std::size_t size)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bounded_append(log_, log_dropped_, data, size);
            bounded_append(pending_, pending_dropped_, data, size);
        }
#if !defined(_WIN32)
        const char byte = 1;
        const ssize_t written = ::write(notify_[1], &byte, 1); // full: still awake
        (void)written;
#endif
    }

    void run()
    {
        char buffer[4096];
#if !defined(_WIN32)
        for (;;) {
            const bool stopping = stop_.load();
            pollfd pfd {fd_, POLLIN, 0};
            const int ready = stopping ? 1 : ::poll(&pfd, 1, 50);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0)
                break;
            if (ready == 0)
                continue;
            const ssize_t n = ::read(fd_, buffer, sizeof(buffer));
            if (n > 0) {
                append(buffer, static_cast<std::size_t>(n));
                continue; // when stopping: until the pipe is empty
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !stopping)
                continue;
            break; // EOF, a hard error, or stopping with the pipe empty
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ended_ = true;
        }
        // Wakes a poll() on notify_fd() with EOF once the last byte is in.
        ::close(notify_[1]);
        notify_[1] = -1;
#else
        for (;;) {
            const bool stopping = stop_.load();
            DWORD available = 0;
            if (!::PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr))
                break; // ERROR_BROKEN_PIPE: every writer is gone
            if (available > 0) {
                DWORD got = 0;
                const DWORD want = available < sizeof(buffer)
                                       ? available
                                       : static_cast<DWORD>(sizeof(buffer));
                // Never more than PeekNamedPipe said is there, so this never
                // blocks.
                if (!::ReadFile(handle_, buffer, want, &got, nullptr) || got == 0)
                    break;
                append(buffer, got);
                continue;
            }
            if (stopping)
                break;
            ::Sleep(20);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ended_ = true;
#endif
    }

    mutable std::mutex mutex_;
    std::string log_;
    std::uint64_t log_dropped_ = 0;
    std::string pending_;
    std::uint64_t pending_dropped_ = 0;
    bool ended_ = false;
    bool drained_ = false;
    std::atomic<bool> stop_ {false};
    std::thread thread_;
#if !defined(_WIN32)
    int fd_ = -1;
    int notify_[2] = {-1, -1};
#else
    HANDLE handle_ = nullptr;
#endif
};

} // namespace detail

ManagedProcess::~ManagedProcess()
{
    terminate();
    close_stderr();
}

void ManagedProcess::close_stderr()
{
    if (capture_)
        capture_->finish();
}

bool ManagedProcess::spawn(const std::vector<std::string>& argv, std::string& error)
{
    return spawn_impl(argv, false, error);
}

bool ManagedProcess::spawn_capturing_stderr(const std::vector<std::string>& argv,
                                            std::string& error)
{
    return spawn_impl(argv, true, error);
}

std::string ManagedProcess::read_stderr()
{
    return capture_ ? capture_->take() : std::string();
}

int ManagedProcess::stderr_fd() const
{
#if !defined(_WIN32)
    return capture_ ? capture_->notify_fd() : -1;
#else
    return -1;
#endif
}

bool ManagedProcess::stderr_open() const
{
    return capture_ && capture_->open();
}

std::string ManagedProcess::stderr_text() const
{
    return capture_ ? capture_->text() : std::string();
}

ManagedProcess::ManagedProcess(ManagedProcess&& other) noexcept
    : pid_(other.pid_), program_pid_(other.program_pid_), reaped_(other.reaped_),
      capture_(std::move(other.capture_)),
      program_start_(std::move(other.program_start_)),
      argv_(std::move(other.argv_)), record_dir_(std::move(other.record_dir_)),
      record_file_(std::move(other.record_file_))
#if !defined(_WIN32)
    , lifeline_fd_(other.lifeline_fd_)
#endif
#if defined(_WIN32)
    , process_handle_(other.process_handle_), job_handle_(other.job_handle_)
#endif
{
    other.pid_ = -1;
    other.program_pid_ = -1;
    other.reaped_ = true;
    other.program_start_.clear();
    other.record_file_.clear();
#if !defined(_WIN32)
    other.lifeline_fd_ = -1;
#endif
#if defined(_WIN32)
    other.process_handle_ = nullptr;
    other.job_handle_ = nullptr;
#endif
}

ManagedProcess& ManagedProcess::operator=(ManagedProcess&& other) noexcept
{
    if (this != &other) {
        terminate();
        close_stderr();
        pid_ = other.pid_;
        program_pid_ = other.program_pid_;
        reaped_ = other.reaped_;
        capture_ = std::move(other.capture_);
        program_start_ = std::move(other.program_start_);
        argv_ = std::move(other.argv_);
        record_dir_ = std::move(other.record_dir_);
        record_file_ = std::move(other.record_file_);
        other.program_start_.clear();
        other.record_file_.clear();
        other.pid_ = -1;
        other.program_pid_ = -1;
        other.reaped_ = true;
#if !defined(_WIN32)
        lifeline_fd_ = other.lifeline_fd_;
        other.lifeline_fd_ = -1;
#endif
#if defined(_WIN32)
        process_handle_ = other.process_handle_;
        job_handle_ = other.job_handle_;
        other.process_handle_ = nullptr;
        other.job_handle_ = nullptr;
#endif
    }
    return *this;
}

#if !defined(_WIN32)

namespace {

volatile sig_atomic_t guard_stop_signal = 0;

void guard_on_signal(int signal_number) { guard_stop_signal = signal_number; }

// SIGTERM to `child`, then up to ~1.5 s for it to exit, then SIGKILL; always
// reaps. Shorter than terminate()'s 2 s grace, so a guard asked to stop is done
// before its owner would escalate to SIGKILLing the guard itself -- which would
// orphan the program. Async-signal-safe only: this runs in a fork of a
// possibly multithreaded process that never execs.
int guard_stop_child(pid_t child)
{
    ::kill(child, SIGTERM);
    int status = 0;
    for (int i = 0; i < 150; ++i) {
        if (::waitpid(child, &status, WNOHANG) == child)
            return status;
        const timespec ten_ms {0, 10 * 1000 * 1000};
        ::nanosleep(&ten_ms, nullptr);
    }
    ::kill(child, SIGKILL);
    ::waitpid(child, &status, 0);
    return status;
}

// Leaves the guard the way the program left: same exit code, or the same
// fatal signal, so a caller inspecting the guard's status sees the program's.
[[noreturn]] void guard_exit_like(int status)
{
    if (WIFSIGNALED(status)) {
        const int signal_number = WTERMSIG(status);
        ::signal(signal_number, SIG_DFL);
        ::kill(::getpid(), signal_number);
        _exit(128 + signal_number);
    }
    _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
}

// After a failed execvp() in a forked child: say so on stderr, then exit 127.
// Exit 127 alone is ambiguous for ssh -- it is also what a *remote* command
// that was not found returns through ssh -- so the failure classifier keys on
// this line instead. Async-signal-safe calls only: the parent may be
// multithreaded.
[[noreturn]] void report_exec_failure(const char* program, int error_number)
{
    const auto put = [](const char* text) {
        const ssize_t n = ::write(STDERR_FILENO, text, std::strlen(text));
        (void)n;
    };
    put("haiku-remote: cannot execute ");
    put(program);
    switch (error_number) {
    case ENOENT:
        put(": No such file or directory\n");
        break;
    case EACCES:
        put(": Permission denied\n");
        break;
    default:
        put(": exec failed\n");
        break;
    }
    _exit(127);
}

// The guard: everything between fork() and _exit() here must be
// async-signal-safe, because the parent may be multithreaded and the guard
// never execs. `stderr_write`, when >= 0, becomes the program's stderr.
[[noreturn]] void run_guard(char* const* argv, int lifeline_read, int report_write,
                            int stderr_write)
{
    // Before the close loop below, which then drops the original descriptor.
    if (stderr_write >= 0)
        ::dup2(stderr_write, STDERR_FILENO);
    // Only the lifeline, the report pipe and stdio may stay open: an inherited
    // copy of ANOTHER ManagedProcess's lifeline write end would keep that
    // lifeline from ever reading EOF.
    long open_max = ::sysconf(_SC_OPEN_MAX);
    if (open_max < 0 || open_max > 65536)
        open_max = 65536;
    for (int fd = 3; fd < open_max; ++fd)
        if (fd != lifeline_read && fd != report_write)
            ::close(fd);
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        if (devnull > STDIN_FILENO)
            ::close(devnull);
    }

    sigset_t none;
    sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
    struct sigaction stop {};
    stop.sa_handler = guard_on_signal;
    sigemptyset(&stop.sa_mask);
    ::sigaction(SIGTERM, &stop, nullptr);
    ::sigaction(SIGHUP, &stop, nullptr);
    ::sigaction(SIGINT, &stop, nullptr);

    const pid_t guard = ::getpid();
    const pid_t child = ::fork();
    if (child < 0)
        _exit(127);
    if (child == 0) {
        ::close(lifeline_read);
        ::close(report_write);
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        ::sigaction(SIGTERM, &dfl, nullptr);
        ::sigaction(SIGHUP, &dfl, nullptr);
        ::sigaction(SIGINT, &dfl, nullptr);
#if defined(__linux__)
        // If the guard itself is killed -- SIGKILLed along with the client by
        // a `killall -9`, which matches the guard by the client's name -- the
        // kernel sends the program SIGTERM. (Survives the exec; macOS has no
        // equivalent, which is what the orphan records are for.) The getppid()
        // check closes the window where the guard died before prctl().
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != guard)
            _exit(127);
#else
        (void)guard;
#endif
        ::execvp(argv[0], argv);
        report_exec_failure(argv[0], errno);
    }
    const ssize_t reported = ::write(report_write, &child, sizeof(child));
    (void)reported;
    ::close(report_write);

    for (;;) {
        if (guard_stop_signal != 0)
            guard_exit_like(guard_stop_child(child));
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) == child)
            guard_exit_like(status);
        pollfd pfd {};
        pfd.fd = lifeline_read;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 200);
        if (ready > 0) {
            char byte = 0;
            const ssize_t n = ::read(lifeline_read, &byte, 1);
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                // EOF: the owner is gone, however it went. Nothing is left to
                // report to, so the program's status is not the point.
                (void)guard_stop_child(child);
                _exit(0);
            }
        }
    }
}

} // namespace

bool ManagedProcess::spawn_impl(const std::vector<std::string>& argv,
                                bool capture_stderr, std::string& error)
{
    if (argv.empty()) {
        error = "cannot spawn an empty command";
        return false;
    }
    close_stderr();
    // argv must outlive the child's execvp; build a NUL-terminated char* vector
    // from the caller's strings (which stay alive for the duration of this call).
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& arg : argv)
        cargv.push_back(const_cast<char*>(arg.c_str()));
    cargv.push_back(nullptr);

    int lifeline[2];
    int report[2];
    if (::pipe(lifeline) != 0) {
        error = std::string("pipe failed: ") + std::strerror(errno);
        return false;
    }
    if (::pipe(report) != 0) {
        error = std::string("pipe failed: ") + std::strerror(errno);
        ::close(lifeline[0]);
        ::close(lifeline[1]);
        return false;
    }
    // Close-on-exec everywhere, so no program this process or a sibling
    // ManagedProcess execs ever holds a lifeline end. (The guard execs nothing
    // and closes stray descriptors itself.)
    for (const int fd : {lifeline[0], lifeline[1], report[0], report[1]})
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    int err_pipe[2] = {-1, -1};
    if (capture_stderr) {
        if (::pipe(err_pipe) != 0) {
            error = std::string("pipe failed: ") + std::strerror(errno);
            for (const int fd : {lifeline[0], lifeline[1], report[0], report[1]})
                ::close(fd);
            return false;
        }
        for (const int fd : err_pipe)
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        for (const int fd : {lifeline[0], lifeline[1], report[0], report[1],
                             err_pipe[0], err_pipe[1]})
            if (fd >= 0)
                ::close(fd);
        return false;
    }
    if (pid == 0) {
        ::close(lifeline[1]);
        ::close(report[0]);
        run_guard(cargv.data(), lifeline[0], report[1], err_pipe[1]);
    }
    ::close(lifeline[0]);
    ::close(report[1]);
    capture_.reset();
    if (capture_stderr) {
        ::close(err_pipe[1]);
        capture_ = std::make_shared<detail::StderrCapture>();
        std::string capture_error;
        if (!capture_->start(err_pipe[0], capture_error)) {
            // The child is running; without a reader its stderr would fill and
            // block it, so do not leave it running.
            capture_.reset();
            pid_ = pid;
            lifeline_fd_ = lifeline[1];
            reaped_ = false;
            ::close(report[0]);
            terminate();
            error = capture_error;
            return false;
        }
    }
    pid_t program = -1;
    ssize_t got = -1;
    do {
        got = ::read(report[0], &program, sizeof(program));
    } while (got < 0 && errno == EINTR);
    ::close(report[0]);
    pid_ = pid;
    program_pid_ = got == static_cast<ssize_t>(sizeof(program)) ? program : -1;
    lifeline_fd_ = lifeline[1];
    reaped_ = false;
    argv_ = argv;
    // Identified while it is certainly ours: the guard has not even been able
    // to reap it yet. (Its start time is the fork's, which exec keeps.)
    program_start_ = program_pid_ > 0 ? process_start_identity(program_pid_)
                                      : std::string();
    write_record();
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
    after_reap();
    return false;
}

void ManagedProcess::close_lifeline()
{
    if (lifeline_fd_ >= 0) {
        ::close(lifeline_fd_);
        lifeline_fd_ = -1;
    }
}

void ManagedProcess::terminate()
{
    if (pid_ <= 0 || reaped_) {
        close_lifeline();
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
    close_lifeline();
    pid_ = -1;
    after_reap();
}

int SystemCommandRunner::run(const std::vector<std::string>& argv, std::string& out,
                             std::string& error)
{
    out.clear();
    if (argv.empty()) {
        error = "cannot run an empty command";
        return -1;
    }
    if (cancel_ != nullptr && cancel_->cancelled()) {
        error = "cancelled";
        return -1;
    }
    // stdout always; stderr only when someone wants ssh's diagnostics,
    // otherwise it stays inherited exactly as before.
    const bool capture_stderr = static_cast<bool>(stderr_sink_);
    int out_pipe[2];
    int err_pipe[2] = {-1, -1};
    if (::pipe(out_pipe) != 0) {
        error = std::string("pipe failed: ") + std::strerror(errno);
        return -1;
    }
    if (capture_stderr && ::pipe(err_pipe) != 0) {
        error = std::string("pipe failed: ") + std::strerror(errno);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return -1;
    }
    for (const int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]})
        if (fd >= 0)
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& arg : argv)
        cargv.push_back(const_cast<char*>(arg.c_str()));
    cargv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        for (const int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]})
            if (fd >= 0)
                ::close(fd);
        return -1;
    }
    if (pid == 0) {
        // dup2 clears close-on-exec on the new descriptor; the originals are
        // closed by exec.
        ::dup2(out_pipe[1], STDOUT_FILENO);
        if (err_pipe[1] >= 0)
            ::dup2(err_pipe[1], STDERR_FILENO);
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            if (devnull > STDIN_FILENO)
                ::close(devnull);
        }
        ::execvp(cargv[0], cargv.data());
        report_exec_failure(cargv[0], errno);
    }
    ::close(out_pipe[1]);
    if (err_pipe[1] >= 0)
        ::close(err_pipe[1]);
    int out_fd = out_pipe[0];
    int err_fd = err_pipe[0];
    const int cancel_fd = cancel_ != nullptr ? cancel_->wait_fd() : -1;
    std::string err_partial;

    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(timeout_seconds_);
    bool timed_out = false;
    bool cancelled = false;
    // Until both stdout and stderr reach EOF, i.e. the child (and anything it
    // left holding them) is gone.
    while (out_fd >= 0 || err_fd >= 0) {
        if (cancel_ != nullptr && cancel_->cancelled()) {
            cancelled = true;
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timed_out = true;
            break;
        }
        const auto remaining_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                .count();
        pollfd pfds[3] {};
        nfds_t count = 0;
        int out_slot = -1;
        int err_slot = -1;
        if (out_fd >= 0) {
            out_slot = static_cast<int>(count);
            pfds[count++] = pollfd {out_fd, POLLIN, 0};
        }
        if (err_fd >= 0) {
            err_slot = static_cast<int>(count);
            pfds[count++] = pollfd {err_fd, POLLIN, 0};
        }
        if (cancel_fd >= 0)
            pfds[count++] = pollfd {cancel_fd, POLLIN, 0};
        const int pr = ::poll(pfds, count, static_cast<int>(remaining_ms) + 1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue; // the deadline check above decides
        const auto drain = [](int& fd, std::string& into) {
            char buffer[4096];
            const ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n > 0) {
                into.append(buffer, static_cast<std::size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                ::close(fd); // EOF
                fd = -1;
            }
        };
        if (out_slot >= 0 && (pfds[out_slot].revents & (POLLIN | POLLHUP | POLLERR)))
            drain(out_fd, out);
        if (err_slot >= 0 && (pfds[err_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            std::string chunk;
            drain(err_fd, chunk);
            feed_lines(err_partial, chunk, stderr_sink_);
        }
    }
    for (int* fd : {&out_fd, &err_fd})
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    flush_lines(err_partial, stderr_sink_);

    if (timed_out || cancelled) {
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
        error = cancelled ? std::string("cancelled")
                          : "command timed out after "
                                + std::to_string(timeout_seconds_) + "s";
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

#else // _WIN32: Part 4 implementation. UNVERIFIED on this host (no Windows
      // toolchain here) -- written to the same contract as the POSIX path, but
      // never compiled or run; a real Windows build must validate it.

namespace {

// Quote one argv element for a Windows command line per the CommandLineToArgvW
// rules MSVC's CRT parses: wrap in quotes, double any run of backslashes that
// precedes a quote (or the closing quote), and escape embedded quotes.
std::string quote_windows_arg(const std::string& arg)
{
    // An argument with no spaces, tabs, or quotes needs no quoting at all.
    if (!arg.empty()
        && arg.find_first_of(" \t\n\v\"") == std::string::npos)
        return arg;

    std::string out;
    out.push_back('"');
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            // Escape all trailing backslashes so they are not read as escaping
            // the closing quote.
            out.append(backslashes * 2, '\\');
            break;
        }
        if (arg[i] == '"') {
            // Escape the run of backslashes and the quote itself.
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back('"');
    return out;
}

std::string build_command_line(const std::vector<std::string>& argv)
{
    std::string line;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0)
            line.push_back(' ');
        line += quote_windows_arg(argv[i]);
    }
    return line;
}

// Start argv[0] with its stdin bound to NUL (mirroring the POSIX /dev/null) and
// stdout optionally redirected to `stdout_write`. The child is created in a new
// kill-on-close job object so it can never be orphaned. Returns the job and
// process HANDLEs (both null on failure) and the pid.
bool spawn_in_job(const std::vector<std::string>& argv, HANDLE stdout_write,
                  HANDLE stderr_write, HANDLE& job_out, HANDLE& process_out,
                  DWORD& pid_out, std::string& error)
{
    job_out = nullptr;
    process_out = nullptr;
    pid_out = 0;
    if (argv.empty()) {
        error = "cannot spawn an empty command";
        return false;
    }

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        error = "CreateJobObject failed (" + std::to_string(::GetLastError()) + ")";
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                   &limits, sizeof(limits))) {
        error = "SetInformationJobObject failed ("
                + std::to_string(::GetLastError()) + ")";
        ::CloseHandle(job);
        return false;
    }

    // Bind stdin to NUL so ssh cannot grab a console for a prompt.
    SECURITY_ATTRIBUTES inherit {};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &inherit, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (nul != INVALID_HANDLE_VALUE) ? nul : nullptr;
    si.hStdOutput = (stdout_write != nullptr)
                        ? stdout_write
                        : ::GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = (stderr_write != nullptr) ? stderr_write
                                             : ::GetStdHandle(STD_ERROR_HANDLE);

    // CreateProcessW may write to the command-line buffer, so give it a mutable
    // wide copy.
    const std::string command = build_command_line(argv);
    const int wide_len =
        ::MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wide(static_cast<std::size_t>(wide_len > 0 ? wide_len : 1));
    ::MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, wide.data(), wide_len);

    PROCESS_INFORMATION pi {};
    // CREATE_SUSPENDED so the child is assigned to the job before it runs;
    // CREATE_NO_WINDOW keeps ssh from flashing a console window.
    const BOOL ok = ::CreateProcessW(
        nullptr, wide.data(), nullptr, nullptr, /*inherit=*/TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (nul != INVALID_HANDLE_VALUE)
        ::CloseHandle(nul);
    if (!ok) {
        error = "CreateProcess failed (" + std::to_string(::GetLastError()) + ")";
        ::CloseHandle(job);
        return false;
    }

    if (!::AssignProcessToJobObject(job, pi.hProcess)) {
        // Could not place it in the job: kill it rather than let it run free.
        error = "AssignProcessToJobObject failed ("
                + std::to_string(::GetLastError()) + ")";
        ::TerminateProcess(pi.hProcess, 1);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(job);
        return false;
    }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);

    job_out = job;
    process_out = pi.hProcess;
    pid_out = pi.dwProcessId;
    return true;
}

} // namespace

bool ManagedProcess::spawn_impl(const std::vector<std::string>& argv,
                                bool capture_stderr, std::string& error)
{
    close_stderr();
    capture_.reset();
    // An inheritable pipe for the child's stderr; the read end stays private
    // so no other child holds it open.
    HANDLE err_read = nullptr;
    HANDLE err_write = nullptr;
    if (capture_stderr) {
        SECURITY_ATTRIBUTES sa {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!::CreatePipe(&err_read, &err_write, &sa, 0)) {
            error = "CreatePipe failed (" + std::to_string(::GetLastError()) + ")";
            return false;
        }
        ::SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    }
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    DWORD pid = 0;
    const bool ok = spawn_in_job(argv, nullptr, err_write, job, process, pid, error);
    if (err_write != nullptr)
        ::CloseHandle(err_write); // the child has its copy; EOF when it exits
    if (!ok) {
        if (err_read != nullptr)
            ::CloseHandle(err_read);
        return false;
    }
    job_handle_ = job;
    process_handle_ = process;
    pid_ = static_cast<long>(pid);
    program_pid_ = pid_;
    reaped_ = false;
    argv_ = argv;
    if (err_read != nullptr) {
        capture_ = std::make_shared<detail::StderrCapture>();
        std::string capture_error;
        capture_->start(err_read, capture_error);
    }
    return true;
}

bool ManagedProcess::running()
{
    if (pid_ <= 0 || reaped_ || process_handle_ == nullptr)
        return false;
    const DWORD rc =
        ::WaitForSingleObject(reinterpret_cast<HANDLE>(process_handle_), 0);
    if (rc == WAIT_TIMEOUT)
        return true; // still alive
    // Exited (WAIT_OBJECT_0) or the handle went bad: tear down and mark reaped.
    terminate();
    return false;
}

void ManagedProcess::terminate()
{
    if (process_handle_ != nullptr) {
        HANDLE process = reinterpret_cast<HANDLE>(process_handle_);
        // No clean SIGTERM equivalent for a windowless child: ask it to die and
        // wait briefly, mirroring the POSIX escalation's end state. Closing the
        // kill-on-close job below also takes down anything it spawned.
        ::TerminateProcess(process, 1);
        ::WaitForSingleObject(process, 2000);
        ::CloseHandle(process);
        process_handle_ = nullptr;
    }
    if (job_handle_ != nullptr) {
        ::CloseHandle(reinterpret_cast<HANDLE>(job_handle_));
        job_handle_ = nullptr;
    }
    reaped_ = true;
    pid_ = -1;
    after_reap();
}

int SystemCommandRunner::run(const std::vector<std::string>& argv, std::string& out,
                             std::string& error)
{
    out.clear();
    if (argv.empty()) {
        error = "cannot run an empty command";
        return -1;
    }

    // An inheritable pipe for the child's stdout; keep the read end private.
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        error = "CreatePipe failed (" + std::to_string(::GetLastError()) + ")";
        return -1;
    }
    ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    // stderr only when someone wants ssh's diagnostics (as on POSIX);
    // otherwise it stays inherited.
    HANDLE err_read = nullptr;
    HANDLE err_write = nullptr;
    if (stderr_sink_) {
        if (!::CreatePipe(&err_read, &err_write, &sa, 0)) {
            error = "CreatePipe failed (" + std::to_string(::GetLastError()) + ")";
            ::CloseHandle(read_pipe);
            ::CloseHandle(write_pipe);
            return -1;
        }
        ::SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    }

    HANDLE job = nullptr;
    HANDLE process = nullptr;
    DWORD pid = 0;
    if (!spawn_in_job(argv, write_pipe, err_write, job, process, pid, error)) {
        ::CloseHandle(read_pipe);
        ::CloseHandle(write_pipe);
        if (err_read != nullptr) {
            ::CloseHandle(err_read);
            ::CloseHandle(err_write);
        }
        return -1;
    }
    // The child owns the write ends now; close ours so a read sees EOF at exit.
    ::CloseHandle(write_pipe);
    if (err_write != nullptr)
        ::CloseHandle(err_write);
    std::string err_partial;
    // Whatever stderr holds, without blocking (never more than is there).
    const auto drain_err = [&] {
        DWORD pending = 0;
        char chunk[4096];
        DWORD got = 0;
        while (err_read != nullptr
               && ::PeekNamedPipe(err_read, nullptr, 0, nullptr, &pending, nullptr)
               && pending > 0
               && ::ReadFile(err_read, chunk,
                             pending < sizeof(chunk) ? pending
                                                     : static_cast<DWORD>(sizeof(chunk)),
                             &got, nullptr)
               && got > 0)
            feed_lines(err_partial, std::string(chunk, got), stderr_sink_);
    };

    const DWORD deadline = ::GetTickCount() + static_cast<DWORD>(timeout_seconds_) * 1000;
    bool timed_out = false;
    bool cancelled = false;
    for (;;) {
        DWORD available = 0;
        if (::PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr)
            && available > 0) {
            char buffer[4096];
            DWORD got = 0;
            if (::ReadFile(read_pipe, buffer, sizeof(buffer), &got, nullptr)
                && got > 0)
                out.append(buffer, got);
            continue;
        }
        drain_err();
        // No data pending: is the child done, or have we run out of time?
        if (::WaitForSingleObject(process, 50) == WAIT_OBJECT_0) {
            // Drain anything still buffered after exit.
            DWORD got = 0;
            char buffer[4096];
            while (::PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr)
                   && available > 0
                   && ::ReadFile(read_pipe, buffer, sizeof(buffer), &got, nullptr)
                   && got > 0)
                out.append(buffer, got);
            drain_err();
            break;
        }
        // Checked between the 50 ms waits above: Windows has no descriptor
        // to fold the cancel into, so this is as prompt as cancel gets here.
        if (cancel_ != nullptr && cancel_->cancelled()) {
            cancelled = true;
            break;
        }
        if (::GetTickCount() >= deadline) {
            timed_out = true;
            break;
        }
    }
    ::CloseHandle(read_pipe);
    drain_err();
    if (err_read != nullptr)
        ::CloseHandle(err_read);
    flush_lines(err_partial, stderr_sink_);

    int result;
    if (timed_out || cancelled) {
        ::TerminateProcess(process, 1);
        ::WaitForSingleObject(process, 2000);
        error = cancelled ? std::string("cancelled")
                          : "command timed out after "
                                + std::to_string(timeout_seconds_) + "s";
        result = -1;
    } else {
        DWORD code = 0;
        ::GetExitCodeProcess(process, &code);
        result = static_cast<int>(code);
    }
    ::CloseHandle(process);
    ::CloseHandle(job); // kill-on-close reaps any stragglers
    return result;
}

std::uint16_t pick_free_local_port(std::string& error)
{
    WSADATA wsa {};
    const bool started = (::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        error = "socket failed (" + std::to_string(::WSAGetLastError()) + ")";
        if (started)
            ::WSACleanup();
        return 0;
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0; // let the kernel pick
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    std::uint16_t port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = "bind failed (" + std::to_string(::WSAGetLastError()) + ")";
    } else {
        sockaddr_in bound {};
        int len = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0)
            port = ntohs(bound.sin_port);
        else
            error = "getsockname failed (" + std::to_string(::WSAGetLastError()) + ")";
    }
    ::closesocket(fd);
    if (started)
        ::WSACleanup();
    return port;
}

#endif

// ---------------------------------------------------------------------------
// Verified orphans (see managed_transport.hpp)
// ---------------------------------------------------------------------------

namespace {

#if !defined(_WIN32)
std::string read_whole_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

#endif

#if defined(__linux__)
// The kernel's id for this boot: start times count clock ticks since boot, so
// they are only comparable within one.
const std::string& linux_boot_id()
{
    static const std::string id = [] {
        std::string text = read_whole_file("/proc/sys/kernel/random/boot_id");
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            text.pop_back();
        return text;
    }();
    return id;
}
#endif

long own_pid()
{
#if defined(_WIN32)
    return static_cast<long>(::GetCurrentProcessId());
#else
    return static_cast<long>(::getpid());
#endif
}

#if !defined(_WIN32)
bool program_matches(const ProcessRecord& record)
{
    if (record.program_pid <= 0 || record.program_start.empty())
        return false;
    if (process_start_identity(record.program_pid) != record.program_start)
        return false;
    const auto argv = process_argv(record.program_pid);
    return argv.has_value() && *argv == record.argv;
}

bool owner_alive(const ProcessRecord& record)
{
    return record.owner_pid > 0 && !record.owner_start.empty()
        && process_start_identity(record.owner_pid) == record.owner_start;
}

// Send `signal_number` to the record's program if, and only if, it is still
// verifiably that process.
bool signal_verified(const ProcessRecord& record, int signal_number)
{
#if defined(__linux__) && defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    // The pidfd pins the process it was opened on: once open, a recycled pid
    // cannot be signalled through it. Verify after opening, then signal.
    const int pidfd = static_cast<int>(
        ::syscall(SYS_pidfd_open, static_cast<pid_t>(record.program_pid), 0));
    if (pidfd >= 0) {
        const bool sent = program_matches(record)
            && ::syscall(SYS_pidfd_send_signal, pidfd, signal_number, nullptr, 0) == 0;
        ::close(pidfd);
        return sent;
    }
    if (errno == ESRCH)
        return false; // gone already
    // ENOSYS (a kernel older than 5.3): verify, then kill() -- the window
    // between the two is the only one left.
#endif
    return program_matches(record)
        && ::kill(static_cast<pid_t>(record.program_pid), signal_number) == 0;
}

// SIGTERM, up to 2 s, then SIGKILL, up to 1 s -- each signal verified first.
// True if the program was there and is gone now.
bool stop_verified(const ProcessRecord& record)
{
    if (!signal_verified(record, SIGTERM))
        return false;
    for (int i = 0; i < 100; ++i) {
        if (!program_matches(record))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    signal_verified(record, SIGKILL);
    for (int i = 0; i < 50; ++i) {
        if (!program_matches(record))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
#endif

} // namespace

std::string process_start_identity(long pid)
{
    if (pid <= 0)
        return {};
#if defined(__linux__)
    const std::string stat = read_whole_file("/proc/" + std::to_string(pid) + "/stat");
    // "pid (comm) state ppid ...": comm may hold spaces and parentheses, so
    // the fields start after the LAST ')'. starttime is field 22 overall, the
    // 20th after comm (state is the 1st).
    const std::size_t close = stat.rfind(')');
    if (close == std::string::npos || linux_boot_id().empty())
        return {};
    std::istringstream rest(stat.substr(close + 1));
    std::vector<std::string> fields;
    for (std::string field; rest >> field;)
        fields.push_back(field);
    if (fields.size() < 20)
        return {};
    if (fields[0] == "Z" || fields[0] == "X" || fields[0] == "x")
        return {}; // a zombie has finished; nothing to signal
    return linux_boot_id() + ":" + fields[19];
#elif defined(__APPLE__)
    struct proc_bsdinfo info {};
    const int got = ::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info,
                                   sizeof(info));
    if (got != static_cast<int>(sizeof(info)) || info.pbi_status == SZOMB)
        return {};
    char micro[8];
    std::snprintf(micro, sizeof(micro), "%06llu",
                  static_cast<unsigned long long>(info.pbi_start_tvusec));
    return std::to_string(static_cast<unsigned long long>(info.pbi_start_tvsec)) + "."
           + micro;
#else
    // Haiku, the BSDs, Windows: no start time this code knows how to read, so
    // no process is ever identified -- and none is ever signalled on a record.
    return {};
#endif
}

std::optional<std::vector<std::string>> process_argv(long pid)
{
    if (pid <= 0)
        return std::nullopt;
#if defined(__linux__)
    const std::string raw = read_whole_file("/proc/" + std::to_string(pid) + "/cmdline");
    if (raw.empty())
        return std::nullopt;
    std::vector<std::string> argv;
    std::size_t start = 0;
    while (start < raw.size()) {
        std::size_t end = raw.find('\0', start);
        if (end == std::string::npos)
            end = raw.size();
        argv.push_back(raw.substr(start, end - start));
        start = end + 1;
    }
    return argv;
#elif defined(__APPLE__)
    int argmax = 0;
    std::size_t size = sizeof(argmax);
    int max_mib[2] = {CTL_KERN, KERN_ARGMAX};
    if (::sysctl(max_mib, 2, &argmax, &size, nullptr, 0) != 0 || argmax <= 0)
        return std::nullopt;
    std::vector<char> buffer(static_cast<std::size_t>(argmax));
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, static_cast<int>(pid)};
    size = buffer.size();
    if (::sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0
        || size < sizeof(int))
        return std::nullopt;
    // int argc, the exec path, NUL padding, then argc NUL-terminated strings.
    int argc = 0;
    std::memcpy(&argc, buffer.data(), sizeof(argc));
    std::size_t at = sizeof(argc);
    while (at < size && buffer[at] != '\0')
        ++at;
    while (at < size && buffer[at] == '\0')
        ++at;
    std::vector<std::string> argv;
    for (int i = 0; i < argc && at < size; ++i) {
        const std::size_t begin = at;
        while (at < size && buffer[at] != '\0')
            ++at;
        argv.emplace_back(buffer.data() + begin, at - begin);
        ++at;
    }
    if (static_cast<int>(argv.size()) != argc)
        return std::nullopt;
    return argv;
#else
    return std::nullopt;
#endif
}

std::string serialize_process_record(const ProcessRecord& record)
{
    json::Array argv;
    for (const auto& arg : record.argv)
        argv.emplace_back(arg);
    json::Object owner;
    owner.emplace_back("pid", json::Value(static_cast<std::int64_t>(record.owner_pid)));
    owner.emplace_back("start", json::Value(record.owner_start));
    json::Object program;
    program.emplace_back("pid",
                         json::Value(static_cast<std::int64_t>(record.program_pid)));
    program.emplace_back("start", json::Value(record.program_start));
    program.emplace_back("argv", json::Value(std::move(argv)));
    json::Object root;
    root.emplace_back("kind", json::Value("haiku-remote-process"));
    root.emplace_back("version", json::Value(1));
    root.emplace_back("owner", json::Value(std::move(owner)));
    root.emplace_back("program", json::Value(std::move(program)));
    return json::serialize(json::Value(std::move(root)), true);
}

std::optional<ProcessRecord> parse_process_record(std::string_view text)
{
    const json::ParseResult parsed = json::parse(text);
    if (!parsed.ok || !parsed.value.is_object())
        return std::nullopt;
    const json::Value& root = parsed.value;
    const json::Value* kind = root.find("kind");
    const json::Value* version = root.find("version");
    const json::Value* owner = root.find("owner");
    const json::Value* program = root.find("program");
    if (kind == nullptr || kind->as_string() != "haiku-remote-process"
        || version == nullptr || version->as_int(0) != 1 || owner == nullptr
        || program == nullptr)
        return std::nullopt;
    const auto field = [](const json::Value* object, std::string_view key) {
        return object->find(key);
    };
    const json::Value* owner_pid = field(owner, "pid");
    const json::Value* owner_start = field(owner, "start");
    const json::Value* program_pid = field(program, "pid");
    const json::Value* program_start = field(program, "start");
    const json::Value* argv = field(program, "argv");
    if (owner_pid == nullptr || !owner_pid->is_number() || owner_start == nullptr
        || !owner_start->is_string() || program_pid == nullptr
        || !program_pid->is_number() || program_start == nullptr
        || !program_start->is_string() || argv == nullptr || !argv->is_array())
        return std::nullopt;
    ProcessRecord record;
    record.owner_pid = static_cast<long>(owner_pid->as_int());
    record.owner_start = owner_start->as_string();
    record.program_pid = static_cast<long>(program_pid->as_int());
    record.program_start = program_start->as_string();
    for (const auto& arg : argv->as_array()) {
        if (!arg.is_string())
            return std::nullopt;
        record.argv.push_back(arg.as_string());
    }
    if (record.owner_pid <= 0 || record.program_pid <= 0 || record.owner_start.empty()
        || record.program_start.empty() || record.argv.empty())
        return std::nullopt;
    return record;
}

std::filesystem::path default_process_record_dir()
{
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && *runtime != '\0')
        return std::filesystem::path(runtime) / "haiku-remote" / "processes";
    return ProfileStore::config_dir() / "processes";
}

OrphanSweep sweep_orphaned_processes(const std::filesystem::path& dir)
{
    OrphanSweep sweep;
#if !defined(_WIN32)
    namespace fs = std::filesystem;
    // A platform that cannot identify a process never acts on a record.
    if (dir.empty() || process_start_identity(own_pid()).empty())
        return sweep;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return sweep;
    std::vector<fs::path> records;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".json" && it->is_regular_file(ec))
            records.push_back(it->path());
    }
    for (const auto& path : records) {
        const auto record = parse_process_record(read_whole_file(path));
        if (!record) {
            ++sweep.unreadable;
            fs::remove(path, ec);
            continue;
        }
        if (owner_alive(*record)) {
            ++sweep.live_owner; // its owner still owns it: never ours to touch
            continue;
        }
        if (stop_verified(*record))
            sweep.stopped.push_back(record->program_pid);
        else if (!program_matches(*record))
            ++sweep.stale;
        if (!program_matches(*record))
            fs::remove(path, ec);
    }
#else
    // The kill-on-close job object already took the program down.
    (void)dir;
#endif
    return sweep;
}

void ManagedProcess::write_record()
{
    record_file_.clear();
    if (record_dir_.empty() || program_start_.empty() || program_pid_ <= 0)
        return;
    ProcessRecord record;
    record.owner_pid = own_pid();
    record.owner_start = process_start_identity(record.owner_pid);
    record.program_pid = program_pid_;
    record.program_start = program_start_;
    record.argv = argv_;
    if (record.owner_start.empty())
        return;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(record_dir_, ec);
    fs::permissions(record_dir_, fs::perms::owner_all, fs::perm_options::replace, ec);
    const fs::path file = record_dir_
        / ("process-" + std::to_string(record.owner_pid) + "-"
           + std::to_string(record.program_pid) + ".json");
    std::string error;
    if (atomic_write_file(file, serialize_process_record(record), error))
        record_file_ = file;
}

void ManagedProcess::after_reap()
{
    bool program_gone = true;
#if !defined(_WIN32)
    if (!program_start_.empty() && program_pid_ > 0) {
        ProcessRecord record;
        record.program_pid = program_pid_;
        record.program_start = program_start_;
        record.argv = argv_;
        // Normally the guard took the program down before it exited and this
        // finds nothing. A guard that was SIGKILLed did not, and the program
        // -- verified as the very process spawn() started -- is stopped here.
        stop_verified(record);
        program_gone = !program_matches(record);
    }
#endif
    program_start_.clear();
    if (!record_file_.empty() && program_gone) {
        std::error_code ec;
        std::filesystem::remove(record_file_, ec);
        record_file_.clear();
    }
    close_stderr();
}

// ---------------------------------------------------------------------------
// SSH argv builders
// ---------------------------------------------------------------------------

std::vector<std::string> build_ssh_base_argv(const SshConfig& cfg)
{
    // The hardened option set of DeBeOS-RDP issue #1 ("SSH tunnel lifecycle
    // requirements"), shared by the tunnel and the one-shot cookie/token
    // fetches. Every option is meaningful on both:
    std::vector<std::string> argv = {
        "ssh",
        // Never request a pseudo-terminal, even when the user's ssh_config
        // says RequestTTY: neither invocation has a terminal to give it, and a
        // tty would rewrite the fetched secret's line endings.
        "-T",
        // Non-interactive by construction: never prompt, never hang on a
        // password or an unknown-host question.
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=accept-new",
        // Bound the TCP connect to a black-holed host.
        "-o", "ConnectTimeout=15",
        // Notice a dead peer in 15 s x 3 = 45 s rather than the kernel's TCP
        // timeout (hours): a dead tunnel then exits, the session sees a drop,
        // and the reconnect policy can act on it.
        "-o", "ServerAliveInterval=15",
        "-o", "ServerAliveCountMax=3",
        // Never ride, or become, a multiplexed master from the user's
        // ssh_config. A mux client hands its request to the existing master
        // and ignores most of the options above (-i included), and a master we
        // started would outlive the child we own and reap.
        "-o", "ControlMaster=no",
        "-o", "ControlPath=none",
    };
    // Always spelled out, 22 included: the profile's SSH port is what the route
    // summary shows, so it is what ssh must use -- not whatever an ssh_config
    // `Port` line for the same host happens to say.
    if (cfg.port > 0) {
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
    // ssh's debug1 lines are the tunnel's readiness signal (forward_readiness()
    // in the header): "Local forwarding listening on 127.0.0.1 port P." and
    // "Entering interactive session.". -o rather than -v so the intent is
    // spelled out; on the command line it outranks any LogLevel in ssh_config.
    argv.push_back("-o");
    argv.push_back("LogLevel=DEBUG1");
    // Fail loudly if the forward cannot be set up, rather than connecting with
    // no tunnel and silently waiting.
    argv.push_back("-o");
    argv.push_back("ExitOnForwardFailure=yes");
    // No remote command, just the forward: -N keeps the session open with no
    // shell. We own this child and manage it in the foreground (no -f).
    argv.push_back("-N");
    argv.push_back("-L");
    // The bind address is explicit. Without it ssh binds every loopback
    // address and calls the forward up if ANY bind works, so with another
    // program on 127.0.0.1:P it stays up on [::1]:P only -- and the client,
    // which dials 127.0.0.1:P, reaches that other program and hands it the
    // session cookie. With it, that bind failing is the forward failing, and
    // ExitOnForwardFailure ends ssh.
    argv.push_back("127.0.0.1:" + std::to_string(local_port) + ":" + remote_host
                   + ":" + std::to_string(remote_port));
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
    return spawn(error) && wait_ready(error);
}

ForwardReadinessScanner::ForwardReadinessScanner(std::uint16_t local_port)
    : listening_("Local forwarding listening on 127.0.0.1 port "
                 + std::to_string(local_port) + "."),
      bind_failed_("bind [127.0.0.1]:" + std::to_string(local_port) + ":")
{
}

ForwardReadiness ForwardReadinessScanner::feed(std::string_view line)
{
    if (state_ != ForwardReadiness::pending)
        return state_;
    const auto has = [line](std::string_view needle) {
        return line.find(needle) != std::string_view::npos;
    };
    if (has(bind_failed_) || has("Could not request local forwarding")) {
        state_ = ForwardReadiness::failed;
        why_ = std::string(line);
    } else if (has(listening_)) {
        // Logged before bind(): only the line that follows can mean "bound".
        listening_seen_ = true;
    } else if (listening_seen_ && has("Entering interactive session.")) {
        state_ = ForwardReadiness::ready;
    }
    return state_;
}

ForwardReadiness forward_readiness(std::string_view ssh_stderr,
                                   std::uint16_t local_port, std::string& why)
{
    ForwardReadinessScanner scanner(local_port);
    std::size_t start = 0;
    while (start <= ssh_stderr.size()) {
        std::size_t end = ssh_stderr.find('\n', start);
        if (end == std::string_view::npos)
            end = ssh_stderr.size();
        std::string_view line = ssh_stderr.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        scanner.feed(line);
        start = end + 1;
    }
    why = scanner.why();
    return scanner.state();
}

bool is_ssh_debug_line(std::string_view line)
{
    return line.size() >= 7 && line.substr(0, 5) == "debug" && line[5] >= '1'
        && line[5] <= '3' && line[6] == ':';
}

bool SshTunnel::spawn(std::string& error)
{
    // One implementation for both platforms: the primitives it rests on
    // (pick_free_local_port, ManagedProcess) each have a POSIX and a Win32
    // form. The Win32 forms are UNVERIFIED (see the header).
    //
    // A crashed earlier run's verified orphans go first: an orphaned forward
    // still holds its local port, and the record is the only thing that can
    // tell it apart from someone else's process.
    const std::filesystem::path dir =
        record_dir_ ? *record_dir_ : default_process_record_dir();
    last_sweep_ = dir.empty() ? OrphanSweep {} : sweep_orphaned_processes(dir);
    if (local_port_ == 0) {
        local_port_ = pick_free_local_port(error);
        if (local_port_ == 0)
            return false;
    }
    const std::vector<std::string> argv =
        build_ssh_tunnel_argv(cfg_, local_port_, remote_host_, remote_port_);
    stderr_partial_.clear();
    readiness_ = ForwardReadinessScanner(local_port_);
    process_.record_in(dir);
    // Always captured, sink or not: ssh's stderr is the readiness signal.
    return process_.spawn_capturing_stderr(argv, error);
}

void SshTunnel::feed(const std::string& chunk)
{
    // Every line goes to the readiness scanner; only the lines ssh would print
    // at its default log level go on to the sink (or, without one, to this
    // process's stderr, as an inherited stderr would have shown them).
    const auto deliver = [this](std::string line) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        readiness_.feed(line);
        if (is_ssh_debug_line(line))
            return;
        if (stderr_sink_)
            stderr_sink_(line);
        else
            std::fprintf(stderr, "%s\n", line.c_str());
    };
    stderr_partial_ += chunk;
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = stderr_partial_.find('\n', start);
        if (nl == std::string::npos)
            break;
        deliver(stderr_partial_.substr(start, nl - start));
        start = nl + 1;
    }
    stderr_partial_.erase(0, start);
    if (!process_.stderr_open() && !stderr_partial_.empty()) {
        // The stream ended: nothing will ever end this last line.
        deliver(stderr_partial_);
        stderr_partial_.clear();
    }
}

void SshTunnel::drain_stderr()
{
    feed(process_.read_stderr());
}

bool SshTunnel::wait_ready(std::string& error)
{
    // Wait for ssh to report the forward listening (ForwardReadinessScanner;
    // the header says why those two lines are sound). Nothing connects through
    // the forward: every connection through it reaches app_server.
    //
    // ssh binds the local listener only after the TCP connect and the
    // authentication, so the wait must outlast ConnectTimeout=15 plus an
    // authentication round trip (ready_timeout_ms_, 20 s).
    const std::string where = "127.0.0.1:" + std::to_string(local_port_);
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(ready_timeout_ms_);
    const auto failed = [&] {
        error = "ssh could not open the forward on " + where + ": "
                + readiness_.why();
        process_.terminate(); // never orphan the child
        drain_stderr();
        return false;
    };
    for (;;) {
        drain_stderr();
        if (readiness_.state() == ForwardReadiness::ready)
            return true;
        if (readiness_.state() == ForwardReadiness::failed)
            return failed();
        if (cancel_ != nullptr && cancel_->cancelled()) {
            process_.terminate();
            drain_stderr();
            error = "cancelled";
            return false;
        }
        if (!process_.running()) {
            drain_stderr(); // whatever ssh said on its way out
            if (readiness_.state() == ForwardReadiness::failed)
                return failed();
            error = "ssh exited before the forward on " + where
                    + " came up (check the key, host, and that the remote port"
                      " is listening)";
            return false; // process_ already reaped by running()
        }
        if (std::chrono::steady_clock::now() >= deadline)
            break;
#if defined(_WIN32)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
#else
        // ssh writing to stderr (its readiness lines, or its last words) or
        // the cancel token firing ends the wait at once.
        pollfd pfds[2] {};
        nfds_t count = 0;
        if (process_.stderr_fd() >= 0)
            pfds[count++] = pollfd {process_.stderr_fd(), POLLIN, 0};
        if (cancel_ != nullptr && cancel_->wait_fd() >= 0)
            pfds[count++] = pollfd {cancel_->wait_fd(), POLLIN, 0};
        if (count > 0)
            (void)::poll(pfds, count, 100);
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
    }
    error = "timed out waiting for the ssh forward on " + where
            + " (ssh did not report it listening)";
    process_.terminate(); // never orphan the child on a timeout
    drain_stderr();
    return false;
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
                                  CommandRunner* runner, ConnectObserver* observer,
                                  std::size_t first_step)
{
    // A null-object observer keeps the body free of per-call null checks; the
    // base class's methods are all no-ops, so this is the same as not reporting.
    ConnectObserver noop;
    ConnectObserver& obs = observer ? *observer : noop;
    const CancelToken* cancel = obs.cancel_token();
    const bool capture = obs.wants_ssh_stderr();
    // Which step ssh's stderr is reported against; updated as the walk moves.
    std::size_t current_step = first_step;
    const StderrSink sink = [&obs, &current_step](const std::string& line) {
        obs.on_ssh_stderr(current_step, line);
    };

    SystemCommandRunner system_runner;
    system_runner.set_cancel(cancel);
    if (capture)
        system_runner.set_stderr_sink(sink);
    CommandRunner& use = runner ? *runner : system_runner;
    const auto cancelled = [cancel] {
        return cancel != nullptr && cancel->cancelled();
    };

    const LaunchPlan plan = plan_launch(profile);
    const SshConfig cfg = config_from_profile(profile);
    const std::uint16_t remote_port = clamp_remote_port(profile.remote_port);

    ManagedConnection conn;
    std::string last_error;

    for (std::size_t index = first_step; index < plan.steps.size(); ++index) {
        // A cancelled attempt stops here: it must never fall back to the next
        // route, which would spawn another ssh after the user said stop.
        if (cancelled()) {
            last_error = "cancelled";
            break;
        }
        const RouteStep& step = plan.steps[index];
        current_step = index;
        obs.on_step_begin(index, step.kind);
        if (step.kind == RouteKind::broker) {
            obs.on_phase(index, ConnectPhase::contacting_broker);
            BrokerCredentials creds;
            std::string e;
            if (!fetch_broker_credentials(use, cfg, default_broker_port, creds, e)) {
                last_error = "broker route: " + e;
                obs.on_step_failed(index, RouteKind::broker, e);
                continue;
            }
            TransportOptions t = step.transport;
            obs.on_secret(creds.token);
            t.token = creds.token;
            t.cookie.clear(); // the broker presents app_server's cookie itself
            // The certificate came over SSH, which authenticated the host by
            // its own known_hosts, so it is a sound first-use seed: recorded in
            // known_brokers if host:port has no entry yet, then verified by the
            // transport's trust-on-first-use check. It is NOT passed as
            // --ca-file: the broker's certificate names only the host's
            // internal DNS name and 127.0.0.1, so a chain-plus-name check fails
            // whenever the host is dialed by IP. Seeding never overwrites an
            // existing entry, so a changed certificate still raises the
            // warning. An unparsable PEM seeds nothing and the user is asked.
            Fingerprint fingerprint {};
            std::string fe;
            std::string seeded_note;
            if (fingerprint_pem_certificate(creds.cert_pem, fingerprint, fe)) {
                t.known_broker_fingerprint = format_fingerprint(fingerprint);
                seeded_note = " and certificate " + display_fingerprint(fingerprint)
                              + " (fetched over SSH)";
            } else {
                seeded_note = " (the fetched certificate could not be read: " + fe
                              + "; it will be verified on first use)";
            }
            conn.ok = true;
            conn.kind = RouteKind::broker;
            conn.transport = std::move(t);
            conn.note = "broker (wss): " + conn.transport.url
                        + " with a fetched token" + seeded_note;
            obs.on_step_ready(index, RouteKind::broker);
            return conn;
        }

        if (step.kind == RouteKind::direct) {
            // Nothing to stand up: no ssh runs on this route. The cookie is
            // the plan's own (cookie_source); with none, the transport refuses
            // locally, before opening a socket (missing_credential).
            conn.ok = true;
            conn.kind = RouteKind::direct;
            conn.transport = step.transport;
            conn.note = "direct tcp: client -> " + step.transport.host + ":"
                        + std::to_string(step.transport.port);
            obs.on_step_ready(index, RouteKind::direct);
            return conn;
        }

        // Tunnel route.
        TransportOptions t = step.transport;
        if (t.cookie.empty()) {
            obs.on_phase(index, ConnectPhase::fetching_cookie);
            std::string ck;
            std::string e;
            if (!fetch_session_cookie(use, cfg, remote_port, ck, e)) {
                last_error = "tunnel route: " + e;
                obs.on_step_failed(index, RouteKind::tunnel, e);
                continue;
            }
            obs.on_secret(ck);
            t.cookie = ck;
        }
        obs.on_phase(index, ConnectPhase::opening_tunnel);
        auto tunnel = std::make_unique<SshTunnel>(
            cfg, remote_port, step.transport.port /*0 = auto*/);
        tunnel->set_cancel(cancel);
        if (capture)
            tunnel->set_stderr_sink(sink);
        std::string e;
        bool up = tunnel->spawn(e);
        if (up) {
            obs.on_phase(index, ConnectPhase::waiting_for_forward);
            up = tunnel->wait_ready(e);
        }
        if (!up) {
            last_error = "tunnel route: " + e;
            obs.on_step_failed(index, RouteKind::tunnel, e);
            continue;
        }
        t.url.clear();
        t.host = "127.0.0.1";
        t.port = tunnel->local_port();
        conn.ok = true;
        conn.kind = RouteKind::tunnel;
        conn.note = "ssh tunnel: " + tunnel->describe();
        conn.transport = std::move(t);
        // The sink refers to this frame; from here on the tunnel's stderr is
        // passed through to ours unless the caller sets a sink of its own.
        tunnel->set_stderr_sink(nullptr);
        tunnel->set_cancel(nullptr);
        conn.tunnel = std::move(tunnel);
        obs.on_step_ready(index, RouteKind::tunnel);
        return conn;
    }

    conn.ok = false;
    conn.error = last_error.empty() ? "no route could be established" : last_error;
    return conn;
}

} // namespace haiku_remote
