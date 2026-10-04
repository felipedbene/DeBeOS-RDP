#include "haiku_remote/ssh_tunnel.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace haiku_remote {

namespace {

// Quote one argv element for a Windows command line per the CommandLineToArgvW
// rules (backslashes are only special before a quote).
std::wstring quote_arg(const std::wstring& arg)
{
    if (!arg.empty()
        && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        unsigned backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(*it);
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                        static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                        static_cast<int>(w.size()), nullptr, 0,
                                        nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          s.data(), n, nullptr, nullptr);
    return s;
}

std::string drain_handle(HANDLE h)
{
    std::string out;
    if (h == nullptr || h == INVALID_HANDLE_VALUE)
        return out;
    for (;;) {
        // PeekNamedPipe keeps the drain non-blocking: only read what is buffered.
        DWORD available = 0;
        if (!::PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr)
            || available == 0)
            break;
        char buffer[4096];
        DWORD want = available < sizeof(buffer) ? available : sizeof(buffer);
        DWORD got = 0;
        if (!::ReadFile(h, buffer, want, &got, nullptr) || got == 0)
            break;
        out.append(buffer, got);
    }
    return out;
}

class WindowsProcess final : public IProcess {
public:
    ~WindowsProcess() override { stop(); }

    bool start(const std::vector<std::string>& argv, std::string& error) override
    {
        if (argv.empty()) { error = "empty argv"; return false; }
        argv_ = argv;

        SECURITY_ATTRIBUTES sa {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE out_read = nullptr, out_write = nullptr;
        HANDLE err_read = nullptr, err_write = nullptr;
        if (!::CreatePipe(&out_read, &out_write, &sa, 0)
            || !::CreatePipe(&err_read, &err_write, &sa, 0)) {
            error = "CreatePipe failed";
            return false;
        }
        ::SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

        // Closed stdin so ssh can never block on a prompt.
        HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                   OPEN_EXISTING, 0, nullptr);

        std::wstring cmdline;
        for (std::size_t i = 0; i < argv_.size(); ++i) {
            if (i) cmdline.push_back(L' ');
            cmdline += quote_arg(widen(argv_[i]));
        }

        STARTUPINFOW si {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = out_write;
        si.hStdError = err_write;
        PROCESS_INFORMATION pi {};

        std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
        mutable_cmd.push_back(L'\0');

        // CREATE_SUSPENDED so the child is assigned to the kill-on-close Job
        // Object before it runs; CREATE_NO_WINDOW keeps ssh headless.
        const BOOL ok = ::CreateProcessW(
            nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

        ::CloseHandle(out_write);
        ::CloseHandle(err_write);
        if (nul != INVALID_HANDLE_VALUE)
            ::CloseHandle(nul);

        if (!ok) {
            error = "CreateProcess failed: " + std::to_string(::GetLastError());
            ::CloseHandle(out_read);
            ::CloseHandle(err_read);
            return false;
        }

        job_ = ::CreateJobObjectW(nullptr, nullptr);
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit {};
            limit.BasicLimitInformation.LimitFlags
                = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(job_, JobObjectExtendedLimitInformation,
                                      &limit, sizeof(limit));
            ::AssignProcessToJobObject(job_, pi.hProcess); // assign THEN resume
        }
        ::ResumeThread(pi.hThread);
        ::CloseHandle(pi.hThread);

        process_ = pi.hProcess;
        pid_ = pi.dwProcessId;
        stdout_ = out_read;
        stderr_ = err_read;
        return true;
    }

    long pid() const override { return static_cast<long>(pid_); }

    bool running() override
    {
        if (process_ == nullptr || exited_)
            return false;
        if (::WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            ::GetExitCodeProcess(process_, &code);
            exit_code_ = static_cast<int>(code);
            exited_ = true;
            return false;
        }
        return true;
    }

    std::optional<int> exit_code() override
    {
        running();
        if (!exited_)
            return std::nullopt;
        return exit_code_;
    }

    std::string drain_stdout() override { return drain_handle(stdout_); }
    std::string drain_stderr() override { return drain_handle(stderr_); }

    void stop() override
    {
        if (job_) {
            // Terminating the job kills the whole process tree; closing the job
            // handle would too (KILL_ON_JOB_CLOSE), but be explicit.
            ::TerminateJobObject(job_, 1);
            ::CloseHandle(job_);
            job_ = nullptr;
        }
        if (process_) {
            ::WaitForSingleObject(process_, 2000);
            ::CloseHandle(process_);
            process_ = nullptr;
            exited_ = true;
        }
        if (stdout_) { ::CloseHandle(stdout_); stdout_ = nullptr; }
        if (stderr_) { ::CloseHandle(stderr_); stderr_ = nullptr; }
        pid_ = 0;
    }

private:
    std::vector<std::string> argv_;
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    HANDLE stdout_ = nullptr;
    HANDLE stderr_ = nullptr;
    DWORD pid_ = 0;
    bool exited_ = false;
    int exit_code_ = 0;
};

long long creation_time_stamp(HANDLE process)
{
    FILETIME created {}, exited {}, kernel {}, user {};
    if (!::GetProcessTimes(process, &created, &exited, &kernel, &user))
        return 0;
    ULARGE_INTEGER v {};
    v.LowPart = created.dwLowDateTime;
    v.HighPart = created.dwHighDateTime;
    return static_cast<long long>(v.QuadPart);
}

// Best-effort read of another process's command line via its PEB. Any failure
// leaves cmdline empty, which the fail-closed reaper reads as unverifiable.
void read_peb_cmdline(HANDLE process, ProcessIdentity& id)
{
    using NtQIP = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return;
    auto query = reinterpret_cast<NtQIP>(
        reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtQueryInformationProcess")));
    if (!query)
        return;

    PROCESS_BASIC_INFORMATION pbi {};
    ULONG len = 0;
    if (query(process, ProcessBasicInformation, &pbi, sizeof(pbi), &len) != 0)
        return;
    if (!pbi.PebBaseAddress)
        return;

    PEB peb {};
    SIZE_T read = 0;
    if (!::ReadProcessMemory(process, pbi.PebBaseAddress, &peb, sizeof(peb), &read))
        return;
    RTL_USER_PROCESS_PARAMETERS params {};
    if (!::ReadProcessMemory(process, peb.ProcessParameters, &params,
                             sizeof(params), &read))
        return;
    const USHORT bytes = params.CommandLine.Length;
    if (bytes == 0 || !params.CommandLine.Buffer)
        return;
    std::wstring cmd(bytes / sizeof(wchar_t), L'\0');
    if (!::ReadProcessMemory(process, params.CommandLine.Buffer, cmd.data(),
                             bytes, &read))
        return;

    int argc = 0;
    LPWSTR* argvw = ::CommandLineToArgvW(cmd.c_str(), &argc);
    if (!argvw)
        return;
    for (int i = 0; i < argc; ++i)
        id.cmdline.push_back(narrow(argvw[i]));
    ::LocalFree(argvw);
}

} // namespace

ProcessFactory default_process_factory()
{
    return [] { return std::unique_ptr<IProcess>(new WindowsProcess()); };
}

void force_kill_process(long pid)
{
    if (pid <= 0)
        return;
    HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (h) {
        ::TerminateProcess(h, 1);
        ::CloseHandle(h);
    }
}

std::optional<ProcessIdentity> probe_process(long pid)
{
    if (pid <= 0)
        return std::nullopt;
    HANDLE h = ::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE,
        static_cast<DWORD>(pid));
    if (!h)
        return std::nullopt;

    ProcessIdentity id;
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    if (::QueryFullProcessImageNameW(h, 0, path, &size)) {
        std::wstring full(path, size);
        const auto slash = full.find_last_of(L"\\/");
        std::wstring name = (slash == std::wstring::npos) ? full
                                                          : full.substr(slash + 1);
        // Normalise "ssh.exe" to "ssh" so reap_decision's basename check matches.
        if (name.size() > 4 && name.substr(name.size() - 4) == L".exe")
            name = name.substr(0, name.size() - 4);
        id.exe = narrow(name);
    }
    id.start_time = creation_time_stamp(h);
    read_peb_cmdline(h, id);
    ::CloseHandle(h);
    return id;
}

} // namespace haiku_remote

#endif // _WIN32
