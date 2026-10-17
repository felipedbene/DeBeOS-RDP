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
        env = dict(__import__("os").environ)
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

    def start_client(self, name, *args):
        err = self.work / f"{name}-client.log"
        proc = subprocess.Popen([str(self.x11), *map(str, args)], env=self.env(),
                                stdout=subprocess.DEVNULL, stderr=open(err, "w"))
        return proc, err


def stop(proc):
    # SIGTERM first: Xvfb removes its /tmp/.X<n>-lock and socket on a clean
    # exit, and SIGKILL leaves both behind -- one leaked display per run, until
    # every display the harness may use is taken and it can only SKIP.
    if proc is not None and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def display_taken(number):
    """A display is taken only while the process its lock names is alive. A
    lock left by a dead Xvfb is stale, and Xvfb clears a stale lock itself."""
    lock = Path(f"/tmp/.X{number}-lock")
    try:
        pid = int(lock.read_text().strip())
    except FileNotFoundError:
        return Path(f"/tmp/.X11-unix/X{number}").exists()
    except (OSError, ValueError):
        return True
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


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
        t.check("ack version=1 caps=0x35" in text, "the mock did not acknowledge"
                " RP_CAP_SECURE_ATTENTION (caps 0x35 = string width | resync |"
                " secure attention | font identity)")
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
        t.check("ack version=1 caps=0x25 " in text, "the mock negotiated more than"
                " string width | resync | font identity (0x25)")
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
    profiles = t.work / "connections.json"
    profiles.write_text('{"version":2,"profiles":[{"id":"p1","name":"Lab box",'
                        '"mode":"direct","host":"127.0.0.1","remotePort":1,'
                        '"width":1024,"height":700,"autoReconnect":false}]}')
    client, _ = t.start_client("library", "--profiles", profiles)
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--x11", required=True, help="build/haiku-remote-x11")
    ap.add_argument("--helper", required=True, help="build/x11_test_helper")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()

    if shutil.which("Xvfb") is None:
        print("SKIP: x11 window tests (no Xvfb on PATH); THEY DID NOT RUN")
        return 0
    work = Path(tempfile.mkdtemp(prefix="haiku-remote-x11-e2e-"))
    xvfb = None
    display = None
    for number in range(121, 150):
        if display_taken(number):
            continue
        xvfb = subprocess.Popen(["Xvfb", f":{number}", "-screen", "0", "1600x1000x24",
                                 "-nolisten", "tcp"],
                                stdout=open(work / "xvfb.log", "w"),
                                stderr=subprocess.STDOUT)
        # Ready once the probe connects. Not "the socket exists": a dead Xvfb's
        # socket may still be there, and is what Xvfb replaces on startup.
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and xvfb.poll() is None:
            if (Path(f"/tmp/.X11-unix/X{number}").exists()
                    and subprocess.run([a.helper, "move", "1", "1"],
                                       capture_output=True,
                                       env={"DISPLAY": f":{number}"}
                                       ).returncode in (0, 77)):
                display = f":{number}"
                break
            time.sleep(0.1)
        if display:
            break
        stop(xvfb)
    if display is None:
        print("SKIP: x11 window tests (Xvfb did not come up); THEY DID NOT RUN")
        return 0

    probe = subprocess.run([a.helper, "move", "1", "1"], capture_output=True,
                           env={"DISPLAY": display})
    if probe.returncode == 77:
        stop(xvfb)
        print("SKIP: x11 window tests (no XTest: libXtst.so.6 is not installed);"
              " THEY DID NOT RUN")
        return 0

    failed = 0
    for name, case in (("toolbar", toolbar), ("negative control: no"
                       " RP_CAP_SECURE_ATTENTION", negative), ("library", library)):
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
    stop(xvfb)
    print(f"{'PASS' if not failed else 'FAIL'} - {3 - failed} of 3 x11 window cases")
    if a.keep:
        print(f"logs in {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
