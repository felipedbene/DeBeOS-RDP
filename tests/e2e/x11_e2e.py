#!/usr/bin/env python3
"""The X11 front end's own window behaviour, under Xvfb, driven through XTest.

tests/e2e/profile_e2e.py already holds haiku-remote-x11 to the SDL front end's
expectations for the library -> connect -> desktop -> library flow, reconnect
and quitting. This covers what that harness cannot see, because it is the X
window itself: input arriving as real X events through the server (XTest, via
tools/x11_test_helper.c), the toolbar, the X CLIPBOARD selection, and the
window being resized.

  toolbar     reveal the bar from the top edge, Keys, Ctrl+Alt+Del: the mock
              must log one RP_SECURE_ATTENTION it negotiated and draw its Team
              Monitor stand-in, which must then be on the X window as an exact
              pixel. Size -> 1280x800: the mock must be asked for exactly that
              mode and the window brought to it. Fit, then a window resize: the
              mock must be asked for the new window size. Ctrl+Alt+Home pins the
              bar (its panel colour where the desktop was) and unpins it (the
              desktop's pixel back). Disconnect ends the client with status 0.
  negative    the same Keys click against a mock without
              RP_CAP_SECURE_ATTENTION: no Ctrl+Alt+Del button, so the click
              lands on the row's Cmd+` instead, and no RP_SECURE_ATTENTION is
              ever sent -- the mock logs a key event with key 0x11 and nothing
              about secure attention.
  library     type into the search field, select all, copy: another X client
              reads exactly that text from CLIPBOARD; another client owns
              CLIPBOARD, paste, select all, copy: the pasted text comes back,
              so both directions crossed real selections. Then resize the
              window: the screen must re-lay out at the new size (its New
              connection button follows the right edge, and the new area is
              painted, not left black).

  clipboard   1 MiB of UTF-8 (every encoded length, numbered) pasted from
              another client that serves it through INCR in 64 KiB chunks, then
              copied and read back by that client -- as UTF8_STRING byte for
              byte through INCR in chunks no larger than a request, and as
              STRING in its Latin-1 form; TARGETS lists TARGETS, TIMESTAMP,
              UTF8_STRING and STRING, and TIMESTAMP is a non-zero INTEGER.
  vanishing   the owner dies after one chunk: the paste inserts nothing and
              the client says why within 2.5 s; a requestor dies after one
              chunk: the client stays up and serves the next request in full.
  scaling     a still, asymmetric scene (rp_mock_server.py --scale-fixture)
              in windows wider, narrower and taller than the desktop: exact
              pixels where SDL's letterbox and nearest filter put them, black
              bars, and clicks mapped back to SDL's desktop coordinates; then
              the same with SDL_RENDER_SCALE_QUALITY=linear, as exact blends.
  full screen under a minimal EWMH window manager (x11_test_helper wm): the
              toolbar's Full screen asks to ADD _NET_WM_STATE_FULLSCREEN, the
              desktop is scaled to the screen exactly, and asking again
              REMOVEs it and restores the window.

Every expected value is stated here from its source rather than read back from
the client: geometry from DesktopShell's layout constants (desktop_shell.cpp),
colours from the mock (TEAM_MONITOR_COLOR, DESKTOP_BG) and from ui_theme.hpp.
Exit status: 0 pass, 1 fail; a host without Xvfb or XTest reports SKIPPED.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MOCK = ROOT / "tools" / "rp_mock_server.py"

# tools/rp_mock_server.py
TEAM_MONITOR = "5a3c96"          # TEAM_MONITOR_COLOR, at TEAM_MONITOR_RECT
DESKTOP_BG = "336698"            # DESKTOP_BG, the scene's background
# include/haiku_remote/ui_theme.hpp
LIBRARY_BACKGROUND = "f6f3ee"    # theme::background
LIBRARY_ACCENT = "2559bd"        # theme::accent, the New connection button
# src/desktop_shell.cpp
BAR_PANEL = "242730"             # panel {36, 39, 48}

# DesktopShell's layout (desktop_shell.cpp): the bar is min(desktop, 860) wide,
# centred at the top, 40 high; its buttons are 80 x 26 at y 7, 6 apart,
# right-aligned 8 from the bar's right edge.
MAX_BAR, BUTTON_W, GAP, MARGIN, BUTTON_Y, BUTTON_H = 860, 80, 6, 8, 7, 26


def button_centre(desktop_width, row_length, index):
    bar = min(desktop_width, MAX_BAR)
    left = (desktop_width - bar) // 2
    first = bar - MARGIN - row_length * (BUTTON_W + GAP) + GAP
    return (left + first + index * (BUTTON_W + GAP) + BUTTON_W // 2,
            BUTTON_Y + BUTTON_H // 2)


# The rows, in the order layout() builds them.
MAIN = ["disconnect", "reconnect", "repaint", "fullscreen", "sizes", "keys", "stats"]
KEYS_WITH_SAS = ["Cmd+F1", "Cmd+F2", "Cmd+F3", "Cmd+F4", "Cmd+`", "Ctrl+Alt+Del", "back"]
KEYS_WITHOUT = ["Cmd+F1", "Cmd+F2", "Cmd+F3", "Cmd+F4", "Cmd+`", "back"]
SIZES = ["fit", "window", "1024x768", "1280x800", "1366x768", "1600x900",
         "1920x1080", "back"]


class Harness:
    def __init__(self, x11, helper, work, display):
        self.x11, self.helper, self.work, self.display = x11, helper, work, display
        self.failures = []

    def check(self, ok, what):
        if not ok:
            self.failures.append(what)

    def env(self):
        env = dict(os.environ)
        env["DISPLAY"] = self.display
        env["HOME"] = str(self.work)
        env["XDG_CONFIG_HOME"] = str(self.work / "config")
        return env

    def h(self, *steps, check=True):
        result = subprocess.run([str(self.helper), *map(str, steps)],
                                env=self.env(), capture_output=True, text=True,
                                timeout=60)
        if check and result.returncode != 0:
            self.failures.append(f"helper {' '.join(map(str, steps))} exited"
                                 f" {result.returncode}: {result.stderr.strip()}")
        return result.stdout.strip()

    def click(self, desktop_width, row, name):
        x, y = button_centre(desktop_width, len(row), row.index(name))
        self.h("click", x, y, "sleep", 400)

    def reveal(self, desktop_width):
        x, _ = button_centre(desktop_width, len(MAIN), 0)
        self.h("move", x, 0, "sleep", 300, "move", x, 20, "sleep", 300)

    def start_mock(self, name, *flags):
        log = self.work / f"{name}-mock.log"
        cookie = self.work / f"{name}-cookie"
        proc = subprocess.Popen([sys.executable, "-u", str(MOCK), "--host", "127.0.0.1",
                                 "--port", "0", "--cookie-file", str(cookie), *flags],
                                stdout=open(log, "w"), stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            text = log.read_text()
            if "listening on 127.0.0.1:" in text and cookie.exists():
                port = int(text.split("listening on 127.0.0.1:")[1].split()[0])
                return proc, log, cookie, port
            time.sleep(0.05)
        raise RuntimeError("the mock did not come up")

    def start_client(self, name, *args, env_extra=None):
        err = self.work / f"{name}-client.log"
        env = self.env()
        env.update(env_extra or {})
        proc = subprocess.Popen([str(self.x11), *map(str, args)], env=env,
                                stdout=subprocess.DEVNULL, stderr=open(err, "w"))
        return proc, err

    def profiles(self):
        """A one-profile library that is never connected to."""
        path = self.work / "connections.json"
        path.write_text('{"version":2,"profiles":[{"id":"p1","name":"Lab box",'
                        '"mode":"direct","host":"127.0.0.1","remotePort":1,'
                        '"width":1024,"height":700,"autoReconnect":false}]}')
        return path

    def expect_pixels(self, what, expected, window="Haiku Remote"):
        """Each (x, y) of the desktop window must be exactly its RRGGBB."""
        points = list(expected)
        steps = []
        for x, y in points:
            steps += ["pixel", window, x, y]
        got = self.h(*steps).split()
        if len(got) != len(points):
            self.failures.append(f"{what}: read {len(got)} of {len(points)} pixels")
            return
        for (x, y), value in zip(points, got):
            want = expected[(x, y)]
            self.check(value == want, f"{what}: pixel ({x},{y}) is {value}, expected"
                       f" {want}")


def stop(proc):
    if proc is not None and proc.poll() is None:
        proc.kill()
        proc.wait()


def display_in_use(number):
    """An X server holds display :number -- its lock names a live process. A
    lock left by a server that was killed is stale, and Xvfb reclaims it, so
    killed runs do not use the range up."""
    lock = Path(f"/tmp/.X{number}-lock")
    try:
        pid = int(lock.read_text().strip())
    except FileNotFoundError:
        return False
    except (OSError, ValueError):
        return True
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def stop_server(proc):
    """Stop Xvfb with SIGTERM, so it removes its own lock and socket."""
    if proc is not None and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def toolbar(t):
    mock, mock_log, cookie, port = t.start_mock("toolbar")
    client, client_log = t.start_client("toolbar", "--port", port, "--cookie-file",
                                        cookie, "--width", 1024, "--height", 700)
    try:
        t.h("wait", "Haiku Remote", 10, "sleep", 1500)
        t.check(t.h("geom", "Haiku Remote") == "1024 700", "the desktop window is not"
                " 1024x700")
        # (550,380): inside the stand-in's rectangle, outside the scene's window.
        before = t.h("pixel", "Haiku Remote", 550, 380)
        t.check(before == DESKTOP_BG, f"pixel (550,380) is {before} before"
                f" Ctrl+Alt+Del, expected the desktop's {DESKTOP_BG}")

        # Keys, then Ctrl+Alt+Del (offered: the mock negotiated the bit).
        t.reveal(1024)
        t.click(1024, MAIN, "keys")
        t.click(1024, KEYS_WITH_SAS, "Ctrl+Alt+Del")
        t.h("sleep", 800)
        text = mock_log.read_text()
        t.check(text.count("<- RP_SECURE_ATTENTION (0B payload) -> raising the Team"
                           " Monitor stand-in (request 1)") == 1
                and "request 2" not in text,
                "the mock did not act on exactly one RP_SECURE_ATTENTION")
        t.check("ack version=1 caps=0x15" in text, "the mock did not acknowledge"
                " RP_CAP_SECURE_ATTENTION (caps 0x15 = string width | resync |"
                " secure attention)")
        after = t.h("pixel", "Haiku Remote", 550, 380)
        t.check(after == TEAM_MONITOR, f"pixel (550,380) is {after} after"
                f" Ctrl+Alt+Del, expected the Team Monitor stand-in {TEAM_MONITOR}")

        # Back, Size, 1280x800: the mode asked for and the window brought to it.
        t.click(1024, KEYS_WITH_SAS, "back")
        t.click(1024, MAIN, "sizes")
        t.click(1024, SIZES, "1280x800")
        # Leave the bar: it hides after DesktopShell::hide_delay (1.2 s) and
        # goes back to its main row.
        t.h("move", 640, 500, "sleep", 1800)
        t.check("<- RP_UPDATE_DISPLAY_MODE 1280x800" in mock_log.read_text(),
                "the mock was not asked for 1280x800")
        t.check(t.h("geom", "Haiku Remote") == "1280 800",
                "the window was not brought to 1280x800")
        corner = t.h("pixel", "Haiku Remote", 1270, 790)
        t.check(corner == DESKTOP_BG, f"pixel (1270,790) is {corner}, expected the"
                f" resized desktop's {DESKTOP_BG}")

        # Fit on, then a window resize: the desktop follows the window.
        t.reveal(1280)
        t.click(1280, MAIN, "sizes")
        t.click(1280, SIZES, "fit")
        t.h("move", 640, 500, "sleep", 1500)  # leave the bar so it hides
        t.h("resize", "Haiku Remote", 1100, 700, "sleep", 1500)
        t.check("<- RP_UPDATE_DISPLAY_MODE 1100x700" in mock_log.read_text(),
                "with Fit on, a window resized to 1100x700 did not ask for 1100x700")

        # The hotkey pins the bar (its panel where the desktop showed) and
        # unpins it (the desktop's own pixel back from the image).
        bar_left = (1100 - min(1100, MAX_BAR)) // 2
        t.h("chord", "ctrl+alt+Home", "sleep", 500)
        pinned = t.h("pixel", "Haiku Remote", bar_left + 2, 2)
        t.check(pinned == BAR_PANEL, f"pixel ({bar_left + 2},2) is {pinned} with the"
                f" bar pinned, expected its panel {BAR_PANEL}")
        t.h("chord", "ctrl+alt+Home", "sleep", 500)
        unpinned = t.h("pixel", "Haiku Remote", bar_left + 2, 2)
        t.check(unpinned == DESKTOP_BG, f"pixel ({bar_left + 2},2) is {unpinned} with"
                f" the bar hidden again, expected the desktop's {DESKTOP_BG}")

        # Disconnect ends the desktop, and the CLI run, with status 0.
        t.reveal(1100)
        t.click(1100, MAIN, "disconnect")
        try:
            client.wait(timeout=10)
        except subprocess.TimeoutExpired:
            t.failures.append("Disconnect did not end the client within 10 s")
        t.check(client.returncode == 0, f"client exit status {client.returncode}"
                " after Disconnect, expected 0")
        log = client_log.read_text()
        t.check(log.count("secure attention requested (RP_SECURE_ATTENTION)") == 1,
                "the client did not report one secure-attention request")
    finally:
        stop(client)
        stop(mock)


def negative(t):
    mock, mock_log, cookie, port = t.start_mock("negative", "--no-secure-attention")
    client, client_log = t.start_client("negative", "--port", port, "--cookie-file",
                                        cookie, "--width", 1024, "--height", 700)
    try:
        t.h("wait", "Haiku Remote", 10, "sleep", 1500)
        t.reveal(1024)
        t.click(1024, MAIN, "keys")
        # Where Ctrl+Alt+Del would be; without it the row is one shorter.
        x, y = button_centre(1024, len(KEYS_WITH_SAS),
                             KEYS_WITH_SAS.index("Ctrl+Alt+Del"))
        t.h("click", x, y, "sleep", 800)
        text = mock_log.read_text()
        t.check("ack version=1 caps=0x5 " in text, "the mock negotiated more than"
                " string width | resync (0x5)")
        t.check("RP_SECURE_ATTENTION" not in text,
                "RP_SECURE_ATTENTION reached a server that never negotiated it")
        # The click found the button that is there instead: Cmd+` (key 0x11).
        t.check("<- RP_KEY_DOWN bytes=b'`' raw_char=96 key=17" in text,
                "the click at the absent button's place did not land on Cmd+`")
        t.check("secure attention" not in client_log.read_text(),
                "the client tried to send secure attention")
    finally:
        stop(client)
        stop(mock)


def library(t):
    client, _ = t.start_client("library", "--profiles", t.profiles())
    owner = None
    try:
        t.h("wait", "Connections", 10, "sleep", 800)
        t.check(t.h("clip-get") == "<none>", "CLIPBOARD had an owner before any copy")
        # The search field has the focus when the library opens.
        t.h("move", 500, 400, "sleep", 200, "type", "lab", "sleep", 200,
            "chord", "ctrl+a", "chord", "ctrl+c", "sleep", 400)
        copied = t.h("clip-get")
        t.check(copied == "lab", f"another X client read {copied!r} from CLIPBOARD"
                " after copy, expected 'lab'")

        owner = subprocess.Popen([str(t.helper), "clip-set", "pasted42", "10"],
                                 env=t.env(), stdout=subprocess.PIPE, text=True)
        t.check(owner.stdout.readline().strip() == "owning",
                "the other client could not take CLIPBOARD")
        t.h("chord", "ctrl+a", "chord", "ctrl+v", "sleep", 300,
            "chord", "ctrl+a", "chord", "ctrl+c", "sleep", 400)
        round_trip = t.h("clip-get")
        t.check(round_trip == "pasted42", f"paste then copy gave {round_trip!r},"
                " expected 'pasted42' -- the paste did not read the selection")

        # Re-layout: the New connection button is right-aligned 40 px from the
        # edge (it ends at x 984 in a 1024-wide window), so at 1200 wide its
        # face covers x 1155; and the new area is painted, not left black.
        t.check(t.h("pixel", "Connections", 1155, 40, check=False) == "",
                "the window is wider than 1024 before the resize")
        t.h("resize", "Connections", 1200, 780, "sleep", 1000)
        t.check(t.h("geom", "Connections") == "1200 780", "the resize did not apply")
        button = t.h("pixel", "Connections", 1155, 40)
        t.check(button == LIBRARY_ACCENT, f"pixel (1155,40) is {button} after the"
                f" resize, expected the New connection button's {LIBRARY_ACCENT}")
        corner = t.h("pixel", "Connections", 1195, 775)
        t.check(corner == LIBRARY_BACKGROUND, f"pixel (1195,775) is {corner} after"
                f" the resize, expected the library background {LIBRARY_BACKGROUND}")
    finally:
        stop(owner)
        stop(client)


# --- clipboard: large selections (ICCCM INCR) -------------------------------

MIB = 1 << 20
# XMaxRequestSize() on Xvfb without BIG-REQUESTS counted: 65535 4-byte units.
# No single property write -- and so no INCR chunk -- may be larger.
MAX_REQUEST_BYTES = 65535 * 4


def payload():
    """Exactly 1 MiB of UTF-8 in every encoded length (1, 2, 3 and 4 bytes),
    numbered so a dropped, repeated or reordered chunk changes it, and with no
    control characters (the library's fields drop those)."""
    unit = "{:07d}:é€中\U0001F600|"   # 21 bytes encoded
    text = "".join(unit.format(i) for i in range(MIB // 21))
    text += "t" * (MIB - len(text.encode()))
    data = text.encode()
    assert len(data) == MIB
    return text, data


def latin1_of(text):
    """What a STRING request must return for `text`: Latin-1, with '?' for each
    character Latin-1 cannot hold (the owner's choice ICCCM leaves open, and the
    one the client documents)."""
    return "".join(c if ord(c) < 256 else "?" for c in text).encode("latin-1")


def parse_got(line):
    """'got N bytes type T incr K max M' -> (N, T, K, M), or None."""
    parts = line.split()
    if len(parts) != 9 or parts[0] != "got":
        return None
    return int(parts[1]), parts[4], int(parts[6]), int(parts[8])


def echo_client(t, name, echoes):
    """The library, with `echoes` pastes scripted as clipboard:echo: each reads
    CLIPBOARD as a paste does (following INCR) and takes the selection back
    with exactly those bytes, leaving the search field alone -- a single-line
    field is not built to lay out a megabyte."""
    return t.start_client(name, "--profiles", t.profiles(), env_extra={
        "HAIKU_REMOTE_TEST_SCRIPT": ";".join(["clipboard:echo"] * echoes)})


def wait_for_line(path, line, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if line in path.read_text():
            return True
        time.sleep(0.05)
    return line in path.read_text()


def clipboard_incr(t):
    text, data = payload()
    source = t.work / "payload.txt"
    source.write_bytes(data)
    client, client_log = echo_client(t, "incr", 1)
    owner = None
    try:
        t.h("wait", "Connections", 10, "sleep", 800)
        # Receiving: another client owns 1 MiB and serves it through INCR in
        # 64 KiB chunks. A real Ctrl+V reads it.
        owner = subprocess.Popen([str(t.helper), "clip-serve-file", source, "60", "0"],
                                 env=t.env(), stdout=subprocess.PIPE, text=True)
        t.check(owner.stdout.readline().strip() == "owning",
                "the other client could not take CLIPBOARD")
        t.h("move", 500, 400, "sleep", 200, "chord", "ctrl+v")
        served = owner.stdout.readline().strip()
        t.check(served == f"served {MIB} bytes in 16 chunks",
                f"the owner reported {served!r}: the paste did not take all of an"
                " INCR transfer (1 MiB in 16 chunks of 64 KiB)")
        t.check(wait_for_line(client_log, f"e2e: clipboard echo read {MIB} bytes", 5),
                f"the paste did not read {MIB} bytes")
        # The client took the selection back: the other client lost it.
        lost = owner.stdout.readline().strip()
        t.check(lost == "lost ownership", f"the other client said {lost!r}, not that"
                " it lost CLIPBOARD to the client")

        # Serving: read the client's 1 MiB back, as UTF8_STRING and as STRING.
        back = t.work / "back.txt"
        got = parse_got(t.h("clip-get-file", "UTF8_STRING", back))
        t.check(got is not None and got[0] == MIB and got[1] == "UTF8_STRING",
                f"reading the copy back gave {got}, expected {MIB} bytes of"
                " UTF8_STRING")
        t.check(got is not None and got[2] >= 2,
                f"the copy came back in {got and got[2]} INCR chunks: 1 MiB is more"
                " than one request can carry, so it must be INCR")
        t.check(got is not None and 0 < got[3] <= MAX_REQUEST_BYTES,
                f"the largest chunk served was {got and got[3]} bytes, more than"
                f" the server's maximum request ({MAX_REQUEST_BYTES})")
        t.check(back.exists() and back.read_bytes() == data,
                "the 1 MiB UTF-8 selection did not round-trip byte for byte")

        latin = t.work / "back-latin1.txt"
        got = parse_got(t.h("clip-get-file", "STRING", latin))
        expected = latin1_of(text)
        t.check(got is not None and got[0] == len(expected) and got[1] == "STRING"
                and got[2] >= 2,
                f"the STRING request gave {got}, expected {len(expected)} bytes of"
                " STRING through INCR")
        t.check(latin.exists() and latin.read_bytes() == expected,
                "the STRING (Latin-1) form is not the selection with '?' for each"
                " character Latin-1 cannot hold")

        targets = t.h("clip-query", "TARGETS")
        t.check(targets.startswith("ATOM/32:") and {"TARGETS", "TIMESTAMP",
                "UTF8_STRING", "STRING"} <= set(targets.split()[1:]),
                f"TARGETS answered {targets!r}")
        stamp = t.h("clip-query", "TIMESTAMP")
        t.check(stamp.startswith("INTEGER/32: ") and stamp.split()[1].isdigit()
                and int(stamp.split()[1]) > 0,
                f"TIMESTAMP answered {stamp!r}, expected a non-zero INTEGER")
        t.check(client.poll() is None, "the client exited")
        t.check("clipboard:" not in client_log.read_text(),
                "the client reported a clipboard problem on a clean transfer")
    finally:
        stop(owner)
        stop(client)


def clipboard_vanish(t):
    _, data = payload()
    source = t.work / "payload.txt"
    source.write_bytes(data)
    client, client_log = echo_client(t, "vanish", 2)
    owners = []
    try:
        t.h("wait", "Connections", 10, "sleep", 800)
        # The owner dies after its first chunk: the paste must end at once and
        # read nothing -- not a truncated 64 KiB, and not after a long hang.
        owner = subprocess.Popen([str(t.helper), "clip-serve-file", source, "60", "1"],
                                 env=t.env(), stdout=subprocess.PIPE, text=True)
        owners.append(owner)
        t.check(owner.stdout.readline().strip() == "owning", "the owner did not start")
        t.h("move", 500, 400, "sleep", 200, "chord", "ctrl+v")
        t.check(owner.stdout.readline().strip() == "dying after 1 chunks",
                "the owner did not die mid-transfer")
        t.check(wait_for_line(client_log, "clipboard: the selection owner went away in"
                              " the middle of an INCR transfer; nothing pasted", 2.5),
                "the client did not notice, within 2.5 s, the owner going away")
        t.check(wait_for_line(client_log, "e2e: clipboard echo read 0 bytes", 2),
                "the paste whose owner vanished did not read as empty")

        # Now the client owns 1 MiB and its requestor dies after one chunk: the
        # client must stay up and serve the next request in full.
        owner = subprocess.Popen([str(t.helper), "clip-serve-file", source, "60", "0"],
                                 env=t.env(), stdout=subprocess.PIPE, text=True)
        owners.append(owner)
        t.check(owner.stdout.readline().strip() == "owning", "the owner did not start")
        t.h("chord", "ctrl+v")
        t.check(wait_for_line(client_log, "e2e: clipboard echo took the selection", 5),
                "the second paste did not take the selection back")
        t.check(t.h("clip-abort", "UTF8_STRING", 1) == "aborted after 1 chunks",
                "the requestor did not abort after its first chunk")
        t.check(wait_for_line(client_log, "clipboard: the requestor went away in the"
                              " middle of an INCR transfer; abandoned it", 3),
                "the client did not notice the requestor going away")
        t.h("sleep", 500)  # time for an X error to have arrived, and killed it
        t.check(client.poll() is None,
                f"the client exited ({client.returncode}) when a requestor vanished"
                " mid-transfer")
        back = t.work / "after-abort.txt"
        got = parse_got(t.h("clip-get-file", "UTF8_STRING", back))
        t.check(got is not None and got[0] == MIB and back.read_bytes() == data,
                f"after the aborted transfer a fresh one gave {got}, not the 1 MiB"
                " byte for byte")
    finally:
        for owner in owners:
            stop(owner)
        stop(client)


# --- the desktop scaled into its window ---------------------------------------

# tools/rp_mock_server.py --scale-fixture (SCALE_*): on DESKTOP_BG, a red block
# at desktop (0,0)-(39,19), a 10 px green band down the right edge, a 1 px
# yellow line at x 100, y 50-149.
RED, GREEN, YELLOW, BLACK = "c80000", "00a000", "ffcb00", "000000"


def scaling(t):
    mock, mock_log, cookie, port = t.start_mock("scale", "--scale-fixture")
    client, _ = t.start_client("scale", "--port", port, "--cookie-file", cookie,
                               "--width", 320, "--height", 200)
    try:
        t.h("wait", "Haiku Remote", 10, "sleep", 1200)
        t.expect_pixels("1:1", {(39, 19): RED, (40, 19): DESKTOP_BG,
                                (39, 20): DESKTOP_BG, (309, 100): DESKTOP_BG,
                                (310, 100): GREEN, (100, 50): YELLOW,
                                (101, 50): DESKTOP_BG})

        # 800x400 is wider than 320x200: scale 400/200 = 2, the desktop 640 px
        # wide at x (800 - 640) / 2 = 80, bars at the sides. Output x shows
        # desktop (x - 80) // 2, output y desktop y // 2.
        t.h("resize", "Haiku Remote", 800, 400, "sleep", 800)
        t.expect_pixels("800x400", {
            (79, 200): BLACK, (720, 200): BLACK, (799, 0): BLACK,     # the bars
            (80, 0): RED, (159, 39): RED,                             # desktop (39,19)
            (160, 39): DESKTOP_BG, (159, 40): DESKTOP_BG,             # (40,19), (39,20)
            (80, 399): DESKTOP_BG,                                    # (0,199): no flip
            (699, 100): DESKTOP_BG, (700, 100): GREEN, (719, 399): GREEN,
            (279, 150): DESKTOP_BG, (280, 150): YELLOW, (281, 150): YELLOW,
            (282, 150): DESKTOP_BG, (280, 99): DESKTOP_BG, (281, 299): YELLOW,
            (281, 300): DESKTOP_BG})

        # 500x300: scale 1.5, desktop 480 wide at x 10. Output x shows desktop
        # floor((2(x - 10) + 1) / 3), y floor((2y + 1) / 3) -- nearest, SDL's
        # default filter: the 1 px line stays 1 px, at x 160 alone.
        t.h("resize", "Haiku Remote", 500, 300, "sleep", 800)
        t.expect_pixels("500x300", {
            (9, 150): BLACK, (490, 150): BLACK,
            (10, 0): RED, (69, 29): RED, (70, 29): DESKTOP_BG, (69, 30): DESKTOP_BG,
            (159, 100): DESKTOP_BG, (160, 100): YELLOW, (161, 100): DESKTOP_BG,
            (160, 74): DESKTOP_BG, (160, 75): YELLOW, (160, 224): YELLOW,
            (160, 225): DESKTOP_BG,
            (474, 150): DESKTOP_BG, (475, 150): GREEN, (489, 150): GREEN})
        # The pointer maps back the way SDL maps it: minus the viewport's x,
        # divided by 1.5, truncated toward zero -- and, like SDL, unclamped.
        t.h("click", 171, 76, "sleep", 400)   # (161/1.5, 76/1.5) = (107.3, 50.7)
        t.h("click", 5, 10, "sleep", 400)     # (-5/1.5, 10/1.5) = (-3.3, 6.7)
        text = mock_log.read_text()
        t.check("<- RP_MOUSE_DOWN (107.0,50.0) buttons=1" in text,
                "a click at window (171,76) did not reach the desktop at (107,50)")
        t.check("<- RP_MOUSE_DOWN (-3.0,6.0) buttons=1" in text,
                "a click on the left bar at (5,10) did not map to (-3,6) as SDL's would")

        # 640x600 is taller: scale 2, the desktop 400 high at y 100, bars above
        # and below. Output y shows desktop (y - 100) // 2.
        t.h("resize", "Haiku Remote", 640, 600, "sleep", 800)
        t.expect_pixels("640x600", {
            (0, 99): BLACK, (0, 500): BLACK, (0, 100): RED, (79, 139): RED,
            (80, 139): DESKTOP_BG, (79, 140): DESKTOP_BG, (0, 499): DESKTOP_BG,
            (639, 499): GREEN, (619, 300): DESKTOP_BG, (620, 300): GREEN})
        t.h("click", 3, 105, "sleep", 400)    # (3/2, 5/2) = (1.5, 2.5)
        t.check("<- RP_MOUSE_DOWN (1.0,2.0) buttons=1" in mock_log.read_text(),
                "a click at window (3,105) did not reach the desktop at (1,2)")
        t.check("RP_UPDATE_DISPLAY_MODE 800x400" not in mock_log.read_text()
                and "RP_UPDATE_DISPLAY_MODE 640x600" not in mock_log.read_text(),
                "with Fit off a resize changed the remote's mode instead of scaling")
    finally:
        stop(client)
        stop(mock)


def scaling_linear(t):
    """SDL_RENDER_SCALE_QUALITY=linear, which SDL reads as its scale-quality
    hint, selects the linear filter in the X11 front end too."""
    mock, _, cookie, port = t.start_mock("linear", "--scale-fixture")
    env_extra = {"SDL_RENDER_SCALE_QUALITY": "linear"}
    client, _ = t.start_client("linear", "--port", port, "--cookie-file", cookie,
                               "--width", 320, "--height", 200, env_extra=env_extra)
    try:
        t.h("wait", "Haiku Remote", 10, "sleep", 1200)
        t.h("resize", "Haiku Remote", 800, 400, "sleep", 800)
        # Output (159,20) samples desktop x 256 * ((79.5) / 2 - 1/2) / 256 =
        # 39.25: red (39) and the background (40) at 192:64. y 20 samples 9.75,
        # rows 9 and 10, both red-or-background alike. Per channel
        # (red * 192 + bg * 64 + 128) >> 8 = (163, 26, 38); at (160,20), x 39.75,
        # 64:192 gives (88, 77, 114). Nearest would give red, then background.
        t.expect_pixels("linear 800x400", {(159, 20): "a31a26", (160, 20): "584d72",
                                           (100, 20): RED, (300, 20): DESKTOP_BG})
    finally:
        stop(client)
        stop(mock)


def fullscreen(t):
    """Full screen through _NET_WM_STATE_FULLSCREEN, asked of a window manager
    (the helper's minimal one), and the desktop scaled to the screen."""
    wm = subprocess.Popen([str(t.helper), "wm", "120"], env=t.env(),
                          stdout=subprocess.PIPE, text=True)
    mock = client = None
    try:
        t.check(wm.stdout.readline().strip() == "wm running",
                "the test window manager did not start")
        mock, mock_log, cookie, port = t.start_mock("full", "--scale-fixture")
        client, client_log = t.start_client("full", "--port", port, "--cookie-file",
                                            cookie, "--width", 1024, "--height", 640)
        t.h("wait", "Haiku Remote", 10, "sleep", 1200)
        t.check(t.h("geom", "Haiku Remote") == "1024 640", "not 1024x640 at first")
        t.reveal(1024)
        t.click(1024, MAIN, "fullscreen")
        t.check(wm.stdout.readline().strip() == "fullscreen action=1 source=1 -> on",
                "the client did not ask to ADD _NET_WM_STATE_FULLSCREEN")
        t.h("move", 800, 700, "sleep", 1800)   # leave the bar so it hides
        t.check(t.h("geom", "Haiku Remote") == "1600 1000",
                "the window did not cover the 1600x1000 screen")
        # 1600x1000 has the desktop's shape (1.6): the whole screen at 1.5625.
        # Output x shows desktop floor((2x + 1) * 1024 / 3200), y
        # floor((2y + 1) * 640 / 2000).
        t.expect_pixels("full screen", {
            (0, 0): RED, (61, 30): RED, (62, 30): DESKTOP_BG, (61, 31): DESKTOP_BG,
            (155, 78): DESKTOP_BG, (156, 78): YELLOW, (157, 78): YELLOW,
            (158, 78): DESKTOP_BG, (156, 77): DESKTOP_BG,
            (1583, 500): DESKTOP_BG, (1584, 500): GREEN, (1599, 999): GREEN,
            (132, 3): DESKTOP_BG, (1300, 3): DESKTOP_BG})        # the bar hidden
        t.check("RP_UPDATE_DISPLAY_MODE 1600x1000" not in mock_log.read_text(),
                "with Fit off, full screen changed the remote's mode")

        # The bar, scaled with the desktop, and its Full screen button again:
        # this time REMOVE, since the window manager now records the state.
        x, y = button_centre(1024, len(MAIN), MAIN.index("fullscreen"))
        t.h("move", round(x * 1.5625), 0, "sleep", 300,
            "move", round(x * 1.5625), 20, "sleep", 300)
        # The bar is scaled too: its panel above the buttons (desktop y 2) at
        # desktop x 84 (output 132) and x 832 (output 1300) -- which is beyond
        # the bar's right edge (941) only if it were drawn 1:1 over the scaled
        # desktop.
        t.expect_pixels("full screen with the bar", {(132, 3): BAR_PANEL,
                                                     (1300, 3): BAR_PANEL,
                                                     (60, 3): RED})
        t.h("click", round(x * 1.5625), round(y * 1.5625), "sleep", 600)
        t.check(wm.stdout.readline().strip() == "fullscreen action=0 source=1 -> off",
                "the client did not ask to REMOVE _NET_WM_STATE_FULLSCREEN")
        t.h("move", 500, 500, "sleep", 1800)
        t.check(t.h("geom", "Haiku Remote") == "1024 640",
                "leaving full screen did not restore 1024x640")
        t.expect_pixels("restored", {(39, 19): RED, (40, 19): DESKTOP_BG,
                                     (39, 20): DESKTOP_BG})
        t.check(client_log.read_text().count("requested (_NET_WM_STATE_FULLSCREEN)") == 2,
                "the client did not log two full-screen requests")
    finally:
        stop(client)
        stop(mock)
        if wm.poll() is None:
            wm.terminate()
            wm.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--x11", required=True, help="build/haiku-remote-x11")
    ap.add_argument("--helper", required=True, help="build/x11_test_helper")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--case", action="append", default=[], metavar="TEXT",
                    help="run only the cases whose name contains TEXT (repeatable)")
    a = ap.parse_args()

    if shutil.which("Xvfb") is None:
        print("SKIP: x11 window tests (no Xvfb on PATH); THEY DID NOT RUN")
        return 0
    work = Path(tempfile.mkdtemp(prefix="haiku-remote-x11-e2e-"))
    xvfb = None
    display = None
    for number in range(121, 150):
        if display_in_use(number):
            continue
        xvfb = subprocess.Popen(["Xvfb", f":{number}", "-screen", "0", "1600x1000x24",
                                 "-nolisten", "tcp"],
                                stdout=open(work / "xvfb.log", "w"),
                                stderr=subprocess.STDOUT)
        # Up once a client can open it (a socket file alone may be a stale one
        # that this server is about to replace).
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and xvfb.poll() is None:
            if subprocess.run([a.helper], capture_output=True,
                              env={"DISPLAY": f":{number}"}).returncode == 0:
                display = f":{number}"
                break
            time.sleep(0.05)
        if display:
            break
        stop_server(xvfb)
    if display is None:
        print("SKIP: x11 window tests (Xvfb did not come up); THEY DID NOT RUN")
        return 0

    probe = subprocess.run([a.helper, "move", "1", "1"], capture_output=True,
                           env={"DISPLAY": display})
    if probe.returncode == 77:
        stop_server(xvfb)
        print("SKIP: x11 window tests (no XTest: libXtst.so.6 is not installed);"
              " THEY DID NOT RUN")
        return 0

    failed = 0
    cases = (("toolbar", toolbar),
             ("negative control: no RP_CAP_SECURE_ATTENTION", negative),
             ("library", library),
             ("clipboard: 1 MiB through INCR, both ways", clipboard_incr),
             ("clipboard: owner and requestor vanishing mid-transfer",
              clipboard_vanish),
             ("scaling: letterbox, nearest, pointer mapping", scaling),
             ("scaling: SDL_RENDER_SCALE_QUALITY=linear", scaling_linear),
             ("full screen: _NET_WM_STATE_FULLSCREEN", fullscreen))
    if a.case:
        cases = tuple(c for c in cases if any(text in c[0] for text in a.case))
    for name, case in cases:
        t = Harness(Path(a.x11), Path(a.helper), work, display)
        try:
            case(t)
        except Exception as error:  # a harness fault fails the case, loudly
            t.failures.append(f"{type(error).__name__}: {error}")
        if t.failures:
            failed += 1
            print(f"FAIL: x11 {name}")
            for f in t.failures:
                print(f"    {f}")
            a.keep = True
        else:
            print(f"PASS: x11 {name}")
    stop_server(xvfb)
    print(f"{'PASS' if not failed else 'FAIL'} - {len(cases) - failed} of {len(cases)}"
          " x11 window cases")
    if a.keep:
        print(f"logs in {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
