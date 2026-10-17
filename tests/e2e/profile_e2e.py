#!/usr/bin/env python3
"""End-to-end test of a saved connection profile (DeBeOS-RDP issue #1, Testing).

Issue #1 asks for an integration test against tools/rp_mock_server.py that
exercises a saved profile through connection progress, the protocol handshake,
desktop presentation, and disconnect back to the library. This drives the real
client binaries -- no test build of the client, no in-process shortcut:

  headless  build/haiku-remote --profile NAME --profiles FILE
            progress (its "connect: <stage>" lines), handshake, the desktop in
            the PNG it writes, and teardown. It has no library to return to.
  gui-profile  haiku-remote-gui --profile NAME: connects at once, the library
            never shown, the connection still recorded in the profile file.
  gui       haiku-remote-gui --profiles FILE under SDL's dummy video driver,
            steered by HAIKU_REMOTE_TEST_SCRIPT (sdl_main.cpp): click the
            profile's Connect button in the library, watch the progress
            stages, wait for exact desktop pixels, close the desktop, and see
            the library come back -- then close it.

Route-agnostic by construction: a case is a ROUTE (how the profile reaches the
mock, and what that must look like on the way) crossed with a FRONT END. A new
transport is one more routes() entry that states its own stages, route note
and expected ssh traffic.

QUIC. With --quic-bridge (tools/quic_bridge_mock.cpp, built exactly when the
client has quic://, i.e. OpenSSL >= 3.5), the QUIC routes run: the fake ssh's
broker fetch then answers token + certificate + "debeos-urp/1", and the bridge
-- a stand-in for the broker's QUIC front door on UDP 127.0.0.1:10902, the
fixed broker port -- authenticates the client and presents the session cookie
to the mock itself. The bridge's own log is asserted too (ALPN, auth, stream
0, close codes), and so is the known_brokers entry the fetched certificate
seeded. Whether the client was built with QUIC is taken from --quic-bridge
(the build system), never from the client's output: it decides what the
"auto" fallback must look like. UDP 10902 already in use is a loud SKIP of
the QUIC cases, never a pass.

Every expectation is stated independently of the client: stage labels, the
route note, pixel values (from the mock's own initial_scene), the ssh argv (a
hand-written golden list), the certificate fingerprint (computed here from the
PEM), and what the mock and the bridge themselves logged. Negative controls
run cases that cannot succeed and require the checks to FAIL -- a wrong session
cookie (no handshake at the mock), and with QUIC a wrong broker token (the
bridge must deny it) and a wrong ALPN (the bridge must refuse it) -- so this
harness is shown, on every run, to be able to fail.

Windows. One harness, per-OS helpers. The client runs only an .exe for "ssh"
there, so --fake-ssh names tests/e2e/fake-ssh/ssh_win.cpp's ssh.exe (CMake
builds it), which runs the same Python fake with its arguments verbatim. The
client stops ssh with TerminateProcess -- nothing the target can log -- so
SshWatcher holds a handle to every ssh.exe the log names and records that end
itself, at the process's own exit time; "tunnel-exit:TerminateProcess" is then
the Windows spelling of "tunnel-exit:SIGTERM". Liveness is OpenProcess, and
every case also counts the fake ssh.exe images left running with a Toolhelp
snapshot. Windows has no SIGTERM to send, so the quit cases are the console
controls instead: Ctrl+C, Ctrl+Break and the console closed, each delivered by
a helper attached to the GUI's own console.

Exit status: 0 all cases passed (or a front end / the QUIC routes were skipped
and said so), 1 a case failed, 2 bad invocation.
"""

import argparse
import hashlib
import json
import re
import os
import secrets
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
FAKE_SSH_SCRIPT = HERE / "fake-ssh" / "ssh"
# Where the fake ssh is on PATH: the script's own directory on POSIX, the
# built ssh.exe's on Windows (--fake-ssh, set in main()).
FAKE_SSH_DIR = HERE / "fake-ssh"
MOCK = REPO / "tools" / "rp_mock_server.py"

WINDOWS = sys.platform == "win32"
# How the client stops a forward, as the ssh log records it: SIGTERM on POSIX
# (logged by the fake), TerminateProcess on Windows (logged by SshWatcher).
TUNNEL_STOP = "TerminateProcess" if WINDOWS else "SIGTERM"
# The ways to ask the GUI to quit while its desktop is up: signals on POSIX,
# the console controls on Windows (which has no SIGTERM to send).
QUIT_REQUESTS = (["CTRL_C_EVENT", "CTRL_BREAK_EVENT", "CTRL_CLOSE_EVENT"]
                 if WINDOWS else ["SIGTERM", "SIGINT"])

WIDTH, HEIGHT = 800, 600

# What the mock's initial_scene draws (tools/rp_mock_server.py), read off its
# source rather than off a capture: the desktop background fill
# color(51, 102, 152), the window's yellow tab color(255, 203, 0) at
# (60, 38)-(210, 59), and its grey body color(216, 216, 216) at (60, 60)-(480,
# 320). The three points are asymmetric, so a mirrored or offset blit moves at
# least one of them onto a different colour.
PIXELS = [
    (5, 5, "336698"),     # background, top-left corner
    (200, 42, "ffcb00"),  # tab, right of the title text
    (470, 310, "d8d8d8"), # body, bottom-right inside the border
]

# The hardened ssh option prefix (issue #1, "Required SSH options"), written by
# hand. Every invocation that reaches the fake ssh must start with exactly this.
SSH_PREFIX = [
    "ssh", "-T",
    "-o", "BatchMode=yes",
    "-o", "StrictHostKeyChecking=accept-new",
    "-o", "ConnectTimeout=15",
    "-o", "ServerAliveInterval=15",
    "-o", "ServerAliveCountMax=3",
    "-o", "ControlMaster=no",
    "-o", "ControlPath=none",
    "-p", "22",
]


@dataclass
class Route:
    """One way a saved profile reaches the mock app_server."""
    name: str
    mode: str                    # the profile's "mode"
    # The progress stages the user is shown, in order, by their labels
    # (connect_flow.cpp stage_label()).
    stages: list
    # The kinds of ssh invocation, in order: "broker", "cookie", "tunnel".
    ssh_calls: list
    # The client's route note must start with this (format: {port} = the
    # mock's port).
    route_note: str
    # Give the profile the mock's cookie file as cookieSource (no SSH fetch).
    cookie_from_file: bool = False
    # The connections.json schema version the profile is written with.
    schema_version: int = 2
    # Extra mock arguments, and whether the mock serves exactly one session.
    mock_args: list = field(default_factory=list)
    # The guest runs a broker with QUIC (the fake ssh's fetch advertises
    # debeos-urp/1) and the session goes through the QUIC bridge. Such a
    # route runs only with --quic-bridge. route_note is then the whole note,
    # exactly, with {fp} = the bridge certificate's SHA256:<hex>.
    quic: bool = False
    # The stages instead, when the client was built with QUIC (its auto plan
    # then starts with a QUIC step: profile_launch.cpp plan_launch()).
    stages_quic_build: list = None


# The stage labels (connect_flow.cpp label_for()), written out.
TUNNEL_STAGES = ["Fetching session cookie", "Starting SSH tunnel",
                 "Waiting for the SSH forward", "Connecting to the session port",
                 "Waiting for the server handshake", "Requesting the display"]
# A broker step that fails at its credential fetch: this guest has no
# remote_broker, so the fake ssh answers the fetch with the marker alone.
BROKER_FAILED_STAGES = ["Contacting broker"]

# The tunnel's stages after the cookie fetch.
TUNNEL = TUNNEL_STAGES[1:]
# A broker session over QUIC: stage_label() + route_flavour()'s " (QUIC)"
# (connect_flow.cpp), and no session-port or SSH stages -- the broker dials
# app_server itself.
QUIC_STAGES = ["Contacting broker (QUIC)", "Authenticating with broker (QUIC)",
               "Waiting for the server handshake (QUIC)",
               "Requesting the display (QUIC)"]
# How many RP_ messages the mock's initial_scene holds at WIDTH x HEIGHT, as
# the mock itself counts and prints them ("-> initial scene, B bytes, N
# messages"); every case checks the print against this, so a scene change is
# noticed here rather than silently moving the reconnect check below.
SCENE_MESSAGES = 38

