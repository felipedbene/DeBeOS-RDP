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
transport -- the QUIC route on feat/quic-transport -- is one more ROUTES entry
that states its own stages, route note and expected ssh traffic; nothing else
here changes.

Every expectation is stated independently of the client: stage labels, the
route note, pixel values (from the mock's own initial_scene), the ssh argv (a
hand-written golden list), and what the mock itself logged. A negative control
runs a case whose handshake cannot happen (the mock is handed a different
cookie than the guest publishes) and requires the checks to FAIL -- so this
harness is shown, on every run, to be able to fail.

Exit status: 0 all cases passed (or a front end was skipped and said so), 1 a
case failed, 2 bad invocation.
"""

import argparse
import json
import os
import socket
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
FAKE_SSH_DIR = HERE / "fake-ssh"
MOCK = REPO / "tools" / "rp_mock_server.py"

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
    # Extra mock arguments, and whether the mock serves exactly one session.
    mock_args: list = field(default_factory=list)


ROUTES = [
    # "direct" is planned as broker first, then the SSH tunnel
    # (profile_launch.cpp). This guest has no remote_broker, so the broker
    # step fails at its credential fetch and the tunnel carries the session.
    # If direct becomes a plain TCP route (see the PR for that decision), this
    # entry becomes: no ssh_calls, stages ["Connecting to session"],
    # cookie_from_file=True.
    Route(name="direct", mode="direct",
          stages=["Contacting broker", "Fetching session cookie",
                  "Opening SSH tunnel", "Connecting to session"],
          ssh_calls=["broker", "cookie", "tunnel"],
          route_note="ssh tunnel: 127.0.0.1:"),
    Route(name="ssh", mode="ssh",
          stages=["Fetching session cookie", "Opening SSH tunnel",
                  "Connecting to session"],
          ssh_calls=["cookie", "tunnel"],
          route_note="ssh tunnel: 127.0.0.1:"),
    Route(name="ssh-cookie-file", mode="ssh",
          stages=["Opening SSH tunnel", "Connecting to session"],
          ssh_calls=["tunnel"],
          route_note="ssh tunnel: 127.0.0.1:",
          cookie_from_file=True),
    # feat/quic-transport: add Route(name="quic", mode=..., stages=[...],
    # ssh_calls=[...], route_note=...) here.
]


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


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


def pid_alive(pid):
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


class Case:
    def __init__(self, route, front_end, binary, work, wrong_cookie=False,
                 reconnect=False):
        self.route, self.front_end, self.binary = route, front_end, binary
        self.work = work
        self.wrong_cookie = wrong_cookie
        # The reconnect scenario: the mock drops each session (a clean FIN,
        # the shape of a dead tunnel) after 5 update frames, and the profile's
        # own Auto reconnect -- no --reconnect flag -- must bring it back once
        # (--reconnect-max-attempts 1), over a rebuilt tunnel.
        self.reconnect = reconnect
        self.sessions = 2 if reconnect else 1
        self.failures = []

    def check(self, ok, what):
        if not ok:
            self.failures.append(what)

    def run(self):
        work, route = self.work, self.route
        port = free_port()
        cookie = work / "session_cookie"
        mock_log = work / "mock.log"
        ssh_log = work / "ssh.log"
        profiles = work / "connections.json"
        name = f"E2E {route.name}"

        mock_cmd = [sys.executable, "-u", str(MOCK), "--host", "127.0.0.1",
                    "--port", str(port), "--cookie-file", str(cookie)]
        mock_cmd += ["--drop-after", "5"] if self.reconnect else ["--once"]
        mock_cmd += route.mock_args
        with open(mock_log, "w") as log:
            mock = subprocess.Popen(mock_cmd, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if cookie.exists() and "listening" in mock_log.read_text():
                    break
                time.sleep(0.05)
            else:
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
            profiles.write_text(json.dumps({"version": 1, "profiles": [profile]}))

            env = dict(os.environ)
            env["PATH"] = str(FAKE_SSH_DIR) + os.pathsep + env.get("PATH", "")
            env["FAKE_SSH_LOG"] = str(ssh_log)
            env["FAKE_SSH_COOKIE_FILE"] = str(served)
            env["FAKE_SSH_REMOTE_PORT"] = str(port)
            # Keep the user's real stores out of it.
            env["HOME"] = str(work)
            env["XDG_CONFIG_HOME"] = str(work / "config")
            started = int(time.time())

            if self.front_end == "headless":
                png = work / "desktop.png"
                cmd = [str(self.binary), "--profile", name, "--profiles",
                       str(profiles), "--output", str(png)]
                cmd += (["--seconds", "5", "--reconnect-max-attempts", "1"]
                        if self.reconnect else ["--seconds", "2"])
            else:
                probes = ",".join(f"{x},{y},{rgb}" for x, y, rgb in PIXELS)
                env["SDL_VIDEODRIVER"] = "dummy"
                env["SDL_AUDIODRIVER"] = "dummy"
                env["HAIKU_REMOTE_SOFTWARE_RENDERER"] = "1"
                if self.front_end == "gui":
                    env["HAIKU_REMOTE_TEST_SCRIPT"] = (
                        f"library:connect={name};session:close-when={probes};"
                        "library:close")
                    cmd = [str(self.binary), "--profiles", str(profiles)]
                else:  # gui-profile: --profile connects at once, no library
                    env["HAIKU_REMOTE_TEST_SCRIPT"] = f"session:close-when={probes}"
                    cmd = [str(self.binary), "--profile", name, "--profiles",
                           str(profiles)]
            try:
                client = subprocess.run(cmd, env=env, capture_output=True,
                                        text=True, timeout=90)
            except subprocess.TimeoutExpired as e:
                self.failures.append("the client did not exit within 90 s")
                for stream, text in (("stdout", e.stdout), ("stderr", e.stderr)):
                    if isinstance(text, bytes):
                        text = text.decode(errors="replace")
                    (self.work / f"client.{stream}").write_text(text or "")
                return
            self.stdout, self.stderr = client.stdout, client.stderr
            (self.work / "client.stdout").write_text(self.stdout)
            (self.work / "client.stderr").write_text(self.stderr)
            if not self.reconnect:
                try:
                    mock.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    self.failures.append("the mock did not finish its one session")
            else:
                time.sleep(0.5)  # let the mock log the last session's end
            self.mock_text = mock_log.read_text()
            self.verify(client, mock, ssh_log, profiles, port, started)
        finally:
            if mock.poll() is None:
                mock.kill()
                mock.wait()

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
        self.check(stages == route.stages * n,
                   f"progress stages {stages}, expected {route.stages * n}")

        # The route that carried the session.
        if self.front_end == "headless":
            notes = [l for l in err.splitlines() if l.startswith("route: ")]
            self.check(len(notes) == n and all(
                           x.startswith("route: " + route.route_note)
                           and x.endswith(f" -> 127.0.0.1:{port}") for x in notes),
                       f"route notes {notes}, expected {n} starting"
                       f" 'route: {route.route_note}' and ending ':{port}'")

        # 2. Handshake, from both ends. The server saw the cookie, the init and
        # the hello at the profile's size; the client decoded the hello ack.
        for line in ("<- RP_SESSION_COOKIE accepted",
                     "<- RP_INIT_CONNECTION",
                     "<- RP_HELLO version=1",
                     f"<- RP_UPDATE_DISPLAY_MODE {WIDTH}x{HEIGHT}"):
            count = mock_text.count(line)
            self.check(count == n,
                       f"the mock logged '{line}' {count} times, expected {n}")
        acks = err.count("hello ack: version 1,")
        self.check(acks == n, f"the client decoded RP_HELLO_ACK {acks} times"
                              f" ('hello ack: version 1,'), expected {n}")
        if self.reconnect:
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

        # 4. Disconnect: every ssh child is gone once the client has exited --
        # the tunnel was torn down, not orphaned.
        # The timeline also shows a reconnect stopped the old forward (by
        # SIGTERM, not a SIGKILL escalation) before the new one started.
        entries = []
        if ssh_log.exists():
            entries = [json.loads(l) for l in ssh_log.read_text().splitlines()]
        calls = [e for e in entries if "argv" in e]
        kind_of = {c["pid"]: classify_ssh(c["argv"]) for c in calls}
        timeline = [classify_ssh(e["argv"]) if "argv" in e
                    else f"{kind_of.get(e['pid'], 'unknown')}-exit:{e['exit']}"
                    for e in entries]
        one = route.ssh_calls + (["tunnel-exit:SIGTERM"]
                                 if "tunnel" in route.ssh_calls else [])
        self.check(timeline == one * n,
                   f"ssh timeline {timeline}, expected {one * n}")
        for c in calls:
            argv = ["ssh"] + c["argv"][1:]
            self.check(argv[:len(SSH_PREFIX)] == SSH_PREFIX,
                       f"ssh argv lacks the hardened prefix: {argv}")
            self.check(not pid_alive(c["pid"]),
                       f"ssh child {c['pid']} ({classify_ssh(c['argv'])}) is"
                       " still running after the client exited")

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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--client", required=True, help="build/haiku-remote")
    ap.add_argument("--gui", help="haiku-remote-gui (skipped when absent)")
    ap.add_argument("--route", action="append",
                    help="run only these routes (default: all)")
    ap.add_argument("--keep", action="store_true", help="keep the work dirs")
    a = ap.parse_args()

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

    routes = [r for r in ROUTES if not a.route or r.name in a.route]
    root = Path(tempfile.mkdtemp(prefix="haiku-remote-e2e-"))
    failed = 0
    total = 0

    def run(route, front_end, binary, wrong_cookie=False, reconnect=False):
        suffix = "-negative" if wrong_cookie else "-reconnect" if reconnect else ""
        work = root / f"{front_end}-{route.name}{suffix}"
        work.mkdir()
        case = Case(route, front_end, binary, work, wrong_cookie, reconnect)
        case.run()
        return case

    for route in routes:
        for front_end, binary in front_ends:
            total += 1
            case = run(route, front_end, binary)
            label = f"{front_end} / {route.name}"
            if case.failures:
                failed += 1
                print(f"FAIL: {label}")
                for f in case.failures:
                    print(f"    {f}")
                print(f"    (logs in {case.work})")
                a.keep = True
            else:
                print(f"PASS: {label}")

    # Reconnect: a saved profile's Auto reconnect, over a rebuilt tunnel.
    if not a.route or "ssh" in a.route:
        total += 1
        ssh_route = next(r for r in ROUTES if r.name == "ssh")
        case = run(ssh_route, "headless", Path(a.client), reconnect=True)
        if case.failures:
            failed += 1
            print("FAIL: headless / ssh / reconnect")
            for f in case.failures:
                print(f"    {f}")
            print(f"    (logs in {case.work})")
            a.keep = True
        else:
            print("PASS: headless / ssh / reconnect")

    # The negative control: the same case with the handshake made impossible.
    # It must fail, and fail on the handshake -- otherwise the checks above
    # cannot tell a session from no session.
    total += 1
    control = run(ROUTES[0], "headless", Path(a.client), wrong_cookie=True)
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

    print(f"{'PASS' if not failed else 'FAIL'} - {total - failed} of {total}"
          " end-to-end cases")
    if not a.keep:
        subprocess.run(["rm", "-rf", str(root)], check=False)
    else:
        print(f"work dirs kept in {root}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
