// ssh.exe for the end-to-end harness on Windows (tests/e2e/profile_e2e.py).
//
// The client starts ssh with CreateProcess, which runs only an .exe for the
// bare name "ssh" (a #! script or a .cmd is never found), so the Python fake
// next to this file (tests/e2e/fake-ssh/ssh) cannot stand in by itself. This
// is the smallest faithful bridge: it runs
//
//     $FAKE_SSH_PYTHON $FAKE_SSH_SCRIPT <its own arguments, verbatim>
//
// with the standard handles it was given, waits, and exits with the fake's
// status -- so there is one fake ssh, and one set of expectations, on every
// OS. The arguments are copied from this process's own command line rather
// than re-quoted, so the fake parses exactly the bytes the client's
// quote_windows_arg() produced.
//
// It exports FAKE_SSH_PID (its own pid) for the fake to log: this is the
// process the client spawns and terminates, so it is the pid the harness
// watches. The fake runs in the same kill-on-close job the client put this
// process in (a child inherits its parent's job), so ending the job ends it.
//
// One thing the bridge changes on purpose: the client stops ssh with
// TerminateProcess(process, 1), and the harness reads exit status 1 as exactly
// that. A fake that itself failed with 1 (an uncaught Python exception) is
// reported as 70 instead, so it can never pass for a clean teardown.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace {

std::wstring env(const wchar_t* name)
{
    wchar_t buffer[32768];
    const DWORD n = ::GetEnvironmentVariableW(name, buffer, 32768);
    return n > 0 && n < 32768 ? std::wstring(buffer, n) : std::wstring();
}

// The command line after argv[0], by the rule CreateProcess's own parser
// (and the CRT's) uses for the program name: a quoted run, or up to the first
// space or tab.
const wchar_t* after_program(const wchar_t* line)
{
    if (*line == L'"') {
        ++line;
        while (*line != L'\0' && *line != L'"')
            ++line;
        if (*line == L'"')
            ++line;
    } else {
        while (*line != L'\0' && *line != L' ' && *line != L'\t')
            ++line;
    }
    return line; // keeps the separating whitespace
}

void complain(const char* text)
{
    DWORD wrote = 0;
    ::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), text,
                static_cast<DWORD>(std::char_traits<char>::length(text)), &wrote,
                nullptr);
}

} // namespace

int main()
{
    const std::wstring python = env(L"FAKE_SSH_PYTHON");
    const std::wstring script = env(L"FAKE_SSH_SCRIPT");
    if (python.empty() || script.empty()) {
        complain("fake ssh.exe: FAKE_SSH_PYTHON and FAKE_SSH_SCRIPT must be set\r\n");
        return 255;
    }
    ::SetEnvironmentVariableW(L"FAKE_SSH_PID",
                              std::to_wstring(::GetCurrentProcessId()).c_str());

    std::wstring line = L"\"" + python + L"\" \"" + script + L"\""
        + after_program(::GetCommandLineW());

    STARTUPINFOW si {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi {};
    if (!::CreateProcessW(python.c_str(), line.data(), nullptr, nullptr,
                          /*inherit=*/TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                          &si, &pi)) {
        complain("fake ssh.exe: cannot start the Python fake\r\n");
        return 255;
    }
    ::CloseHandle(pi.hThread);
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 255;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    return code == 1 ? 70 : static_cast<int>(code);
}