QUIC_PORT = 10902   # default_broker_port (profile_launch.hpp), over UDP
QUIC_NOTE = ("broker (quic): quic://127.0.0.1:10902 with a fetched token and"
             " certificate {fp} (fetched over SSH)")


def routes(build_has_wss, build_has_quic):
    """The routes, for a client built with or without wss, and with or
    without QUIC (OpenSSL >= 3.5; known from --quic-bridge, the build).

    Without wss, `auto` plans no broker step at all (profile_launch.cpp: a
    route the build cannot honour is not a route), so it is the tunnel alone.
    With QUIC it reaches the broker over QUIC first, then over wss (#82).
    """
    # A guest with no remote_broker: the credential fetch finds no token, and
    # every broker step fails on that one fetch (one fetch serves both).
    auto_broker = []
    if build_has_wss:
        auto_broker = (["Contacting broker (QUIC)"] if build_has_quic else []) \
            + BROKER_FAILED_STAGES
    no_broker_calls = (["broker"] if build_has_wss else []) + ["cookie", "tunnel"]
    return [
        # Issue #1's direct TCP: client -> 127.0.0.1:<mock>, no ssh at all,
        # the cookie from cookieSource. Nothing to start, nothing to forward.
        Route(name="direct", mode="direct",
              stages=["Connecting to the session port",
                      "Waiting for the server handshake", "Requesting the display"],
              ssh_calls=[],
              route_note="direct tcp: client -> 127.0.0.1:",
              cookie_from_file=True),
        # auto on a guest whose broker advertises QUIC: the first step carries
        # the session -- one ssh call, the credential fetch, nothing after it.
        Route(name="auto", mode="auto", stages=QUIC_STAGES,
              ssh_calls=["broker"], route_note=QUIC_NOTE, quic=True),
        # auto on a guest with no broker: every broker step fails on the
        # fetch, and the tunnel carries the session.
        Route(name="auto-no-broker", mode="auto",
              stages=auto_broker + TUNNEL_STAGES,
              ssh_calls=no_broker_calls,
              route_note="ssh tunnel: 127.0.0.1:"),
        # A schema v1 file's "direct" is v2's auto (profile_store.cpp): the
        # same profile, saved by a v1 client, must take the auto route.
        Route(name="v1-direct", mode="direct", schema_version=1,
              stages=auto_broker + TUNNEL_STAGES,
              ssh_calls=no_broker_calls,
              route_note="ssh tunnel: 127.0.0.1:"),
        # Forced QUIC: the QUIC step alone, no fallback.
        Route(name="quic", mode="quic", stages=QUIC_STAGES,
              ssh_calls=["broker"], route_note=QUIC_NOTE, quic=True),
        Route(name="ssh", mode="ssh",
              stages=TUNNEL_STAGES,
              ssh_calls=["cookie", "tunnel"],
              route_note="ssh tunnel: 127.0.0.1:"),
        Route(name="ssh-cookie-file", mode="ssh",
              stages=TUNNEL_STAGES[1:],
              ssh_calls=["tunnel"],
              route_note="ssh tunnel: 127.0.0.1:",
              cookie_from_file=True),
    ]


def client_has_wss(client):
    """Ask the client binary itself whether it was built with wss."""
    probe = subprocess.run([str(client), "--url", "wss://127.0.0.1:9/", "--token",
                            "x", "--seconds", "1"], capture_output=True, text=True,
                           timeout=30)
    return "no WebSocket support" not in probe.stdout + probe.stderr


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def udp_port_busy(port):
    """Why UDP 127.0.0.1:port cannot be bound, or None when it can."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.bind(("127.0.0.1", port))
        except OSError as e:
            return e.strerror or str(e)
    return None


class Quic:
    """What the QUIC cases share: the bridge binary, a throwaway certificate
    and its fingerprint, computed here from the PEM rather than by the
    client."""

    def __init__(self, bridge, root):
        self.bridge = Path(bridge)
        self.cert, self.key = root / "broker.pem", root / "broker.key"
        made = subprocess.run([str(self.bridge), "--make-cert", str(self.cert),
                               str(self.key)], capture_output=True, text=True)
        if made.returncode != 0:
            raise RuntimeError(f"quic_bridge_mock --make-cert failed: {made.stderr}")
        der = ssl.PEM_cert_to_DER_cert(self.cert.read_text())
        self.digest = hashlib.sha256(der).hexdigest()
        self.fingerprint = "SHA256:" + self.digest
        self.token = secrets.token_hex(24)

    def start(self, work, backend_port, cookie_file):
        """Starts a bridge for one case. Returns (process, log path), or
        (None, reason) when UDP 10902 is taken -- a SKIP, said loudly."""
        busy = udp_port_busy(QUIC_PORT)
        if busy:
            return None, f"UDP 127.0.0.1:{QUIC_PORT} is busy ({busy})"
        log_path = work / "bridge.log"
        with open(log_path, "w") as log:
            proc = subprocess.Popen(
                [str(self.bridge), "--listen", str(QUIC_PORT),
                 "--backend", f"127.0.0.1:{backend_port}",
                 "--token", self.token, "--cert", str(self.cert),
                 "--key", str(self.key), "--cookie-file", str(cookie_file)],
                stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                if proc.returncode == 3:  # its bind failed: taken meanwhile
                    return None, (f"UDP 127.0.0.1:{QUIC_PORT} is busy"
                                  f" ({log_path.read_text().strip()})")
                raise RuntimeError("the QUIC bridge exited at startup: "
                                   + log_path.read_text())
            if "listening on quic://" in log_path.read_text():
                return proc, log_path
            time.sleep(0.05)
        proc.kill()
        raise RuntimeError("the QUIC bridge never came up")


def stop(proc):
    if proc is not None and proc.poll() is None:
        proc.kill()
        proc.wait()


def read_png_rgb(path):
    """Decode an 8-bit RGB/RGBA non-interlaced PNG (what png_writer emits)."""
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    width, height, depth, ctype, _, _, interlace = ihdr
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise ValueError(f"unsupported PNG (depth {depth}, type {ctype})")
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(height):
        ftype, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ftype == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ftype == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ftype == 3:
                line[x] = (line[x] + (a + b) // 2) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 0xFF
        rows.append(bytes(line))
        prev = line

    def pixel(x, y):
        o = x * bpp
        return "%02x%02x%02x" % tuple(rows[y][o:o + 3])
    return width, height, pixel


def classify_ssh(argv):
    if "-N" in argv:
        return "tunnel"
    command = argv[-1]
    if "remote_broker" in command:
        return "broker"
    if "session_cookie." in command:
        return "cookie"
    return "other"


if WINDOWS:
    import ctypes
    from ctypes import wintypes

    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _k32.OpenProcess.restype = wintypes.HANDLE
    _k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    _k32.WaitForSingleObject.restype = wintypes.DWORD
    _k32.CloseHandle.argtypes = [wintypes.HANDLE]
    _k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE,
                                        ctypes.POINTER(wintypes.DWORD)]
    _k32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [
        ctypes.POINTER(wintypes.FILETIME)] * 4
    _k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    _k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    _k32.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD)]
    SYNCHRONIZE = 0x00100000
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    WAIT_TIMEOUT = 0x102

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t),
                    ("th32ModuleID", wintypes.DWORD),
                    ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD),
                    ("pcPriClassBase", ctypes.c_long),
                    ("dwFlags", wintypes.DWORD),
                    ("szExeFile", ctypes.c_wchar * 260)]

    _k32.Process32FirstW.argtypes = [wintypes.HANDLE,
                                     ctypes.POINTER(PROCESSENTRY32W)]
    _k32.Process32NextW.argtypes = [wintypes.HANDLE,
                                    ctypes.POINTER(PROCESSENTRY32W)]


def pid_alive(pid):
    if WINDOWS:
        handle = _k32.OpenProcess(SYNCHRONIZE, False, pid)
        if not handle:
            return False  # no such process any more
        try:
            return _k32.WaitForSingleObject(handle, 0) == WAIT_TIMEOUT
        finally:
            _k32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    # A zombie still answers kill(0); read its state where /proc exists.
    stat = Path(f"/proc/{pid}/stat")
    if stat.exists():
        try:
            return stat.read_text().split(") ")[-1].split()[0] != "Z"
        except OSError:
            return False
    return True


def live_fake_ssh():
    """The pids of fake ssh.exe processes still running, by a Toolhelp
    snapshot of every process whose image is FAKE_SSH_DIR's ssh.exe -- the
    count the client must bring to zero, independent of the log. Windows
    only; [] elsewhere (the per-pid checks cover POSIX)."""
    if not WINDOWS:
        return []
    want = os.path.normcase(str(FAKE_SSH_DIR / "ssh.exe"))
    snap = _k32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    found = []
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    more = _k32.Process32FirstW(snap, ctypes.byref(entry))
    while more:
        if entry.szExeFile.lower() == "ssh.exe":
            handle = _k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                      False, entry.th32ProcessID)
            if handle:
                buf = ctypes.create_unicode_buffer(1024)
                size = wintypes.DWORD(1024)
                if (_k32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size))
                        and os.path.normcase(buf.value) == want
                        and _k32.WaitForSingleObject(handle, 0) == WAIT_TIMEOUT):
                    found.append(entry.th32ProcessID)
                _k32.CloseHandle(handle)
        more = _k32.Process32NextW(snap, ctypes.byref(entry))
    _k32.CloseHandle(snap)
    return found


class SshWatcher:
    """Windows: records how each fake ssh.exe ended when the client ended it.

    The client stops ssh with TerminateProcess(process, 1) (managed_transport.cpp
    ManagedProcess::terminate()), which runs no code in the target, so the fake
    cannot log it as it logs a signal on POSIX. This holds a handle to every
    ssh.exe the log names, from the moment its argv line appears, and appends
    {"exit": "TerminateProcess"} for one that ended with status 1 -- which
    ssh_win.cpp never returns for any other reason. The entry's "t" is the
    process's own exit time (GetProcessTimes), not when this noticed it, so
    the timeline orders it exactly against the next ssh's start. A no-op on
    POSIX."""

    def __init__(self, ssh_log):
        self.ssh_log = ssh_log
        self.handles = {}
        self.done = set()
        self.stopping = False
        self.thread = None
        if WINDOWS:
            import threading
            self.thread = threading.Thread(target=self._run, daemon=True)
            self.thread.start()

    def _scan(self):
        if self.ssh_log.exists():
            for line in self.ssh_log.read_text().splitlines():
                try:
                    entry = json.loads(line)
                except ValueError:
                    continue  # a line being written
                pid = entry.get("pid")
                if "argv" in entry and pid not in self.handles:
                    self.handles[pid] = _k32.OpenProcess(
                        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        for pid, handle in self.handles.items():
            if pid in self.done or not handle:
                continue
            if _k32.WaitForSingleObject(handle, 0) == WAIT_TIMEOUT:
                continue
            self.done.add(pid)
            code = wintypes.DWORD()
            _k32.GetExitCodeProcess(handle, ctypes.byref(code))
            if code.value != 1:
                continue  # it ended by itself (and logged so, if it does)
            times = [wintypes.FILETIME() for _ in range(4)]
            _k32.GetProcessTimes(handle, *[ctypes.byref(t) for t in times])
            ticks = (times[1].dwHighDateTime << 32) | times[1].dwLowDateTime
            when = ticks / 1e7 - 11644473600  # FILETIME -> Unix seconds
            with open(self.ssh_log, "a", encoding="utf-8") as f:
                f.write(json.dumps({"exit": "TerminateProcess", "pid": pid,
                                    "t": when}) + "\n")

    def _run(self):
        while not self.stopping:
            self._scan()
            time.sleep(0.01)

    def stop(self):
        """After the client has exited: give each watched ssh.exe a moment to
        end, record it, and release the handles."""
        if self.thread is None:
            return
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and any(
                pid not in self.done for pid, h in list(self.handles.items()) if h):
            time.sleep(0.02)
        self.stopping = True
        self.thread.join()
        self._scan()
        for handle in self.handles.values():
            if handle:
                _k32.CloseHandle(handle)


def read_ssh_log(ssh_log):
    """The ssh log's entries in the order they happened. On POSIX that is the
    file's order (one writer per event); on Windows two processes write it --
    the fake and SshWatcher -- so it is sorted by each entry's own time."""
    entries = []
    if ssh_log.exists():
        entries = [json.loads(l) for l in ssh_log.read_text().splitlines()]
    if WINDOWS:
        entries.sort(key=lambda e: e.get("t", 0))
    return entries


def ssh_pids(call):
    """Every process an ssh invocation was: the ssh the client ran and, on
    Windows, the Python fake behind its ssh.exe."""
    return sorted({call["pid"], call.get("impl", call["pid"])})


def fake_ssh_env(env, ssh_log):
    """The fake ssh first on PATH, logging to ssh_log."""
    env["PATH"] = str(FAKE_SSH_DIR) + os.pathsep + env.get("PATH", "")
    env["FAKE_SSH_LOG"] = str(ssh_log)
    env["FAKE_SSH_PYTHON"] = sys.executable
    env["FAKE_SSH_SCRIPT"] = str(FAKE_SSH_SCRIPT)


def config_dir(work):
    """Where the client keeps known_brokers, with the environment a case runs
    it under (ProfileStore::config_dir_for)."""
    if WINDOWS:
        return work / "config" / "Haiku Remote"   # %APPDATA%\Haiku Remote
    if sys.platform == "darwin":
        return work / "Library" / "Application Support" / "Haiku Remote"
    return work / "config" / "haiku-remote"       # $XDG_CONFIG_HOME/haiku-remote


DETACHED_PROCESS = 0x00000008
CREATE_NEW_CONSOLE = 0x00000010

# Delivers a console control to the console a process owns: attaches to it
# (FreeConsole first -- a process has one console), and raises the event for
# every process on it, itself shielded by a handler that swallows it -- or,
# for the close, posts WM_CLOSE to the console's window (what the user's [X]
# does) and leaves the console before conhost broadcasts the close.
CONSOLE_CONTROL_HELPER = r"""
import ctypes, sys
from ctypes import wintypes
k = ctypes.WinDLL("kernel32", use_last_error=True)
u = ctypes.WinDLL("user32", use_last_error=True)
pid, what = int(sys.argv[1]), sys.argv[2]
k.FreeConsole()
if not k.AttachConsole(pid):
    sys.exit("AttachConsole(%d) failed: %d" % (pid, ctypes.get_last_error()))
if what == "CTRL_CLOSE_EVENT":
    k.GetConsoleWindow.restype = ctypes.c_void_p
    hwnd = k.GetConsoleWindow()
    ok = hwnd and u.PostMessageW(ctypes.c_void_p(hwnd), 0x0010, 0, 0)
    error = ctypes.get_last_error()
    k.FreeConsole()
else:
    swallow = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.DWORD)(lambda event: True)
    k.SetConsoleCtrlHandler(swallow, True)
    ok = k.GenerateConsoleCtrlEvent({"CTRL_C_EVENT": 0, "CTRL_BREAK_EVENT": 1}[what], 0)
    error = ctypes.get_last_error()
    k.FreeConsole()
sys.exit(0 if ok else "%s failed: %d" % (what, error))
"""


def run_client(cmd, env, work, timeout=90):
    """Run the client to its exit with stdout and stderr in files (work/
    client.stdout, client.stderr), not pipes. A pipe's reader waits for every
    holder of the write end to close it, so an ssh the client failed to stop
    -- which inherits the client's standard handles -- would hang the harness
    past any timeout (on Windows subprocess.run's own timeout path waits for
    that EOF too) instead of failing the case. Raises TimeoutExpired."""
    out_path, err_path = work / "client.stdout", work / "client.stderr"
    with open(out_path, "w") as out, open(err_path, "w") as err:
        proc = subprocess.Popen(cmd, env=env, stdout=out, stderr=err, text=True)
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        raise
    return subprocess.CompletedProcess(cmd, proc.returncode, out_path.read_text(),
                                       err_path.read_text())


def request_quit(proc, what):
    """Ask a running GUI to quit: a signal on POSIX, a console control on
    Windows. Returns why it could not be sent, or None."""
    if not WINDOWS:
        proc.send_signal(getattr(signal, what))
        return None
    sent = subprocess.run([sys.executable, "-c", CONSOLE_CONTROL_HELPER,
                           str(proc.pid), what], capture_output=True, text=True,
                          timeout=30, creationflags=DETACHED_PROCESS)
    return None if sent.returncode == 0 else (sent.stderr.strip() or "failed")


class Case:
    def __init__(self, route, front_end, binary, work, wrong_cookie=False,
                 reconnect=False, quic=None, client_has_quic=False,
                 wrong_token=False, cancel_reconnect=False, quit_signal=None):
        self.route, self.front_end, self.binary = route, front_end, binary
        # The app asked to quit (a QUIT_REQUESTS name: a signal, or on Windows
        # a console control) while its desktop is up: it must exit, tearing
        # the session and tunnel down, and never show the library again --
        # unlike closing the window (issue #1).
        self.quit_signal = quit_signal
        # The GUI reconnect, cancelled: the session drops, the reconnect's
        # ssh then hangs (FAKE_SSH_STALL_FROM), and the window is closed while
        # it does. The desktop must close at once -- the attempt runs on a
        # worker -- and leave no ssh behind.
        self.cancel_reconnect = cancel_reconnect
        self.elapsed = None
        self.work = work
        self.wrong_cookie = wrong_cookie
        # The QUIC bridge (a Quic), for a route with quic=True; whether the
        # client was built with QUIC (from --quic-bridge, i.e. the build).
        self.quic, self.client_has_quic = quic, client_has_quic
        # The QUIC negative control: the guest hands the client a token the
        # broker does not hold, so the bridge must deny it.
        self.wrong_token = wrong_token
        self.skipped = None
        self.bridge_text = ""
        # The reconnect scenario: the mock drops each session (a clean FIN,
        # the shape of a dead tunnel) after 5 update frames, and the profile's
        # own Auto reconnect -- no --reconnect flag -- must bring it back once
        # (--reconnect-max-attempts 1), over a rebuilt tunnel.
        self.reconnect = reconnect or cancel_reconnect
        self.sessions = 2 if reconnect else 1
        self.failures = []

    def check(self, ok, what):
        if not ok:
            self.failures.append(what)

    def run(self):
        work, route = self.work, self.route
        cookie = work / "session_cookie"
        mock_log = work / "mock.log"
        ssh_log = work / "ssh.log"
        profiles = work / "connections.json"
        name = f"E2E {route.name}"

        mock_cmd = [sys.executable, "-u", str(MOCK), "--host", "127.0.0.1",
                    "--port", "0", "--cookie-file", str(cookie)]
        mock_cmd += ["--drop-after", "5"] if self.reconnect else ["--once"]
        mock_cmd += route.mock_args
        with open(mock_log, "w") as log:
            mock = subprocess.Popen(mock_cmd, stdout=log, stderr=subprocess.STDOUT)
        bridge = None
        try:
            # The mock binds port 0 and says which port it got: no
            # pick-a-free-port-then-bind race between cases.
            port = None
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and mock.poll() is None:
                listening = re.search(r"listening on 127\.0\.0\.1:(\d+)",
                                      mock_log.read_text())
                if cookie.exists() and listening:
                    port = int(listening.group(1))
                    break
                time.sleep(0.05)
            if port is None:
                self.failures.append("the mock never came up")
                return

            # The guest's published cookie. The negative control serves a
            # different one, so the gate must refuse and no handshake happens.
            served = cookie
            if self.wrong_cookie:
                served = work / "wrong_cookie"
                served.write_text("0" * 32 + "\n")

            profile = {
                "id": f"e2e-{route.name}", "name": name, "favorite": False,
                "mode": route.mode, "host": "127.0.0.1", "remotePort": port,
                "sshUser": "baron", "sshPort": 22, "identityFile": "",
                "localPort": None, "width": WIDTH, "height": HEIGHT,
                "autoReconnect": self.reconnect, "knownHostsFile": "",
                "cookieSource": str(served) if route.cookie_from_file else "",
                "lastConnectedAt": 0, "lastError": "",
            }
            profiles.write_text(json.dumps({"version": route.schema_version,
                                            "profiles": [profile]}))

            if route.quic:
                # The broker reads the guest's published cookie itself.
                bridge, why = self.quic.start(work, port, served)
                if bridge is None:
                    self.skipped = why
                    return

            env = dict(os.environ)
            fake_ssh_env(env, ssh_log)
            env["FAKE_SSH_COOKIE_FILE"] = str(served)
            env["FAKE_SSH_REMOTE_PORT"] = str(port)
            if self.cancel_reconnect:
                # The first connect is the cookie fetch and the tunnel; the
                # reconnect's first ssh (whatever the route) hangs.
                env["FAKE_SSH_STALL_FROM"] = str(len(route.ssh_calls) + 1)
            if route.quic:
                env["FAKE_SSH_BROKER_TOKEN"] = (
                    "f" * 48 if self.wrong_token else self.quic.token)
                env["FAKE_SSH_BROKER_PEM"] = str(self.quic.cert)
                env["FAKE_SSH_BROKER_QUIC"] = "debeos-urp/1"
            # Keep the user's real stores out of it.
            env["HOME"] = str(work)
            env["XDG_CONFIG_HOME"] = str(work / "config")
            env["XDG_RUNTIME_DIR"] = str(work / "run")
            env["APPDATA"] = str(work / "config")
            env["LOCALAPPDATA"] = str(work / "local")
            started = int(time.time())

            if self.front_end == "headless":
                png = work / "desktop.png"
                cmd = [str(self.binary), "--profile", name, "--profiles",
                       str(profiles), "--output", str(png)]
                cmd += (["--seconds", "5", "--reconnect-max-attempts", "1"]
                        if self.reconnect else ["--seconds", "2"])
            else:
                probes = ",".join(f"{x},{y},{rgb}" for x, y, rgb in PIXELS)
                # dummy unless the caller chose (offscreen reads back too).
                env["SDL_VIDEODRIVER"] = os.environ.get("SDL_VIDEODRIVER") or "dummy"
                env["SDL_AUDIODRIVER"] = "dummy"
                env["HAIKU_REMOTE_SOFTWARE_RENDERER"] = "1"
                if self.front_end == "gui" and self.quit_signal:
                    # The library:close step must never be reached.
                    env["HAIKU_REMOTE_TEST_SCRIPT"] = (
                        f"library:connect={name};session:hold-when={probes};"
                        "library:close")
                    cmd = [str(self.binary), "--profiles", str(profiles)]
                elif self.front_end == "gui" and self.cancel_reconnect:
                    env["HAIKU_REMOTE_TEST_SCRIPT"] = (
                        f"library:connect={name};reconnect:close;library:close")
                    cmd = [str(self.binary), "--profiles", str(profiles)]
                elif self.front_end == "gui" and self.reconnect:
                    # The desktop must come back after the drop: the pixel
                    # check runs on the reconnected session's frames.
                    env["HAIKU_REMOTE_TEST_SCRIPT"] = (
                        f"library:connect={name};reconnect:resume;"
                        f"session:close-when=min={SCENE_MESSAGES},{probes};"
                        "library:close")
                    cmd = [str(self.binary), "--profiles", str(profiles)]
                elif self.front_end == "gui":
                    env["HAIKU_REMOTE_TEST_SCRIPT"] = (
                        f"library:connect={name};session:close-when={probes};"
                        "library:close")
                    cmd = [str(self.binary), "--profiles", str(profiles)]
                else:  # gui-profile: --profile connects at once, no library
                    env["HAIKU_REMOTE_TEST_SCRIPT"] = f"session:close-when={probes}"
                    cmd = [str(self.binary), "--profile", name, "--profiles",
                           str(profiles)]
            watcher = SshWatcher(ssh_log)
            try:
                began = time.monotonic()
                if self.quit_signal:
                    client = self.run_and_signal(cmd, env)
                else:
                    client = run_client(cmd, env, self.work)
                self.elapsed = time.monotonic() - began
                watcher.stop()
            except subprocess.TimeoutExpired:
                watcher.stop()
                self.failures.append("the client did not exit within 90 s")
                return
            self.stdout, self.stderr = client.stdout, client.stderr
            if self.wrong_token:
                time.sleep(0.5)  # no session can reach the mock at all
            elif not self.reconnect:
                try:
                    mock.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    self.failures.append("the mock did not finish its one session")
            else:
                time.sleep(0.5)  # let the mock log the last session's end
            if bridge is not None:
                # The bridge logs the close after the client has gone.
                log_path = work / "bridge.log"
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    text = log_path.read_text()
                    if ("session ended" in text or "error 1" in text
                            and "closed with" in text):
                        break
                    time.sleep(0.05)
                time.sleep(0.2)
                stop(bridge)
                self.bridge_text = log_path.read_text()
            self.mock_text = mock_log.read_text()
            if self.cancel_reconnect:
                self.verify_cancelled_reconnect(client, ssh_log)
            elif self.quit_signal:
                self.verify_signal_quit(client, mock, ssh_log)
            else:
                self.verify(client, mock, ssh_log, profiles, port, started)
        finally:
            if mock.poll() is None:
                mock.kill()
                mock.wait()
            stop(bridge)

    def run_and_signal(self, cmd, env):
        """Run the GUI until it holds its desktop open, then ask it to quit."""
        out_path, err_path = self.work / "client.out", self.work / "client.err"
        # On Windows its own console, so a control sent there reaches only it;
        # and Ctrl+C processing on, which a process inherits from its parent
        # -- here the SSM agent's shell had it off, and the GUI rightly
        # leaves an inherited "ignore" alone, as SDL leaves an inherited
        # SIG_IGN alone on POSIX.
        flags = CREATE_NEW_CONSOLE if WINDOWS else 0
        if WINDOWS:
            _k32.SetConsoleCtrlHandler(None, False)
        with open(out_path, "w") as out, open(err_path, "w") as err:
            proc = subprocess.Popen(cmd, env=env, stdout=out, stderr=err, text=True,
                                    creationflags=flags)
        held = False
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and proc.poll() is None:
            if "e2e: holding the desktop open" in err_path.read_text():
                held = True
                break
            time.sleep(0.05)
        self.check(held, "the desktop was never presented and held open")
        if held:
            why = request_quit(proc, self.quit_signal)
            self.check(why is None, f"{self.quit_signal} could not be sent: {why}")
        try:
            proc.wait(timeout=30 if held else 1)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            self.failures.append(
                f"the client did not exit within 30 s of {self.quit_signal}")
        return subprocess.CompletedProcess(cmd, proc.returncode,
                                           out_path.read_text(),
                                           err_path.read_text())

    def verify_signal_quit(self, client, mock, ssh_log):
        route, err = self.route, self.stderr
        lines = err.splitlines()
        name = self.quit_signal
        self.check(client.returncode == 0,
                   f"client exit status {client.returncode} after {name},"
                   " expected 0 (a clean quit)")
        self.check(mock.returncode == 0,
                   f"mock exit status {mock.returncode}, expected 0")
        want = "".join(f" ({x},{y})={rgb}" for x, y, rgb in PIXELS)
        desktop = [i for i, l in enumerate(lines)
                   if l.startswith("e2e: desktop presented after ")]
        self.check(len(desktop) == 1 and lines[desktop[0]].endswith(":" + want),
                   f"desktop report {[lines[i] for i in desktop]}, expected one"
                   f" ending ':{want}'")
        quits = [l for l in lines if l.startswith("quit requested (")]
        self.check(quits == ["quit requested (SIGTERM, SIGINT or the system):"
                             " ending the session and exiting"],
                   f"quit lines {quits}, expected exactly one")
        # Never back to the library: shown once, before the desktop, and the
        # script's library:close step never ran.
        shown = [i for i, l in enumerate(lines)
                 if l == "e2e: library shown with 1 profiles"]
        self.check(len(shown) == 1 and desktop and shown[0] < desktop[0],
                   f"the library was shown at lines {shown}, expected once,"
                   f" before the desktop -- {name} returned to the library")
        self.check("e2e: closing the library" not in lines,
                   f"the library came back after {name} and was closed by the"
                   " script")
        # The session and its tunnel were torn down, the tunnel stopped by
        # the client (SIGTERM; TerminateProcess on Windows).
        entries = read_ssh_log(ssh_log)
        calls = [e for e in entries if "argv" in e]
        kind_of = {c["pid"]: classify_ssh(c["argv"]) for c in calls}
        timeline = [classify_ssh(e["argv"]) if "argv" in e
                    else f"{kind_of.get(e['pid'], 'unknown')}-exit:{e['exit']}"
                    for e in entries if "argv" in e or "exit" in e]
        one = route.ssh_calls + [f"tunnel-exit:{TUNNEL_STOP}"]
        self.check(timeline == one, f"ssh timeline {timeline}, expected {one}")
        self.check_no_ssh_left(calls, f"after {name}")

    def verify_cancelled_reconnect(self, client, ssh_log):
        err = self.stderr
        lines = err.splitlines()
        self.check(client.returncode == 0,
                   f"client exit status {client.returncode}, expected 0")
        # The stalled ssh never answers, so only a reconnect that runs off the
        # UI thread lets the close through. The old blocking reconnect waited
        # out the ssh (here: forever, until the 90 s harness timeout).
        self.check(self.elapsed is not None and self.elapsed < 15,
                   f"the GUI took {self.elapsed} s to close during a stalled"
                   " reconnect, expected under 15 s")
        stages = (self.route.stages_quic_build
                  if self.client_has_quic and self.route.stages_quic_build
                  else self.route.stages)
        want = (f"e2e: reconnect attempt running ({stages[0]});"
                " closing the window")
        self.check(lines.count(want) == 1,
                   f"no '{want}': the window was not closed while the"
                   " reconnect's ssh hung")
        self.check(lines.count("e2e: library shown with 1 profiles") == 2
                   and "e2e: closing the library" in lines,
                   "the GUI did not go back to the library and close it")
        count = self.mock_text.count("<- RP_INIT_CONNECTION")
        self.check(count == 1, f"the mock saw {count} sessions, expected 1 (the"
                               " reconnect never reached it)")
        entries = read_ssh_log(ssh_log)
        calls = [e for e in entries if "argv" in e]
        kinds = [classify_ssh(c["argv"]) for c in calls]
        first = self.route.ssh_calls
        want = first + first[:1]
        self.check(kinds == want, f"ssh calls {kinds}, expected {want} (the"
                                  " first connect, then the reconnect's first"
                                  " ssh, which hangs)")
        self.check_no_ssh_left(calls, "after the client exited")

    def check_no_ssh_left(self, calls, when):
        """Every process each ssh invocation was is gone -- by pid, and on
        Windows also by a Toolhelp count of the fake ssh.exe images."""
        for c in calls:
            for pid in ssh_pids(c):
                self.check(not pid_alive(pid),
                           f"ssh child {pid} ({classify_ssh(c['argv'])}) is"
                           f" still running {when}")
        left = live_fake_ssh()
        self.check(left == [], f"{len(left)} fake ssh.exe still running {when}"
                               f" (Toolhelp): {left}")

    def verify_bridge(self, profiles):
        """What the QUIC bridge itself saw: one connection, the ALPN, stream
        0, the token accepted and the cookie presented, then an orderly end
        of the client's making -- application error 0 both ways."""
        lines = [l for l in self.bridge_text.splitlines()
                 if not l.startswith("listening on ")]
        for line in ("conn 1: accepted", "conn 1: alpn debeos-urp/1",
                     "conn 1: stream 0 accepted", "conn 1: auth ok",
                     "conn 1: cookie presented to the backend",
                     "conn 1: closing with application error 0",
                     "conn 1: session ended"):
            count = lines.count(line)
            self.check(count == 1, f"the bridge logged '{line}' {count} times,"
                                   " expected 1")
        # Who sent CONNECTION_CLOSE first is a race between the client's
        # close and the bridge's answer to its FIN; the code is not.
        closes = [l for l in lines if " closed with " in l]
        self.check(closes in (["conn 1: peer closed with application error 0"],
                              ["conn 1: bridge closed with application error 0"]),
                   f"the bridge's close record is {closes}, expected one"
                   " 'closed with application error 0'")
        # The client ended the session (its FIN, or its close racing it),
        # not the backend.
        ends = [l for l in lines if l.startswith("conn 1: relay ended: ")]
        self.check(ends in (["conn 1: relay ended: client FIN"],
                            ["conn 1: relay ended: client closed"]),
                   f"relay end {ends}, expected the client's FIN or close")
        self.check(len(lines) == 9,  # the 7 above, the close and relay end
                   f"the bridge logged {len(lines)} lines, expected 9 (a"
                   f" second connection or an error?): {lines}")
        # Trust on first use, seeded from the PEM the fake ssh served: the
        # entry is exactly the fingerprint computed here.
        store = config_dir(self.work) / "known_brokers"
        entries = []
        if store.exists():
            entries = [l for l in store.read_text().splitlines()
                       if l and not l.startswith("#")]
        want = f"127.0.0.1:{QUIC_PORT} sha256:{self.quic.digest}"
        self.check(entries == [want],
                   f"known_brokers holds {entries}, expected [{want!r}]")

    def verify(self, client, mock, ssh_log, profiles, port, started):
        route, err, out = self.route, self.stderr, self.stdout
        mock_text = self.mock_text

        self.check(client.returncode == 0,
                   f"client exit status {client.returncode}, expected 0")
        if not self.reconnect:
            self.check(mock.returncode == 0,
                       f"mock exit status {mock.returncode}, expected 0 (its"
                       " session was refused or a check failed)")
        n = self.sessions

        # 1. Progress: the stages, by label, in order, nothing else -- once per
        # connect (a reconnect runs the whole managed connect again).
        prefix = "connect: " if self.front_end == "headless" else "e2e: stage "
        stages = [l[len(prefix):] for l in err.splitlines() if l.startswith(prefix)]
        want_stages = (route.stages_quic_build
                       if self.client_has_quic and route.stages_quic_build
                       else route.stages)
        # The GUI's progress screen runs the first connect only; a desktop's
        # reconnect shows its stages on the toolbar instead.
        n_stages = 1 if self.front_end != "headless" else n
        self.check(stages == want_stages * n_stages,
                   f"progress stages {stages}, expected {want_stages * n_stages}")

        # The route that carried the session.
        if self.front_end == "headless":
            notes = [l for l in err.splitlines() if l.startswith("route: ")]
            if route.quic:
                want = "route: " + route.route_note.format(fp=self.quic.fingerprint)
                self.check(notes == [want] * n,
                           f"route notes {notes}, expected {[want] * n}")
            else:
                self.check(len(notes) == n and all(
                               x.startswith("route: " + route.route_note)
                               and x.endswith(f" -> 127.0.0.1:{port}")
                               for x in notes),
                           f"route notes {notes}, expected {n} starting"
                           f" 'route: {route.route_note}' and ending ':{port}'")

        if route.quic and not self.reconnect:
            self.verify_bridge(profiles)
        elif route.quic:
            # One QUIC connection per session, each authenticated.
            for i in range(1, n + 1):
                count = self.bridge_text.count(f"conn {i}: auth ok")
                self.check(count == 1, f"the bridge logged 'conn {i}: auth ok'"
                                       f" {count} times, expected 1")
            self.check(f"conn {n + 1}: " not in self.bridge_text,
                       f"the bridge saw more than {n} connections")

        # 2. Handshake, from both ends. The server saw the cookie, the init and
        # the hello at the profile's size; the client decoded the hello ack.
        for line in ("<- RP_SESSION_COOKIE accepted",
                     "<- RP_INIT_CONNECTION",
                     "<- RP_HELLO version=1",
                     f"<- RP_UPDATE_DISPLAY_MODE {WIDTH}x{HEIGHT}"):
            count = mock_text.count(line)
            self.check(count == n,
                       f"the mock logged '{line}' {count} times, expected {n}")
        # Nothing but the session reached app_server: in particular not the
        # broker's RP_AUTHENTICATE preamble (code 10), which a bridge that
        # skipped its token check would relay behind the cookie.
        scenes = [l for l in mock_text.splitlines() if l.startswith("-> initial scene, ")]
        self.check(len(scenes) == n and all(f", {SCENE_MESSAGES} messages " in l
                                            for l in scenes),
                   f"the mock's scenes {scenes}, expected {n} of"
                   f" {SCENE_MESSAGES} messages")
        stray = [l for l in mock_text.splitlines() if "<- unexpected code" in l]
        self.check(stray == [], f"the mock received messages it should not: {stray}")
        acks = err.count("hello ack: version 1,")
        self.check(acks == n, f"the client decoded RP_HELLO_ACK {acks} times"
                              f" ('hello ack: version 1,'), expected {n}")
        if self.reconnect and self.front_end == "headless":
            self.check(" over 2 connections" in out,
                       f"stdout does not report 2 connections: {out!r}")

        # 3. Desktop presentation: exact pixels of the mock's scene.
        if self.front_end == "headless":
            png = self.work / "desktop.png"
            if png.exists():
                w, h, pixel = read_png_rgb(png)
                self.check((w, h) == (WIDTH, HEIGHT),
                           f"desktop is {w}x{h}, expected {WIDTH}x{HEIGHT}")
                for x, y, rgb in PIXELS:
                    got = pixel(x, y)
                    self.check(got == rgb, f"desktop pixel ({x},{y}) is {got},"
                                           f" expected {rgb}")
            else:
                self.failures.append("no desktop PNG was written")
            self.check(f'via profile "E2E {route.name}"' in out,
                       f"stdout does not report the profile session: {out!r}")
        else:
            expected = "e2e: desktop presented after "
            lines = [l for l in err.splitlines() if l.startswith(expected)]
            want = "".join(f" ({x},{y})={rgb}" for x, y, rgb in PIXELS)
            self.check(len(lines) == 1 and lines[0].endswith(":" + want),
                       f"desktop report {lines}, expected one ending ':{want}'")
            if self.reconnect and lines:
                # The surface keeps the last frame across a reconnect (it is
                # the only copy of the screen), so after one the pixels are
                # right before the new session has drawn anything. What shows
                # the reconnected session drew the scene is how much it had
                # decoded when they matched: at least the scene's own message
                # count, as the mock counted it.
                scenes = [int(l.split(", ")[2].split()[0])
                          for l in mock_text.splitlines()
                          if l.startswith("-> initial scene, ")]
                decoded = int(lines[0][len(expected):].split()[0])
                self.check(len(scenes) == n and decoded >= scenes[-1],
                           f"the reconnected desktop matched after {decoded}"
                           f" messages, fewer than its scene's {scenes} -- the"
                           " pixels were the old session's")

        # 4. Disconnect: every ssh child is gone once the client has exited --
        # the tunnel was torn down, not orphaned.
        # The timeline also shows a reconnect stopped the old forward (by
        # SIGTERM, not a SIGKILL escalation) before the new one started.
        entries = read_ssh_log(ssh_log)
        calls = [e for e in entries if "argv" in e]
        kind_of = {c["pid"]: classify_ssh(c["argv"]) for c in calls}
        timeline = [classify_ssh(e["argv"]) if "argv" in e
                    else f"{kind_of.get(e['pid'], 'unknown')}-exit:{e['exit']}"
                    for e in entries if "argv" in e or "exit" in e]
        # Readiness never connects through the forward: exactly one connection
        # per session crosses it, and it carries that session.
        accepts = [e for e in entries if "accept" in e]
        want_accepts = n if "tunnel" in route.ssh_calls else 0
        self.check(len(accepts) == want_accepts,
                   f"{len(accepts)} connections went through the forward,"
                   f" expected {want_accepts} (one per session, no probe)")
        self.check("closed without sending a byte" not in mock_text,
                   "app_server saw an empty connection (a readiness probe)")
        one = route.ssh_calls + ([f"tunnel-exit:{TUNNEL_STOP}"]
                                 if "tunnel" in route.ssh_calls else [])
        self.check(timeline == one * n,
                   f"ssh timeline {timeline}, expected {one * n}")
        for c in calls:
            argv = ["ssh"] + c["argv"][1:]
            self.check(argv[:len(SSH_PREFIX)] == SSH_PREFIX,
                       f"ssh argv lacks the hardened prefix: {argv}")
            if classify_ssh(c["argv"]) == "tunnel":
                self.check(argv[len(SSH_PREFIX):-1][:6]
                           == ["-o", "LogLevel=DEBUG1", "-o",
                               "ExitOnForwardFailure=yes", "-N", "-L"]
                           and argv[-2].startswith("127.0.0.1:")
                           and argv[-2].endswith(f":127.0.0.1:{port}"),
                           f"tunnel argv is not -o LogLevel=DEBUG1 -o"
                           f" ExitOnForwardFailure=yes -N -L 127.0.0.1:P:"
                           f"127.0.0.1:{port}: {argv}")
        self.check_no_ssh_left(calls, "after the client exited")

        # 5. Back to the library (GUI): shown again after the desktop, with the
        # connection recorded on the card. A --profile launch never shows it.
        if self.front_end == "gui-profile":
            shown = [l for l in err.splitlines() if l.startswith("e2e: library shown")]
            self.check(shown == [], f"--profile showed the library: {shown}")
            saved = json.loads(profiles.read_text())["profiles"][0]
            self.check(saved["lastConnectedAt"] >= started,
                       f"lastConnectedAt {saved['lastConnectedAt']} was not"
                       f" recorded (>= {started})")
        if self.front_end == "gui":
            lines = err.splitlines()
            shown = [i for i, l in enumerate(lines)
                     if l == "e2e: library shown with 1 profiles"]
            desktop = [i for i, l in enumerate(lines)
                       if l.startswith("e2e: desktop presented after ")]
            self.check(len(shown) == 2 and len(desktop) == 1
                       and shown[0] < desktop[0] < shown[1],
                       "the library was not shown, then the desktop, then the"
                       " library again")
            self.check("e2e: closing the library" in lines,
                       "the returned-to library was not closed by the script")
            saved = json.loads(profiles.read_text())["profiles"][0]
            self.check(saved["lastConnectedAt"] >= started,
                       f"lastConnectedAt {saved['lastConnectedAt']} was not"
                       f" recorded (>= {started})")
            self.check(saved["lastError"] == "",
                       f"lastError is {saved['lastError']!r}, expected ''")


def run_port_taken(client, work):
    """Another program holds 127.0.0.1:P, the profile's pinned local port.

    OpenSSH without a bind address would stay up on [::1]:P and the client,
    dialing 127.0.0.1:P, would hand the squatter its session cookie. With
    `-L 127.0.0.1:P:...` the forward fails, ExitOnForwardFailure ends ssh, and
    the connect fails on the forward. The squatter must see no connection at
    all, and app_server no session.
    """
    failures = []
    cookie = work / "session_cookie"
    mock_log = work / "mock.log"
    ssh_log = work / "ssh.log"
    profiles = work / "connections.json"
    with open(mock_log, "w") as log:
        mock = subprocess.Popen([sys.executable, "-u", str(MOCK), "--host",
                                 "127.0.0.1", "--port", "0", "--cookie-file",
                                 str(cookie), "--once"],
                                stdout=log, stderr=subprocess.STDOUT)
    squatter = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if not WINDOWS:  # there it would let a second socket share the port
        squatter.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    squatter.bind(("127.0.0.1", 0))
    squatter.listen(8)
    squatter.settimeout(0.2)
    taken = squatter.getsockname()[1]
    try:
        # The mock binds port 0 and reports the port it got.
        port = None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and mock.poll() is None:
            listening = re.search(r"listening on 127\.0\.0\.1:(\d+)",
                                  mock_log.read_text())
            if cookie.exists() and listening:
                port = int(listening.group(1))
                break
            time.sleep(0.05)
        if port is None:
            return ["the mock never came up"]
        profile = {
            "id": "e2e-taken", "name": "E2E taken", "mode": "ssh",
            "host": "127.0.0.1", "remotePort": port, "sshUser": "baron",
            "sshPort": 22, "localPort": taken, "width": WIDTH, "height": HEIGHT,
            "autoReconnect": False, "cookieSource": str(cookie),
        }
        profiles.write_text(json.dumps({"version": 2, "profiles": [profile]}))
        env = dict(os.environ)
        fake_ssh_env(env, ssh_log)
        env["FAKE_SSH_COOKIE_FILE"] = str(cookie)
        env["FAKE_SSH_REMOTE_PORT"] = str(port)
        env["HOME"] = str(work)
        env["XDG_CONFIG_HOME"] = str(work / "config")
        env["XDG_RUNTIME_DIR"] = str(work / "run")
        env["APPDATA"] = str(work / "config")
        env["LOCALAPPDATA"] = str(work / "local")
        client_run = run_client(
            [str(client), "--profile", "E2E taken", "--profiles", str(profiles),
             "--seconds", "2", "--output", str(work / "desktop.png")], env, work)
        connections = 0
        try:
            while True:
                conn, _ = squatter.accept()
                connections += 1
                conn.close()
        except (socket.timeout, OSError):
            pass
        if client_run.returncode != 1:
            failures.append(f"client exit status {client_run.returncode},"
                            " expected 1 (the connect failed)")
        # ssh's bind error, classified (connect_flow.cpp) as what it is.
        lines = client_run.stderr.splitlines()
        stages = [l for l in lines if l.startswith("connect: ")]
        if stages != ["connect: Starting SSH tunnel",
                      "connect: Waiting for the SSH forward"]:
            failures.append(f"progress stages {stages}: expected the forward"
                            " wait to be the last")
        if not any(l.startswith("connect failed: Local port already in use")
                   for l in lines):
            failures.append("the client did not report 'Local port already in"
                            f" use': {lines}")
        if connections != 0:
            failures.append(f"the program holding 127.0.0.1:{taken} received"
                            f" {connections} connection(s); expected none")
        entries = read_ssh_log(ssh_log)
        timeline = [classify_ssh(e["argv"]) if "argv" in e else f"exit:{e['exit']}"
                    for e in entries if "argv" in e or "exit" in e]
        if timeline != ["tunnel", "exit:255"]:
            failures.append(f"ssh timeline {timeline}, expected"
                            " ['tunnel', 'exit:255'] (ExitOnForwardFailure)")
        if "RP_SESSION_COOKIE accepted" in mock_log.read_text():
            failures.append("app_server accepted a session")
    finally:
        squatter.close()
        if mock.poll() is None:
            mock.kill()
            mock.wait()
    return failures


def alpn_control(quic, root):
    """The bridge refuses any ALPN but debeos-urp/1 -- what a client offering
    the wrong one meets. A probe (the bridge binary's own client mode, OpenSSL
    underneath) offers debeos-urp/2 and must not complete a handshake, and
    the bridge must say it refused; the same probe offering debeos-urp/1 must
    complete, so the probe itself is shown able to succeed. Returns (skip
    reason or None, failures)."""
    work = root / "quic-alpn"
    work.mkdir()
    cookie = work / "session_cookie"
    cookie.write_text("0" * 64 + "\n")
    bridge, why = quic.start(work, free_port(), cookie)
    if bridge is None:
        return why, []
    failures = []
    try:
        probes = {}
        for alpn in ("debeos-urp/2", "debeos-urp/1"):
            done = subprocess.run([str(quic.bridge), "--probe-alpn", alpn,
                                   "--port", str(QUIC_PORT)],
                                  capture_output=True, text=True, timeout=30)
            probes[alpn] = (done.returncode, done.stdout.strip())
        time.sleep(0.3)
    finally:
        stop(bridge)
    text = (work / "bridge.log").read_text()
    rc, out = probes["debeos-urp/2"]
    if rc != 1 or not out.startswith("probe: handshake failed"):
        failures.append(f"a client offering debeos-urp/2 got {out!r} (exit {rc}),"
                        " expected 'probe: handshake failed' (exit 1)")
    refused = text.count("alpn refused (client offered: debeos-urp/2)")
    if refused < 1:
        failures.append("the bridge never logged 'alpn refused (client offered:"
                        " debeos-urp/2)'")
    if "alpn debeos-urp/2" in text:
        failures.append("the bridge accepted ALPN debeos-urp/2")
    rc, out = probes["debeos-urp/1"]
    if rc != 0 or out != "probe: handshake completed, alpn debeos-urp/1":
        failures.append(f"a client offering debeos-urp/1 got {out!r} (exit {rc}),"
                        " expected 'probe: handshake completed, alpn"
                        " debeos-urp/1' (exit 0)")
    if failures:
        failures.append(f"(logs in {work})")
    return None, failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--client", required=True, help="build/haiku-remote")
    ap.add_argument("--gui", help="haiku-remote-gui (skipped when absent)")
    ap.add_argument("--quic-bridge",
                    help="build/quic_bridge_mock; given exactly when the client"
                         " was built with QUIC (OpenSSL >= 3.5)")
    ap.add_argument("--fake-ssh",
                    help="the built fake ssh.exe (tests/e2e/fake-ssh/ssh_win.cpp);"
                         " required on Windows, where the client runs only an"
                         " .exe for ssh")
    ap.add_argument("--route", action="append",
                    help="run only these routes (default: all)")
    ap.add_argument("--keep", action="store_true", help="keep the work dirs")
    a = ap.parse_args()
    global FAKE_SSH_DIR
    if WINDOWS:
        if not a.fake_ssh or not Path(a.fake_ssh).is_file():
            print(f"no fake ssh.exe (--fake-ssh {a.fake_ssh}): on Windows the"
                  " harness needs tests/e2e/fake-ssh/ssh_win.cpp built",
                  file=sys.stderr)
            return 2
        if Path(a.fake_ssh).name.lower() != "ssh.exe":
            print(f"--fake-ssh must be named ssh.exe, not {a.fake_ssh}",
                  file=sys.stderr)
            return 2
        FAKE_SSH_DIR = Path(a.fake_ssh).resolve().parent

    front_ends = [("headless", Path(a.client))]
    if a.gui and Path(a.gui).exists():
        front_ends.append(("gui", Path(a.gui)))
        front_ends.append(("gui-profile", Path(a.gui)))
    else:
        print("SKIP: gui front end (haiku-remote-gui was not built; the"
              " library -> desktop -> library half of the test needs it)")
    for _, binary in front_ends:
        if not binary.exists():
            print(f"no such binary: {binary}", file=sys.stderr)
            return 2
    if a.quic_bridge and not Path(a.quic_bridge).exists():
        print(f"no such binary: {a.quic_bridge}", file=sys.stderr)
        return 2

    root = Path(tempfile.mkdtemp(prefix="haiku-remote-e2e-"))
    client_has_quic = bool(a.quic_bridge)
    quic = Quic(a.quic_bridge, root) if client_has_quic else None
    if not client_has_quic:
        print("SKIP: QUIC routes (no --quic-bridge: this client was built"
              " without QUIC, OpenSSL < 3.5; the 'auto' fallback is checked"
              " with the non-QUIC plan)")
    has_wss = client_has_wss(a.client)
    print(f"client built {'with' if has_wss else 'without'} wss")
    all_routes = routes(has_wss, client_has_quic)
    selected = [r for r in all_routes if (not a.route or r.name in a.route)
                and (client_has_quic or not r.quic)]
    failed = 0
    total = 0
    skipped = []

    def run(route, front_end, binary, wrong_cookie=False, reconnect=False,
            wrong_token=False, cancel_reconnect=False, quit_signal=None):
        suffix = ("-negative" if wrong_cookie else "-reconnect" if reconnect
                  else "-wrong-token" if wrong_token
                  else "-reconnect-cancel" if cancel_reconnect
                  else f"-{quit_signal}" if quit_signal else "")
        work = root / f"{front_end}-{route.name}{suffix}"
        work.mkdir()
        case = Case(route, front_end, binary, work, wrong_cookie, reconnect,
                    quic=quic, client_has_quic=client_has_quic,
                    wrong_token=wrong_token, cancel_reconnect=cancel_reconnect,
                    quit_signal=quit_signal)
        case.run()
        return case

    def skip(label, why):
        skipped.append(label)
        print(f"SKIP: {label} -- {why}; THIS CASE DID NOT RUN")

    for route in selected:
        for front_end, binary in front_ends:
            label = f"{front_end} / {route.name}"
            case = run(route, front_end, binary)
            if case.skipped:
                skip(label, case.skipped)
                continue
            total += 1
            if case.failures:
                failed += 1
                print(f"FAIL: {label}")
                for f in case.failures:
                    print(f"    {f}")
                print(f"    (logs in {case.work})")
                a.keep = True
            else:
                print(f"PASS: {label}")

    # Reconnect: a saved profile's Auto reconnect, over a rebuilt tunnel --
    # headless (the capture loop) and in the GUI's desktop (AsyncConnect on a
    # worker, re-handshaking the desktop's own session), over the tunnel and
    # over QUIC; and the GUI closed while a reconnect hangs.
    reconnects = [("ssh", "headless", Path(a.client), {"reconnect": True})]
    if a.gui and Path(a.gui).exists():
        reconnects += [("ssh", "gui", Path(a.gui), {"reconnect": True}),
                       ("ssh", "gui", Path(a.gui), {"cancel_reconnect": True})]
        if client_has_quic:
            reconnects += [("quic", "gui", Path(a.gui), {"reconnect": True})]
    # Quitting: SIGTERM and SIGINT (Windows: Ctrl+C, Ctrl+Break, the console
    # closed) end the app, session and tunnel included, where closing the
    # window (the plain "gui" cases) returns to the library.
    if a.gui and Path(a.gui).exists():
        reconnects += [("ssh", "gui", Path(a.gui), {"quit_signal": what})
                       for what in QUIT_REQUESTS]
    for route_name, front_end, binary, how in reconnects:
        if a.route and route_name not in a.route:
            continue
        route = next(r for r in all_routes if r.name == route_name)
        label = (f"{front_end} / {route_name} / "
                 + (f"quit by {how['quit_signal']}" if how.get("quit_signal")
                    else "reconnect" if how.get("reconnect")
                    else "reconnect cancelled"))
        case = run(route, front_end, binary, **how)
        if case.skipped:
            skip(label, case.skipped)
            continue
        total += 1
        if case.failures:
            failed += 1
            print(f"FAIL: {label}")
            for f in case.failures:
                print(f"    {f}")
            print(f"    (logs in {case.work})")
            a.keep = True
        else:
            print(f"PASS: {label}")

    # The negative control: the same case with the handshake made impossible.
    # It must fail, and fail on the handshake -- otherwise the checks above
    # cannot tell a session from no session.
    total += 1
    control = run(next(r for r in all_routes if r.name == "ssh"), "headless",
                  Path(a.client), wrong_cookie=True)
    handshake_caught = any("RP_SESSION_COOKIE accepted" in f
                           and " 0 times" in f for f in control.failures)
    if control.failures and handshake_caught:
        print("PASS: negative control (wrong cookie -> no handshake) failed as"
              f" it must, with {len(control.failures)} failed checks")
    else:
        failed += 1
        print("FAIL: negative control -- a case with no possible handshake"
              f" was not caught (failures: {control.failures})")
        a.keep = True

    # A taken local port: the forward must fail, never reach the squatter.
    if not a.route or "ssh-port-taken" in a.route:
        total += 1
        work = root / "headless-ssh-port-taken"
        work.mkdir()
        port_failures = run_port_taken(Path(a.client), work)
        if port_failures:
            failed += 1
            print("FAIL: headless / ssh-port-taken")
            for f in port_failures:
                print(f"    {f}")
            print(f"    (logs in {work})")
            a.keep = True
        else:
            print("PASS: headless / ssh-port-taken")


    if client_has_quic:
        # The broker's token check: the guest hands out a token the bridge
        # does not hold. The case must fail on the auth -- the bridge denying
        # it, by its own log -- with nothing reaching the mock.
        quic_route = next(r for r in all_routes if r.name == "quic")
        control = run(quic_route, "headless", Path(a.client), wrong_token=True)
        if control.skipped:
            skip("negative control (wrong broker token)", control.skipped)
        else:
            total += 1
            denied = control.bridge_text.count("conn 1: auth denied") == 1
            auth_caught = any("'conn 1: auth ok' 0 times" in f
                              for f in control.failures)
            no_session = any("RP_SESSION_COOKIE accepted" in f
                             and " 0 times" in f for f in control.failures)
            if control.failures and denied and auth_caught and no_session:
                print("PASS: negative control (wrong broker token -> denied by"
                      f" the bridge) failed as it must, with"
                      f" {len(control.failures)} failed checks")
            else:
                failed += 1
                print("FAIL: negative control -- a wrong broker token was not"
                      f" denied (bridge said 'auth denied': {denied}; failures:"
                      f" {control.failures}; logs in {control.work})")
                a.keep = True

        why, failures = alpn_control(quic, root)
        if why:
            skip("ALPN control (debeos-urp/2 refused)", why)
        else:
            total += 1
            if failures:
                failed += 1
                print("FAIL: ALPN control -- the bridge did not refuse a wrong"
                      " ALPN as it must")
                for f in failures:
                    print(f"    {f}")
                a.keep = True
            else:
                print("PASS: ALPN control (debeos-urp/2 refused, debeos-urp/1"
                      " accepted)")

    summary = (f"{'PASS' if not failed else 'FAIL'} - {total - failed} of"
               f" {total} end-to-end cases")
    if skipped:
        summary += f"; {len(skipped)} SKIPPED (did not run): {', '.join(skipped)}"
    print(summary)
    if not a.keep:
        shutil.rmtree(root, ignore_errors=True)
    else:
        print(f"work dirs kept in {root}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
