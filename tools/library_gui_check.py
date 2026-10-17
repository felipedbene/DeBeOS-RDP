#!/usr/bin/env python3
"""Headless check of the SDL connection library: caret, selection, clipboard,
minimum window size and editor scrolling, read back from the RENDERER.

Runs build/haiku-remote-gui with SDL_VIDEODRIVER=offscreen and an empty,
private connection library, drives it with HAIKU_REMOTE_LIBRARY_EVENTS
(synthetic SDL events that go through sdl_main.cpp's real translation), and
asserts exact pixel values in the snapshots it takes. Expected positions come
from the layout's documented geometry, or from a second snapshot reached by a
different route (the caret after typing N characters vs. the end of a
selection of N characters) -- never from the code that draws them.

    python3 tools/library_gui_check.py build/haiku-remote-gui

Exit 0 when every check holds, 1 when one fails, 77 (SKIP) when this SDL has
no offscreen video driver.
"""

import os
import struct
import subprocess
import sys
import tempfile
import zlib

ACCENT = (37, 89, 189)
FIELD = (255, 255, 255)
BACKGROUND = (246, 243, 238)
BORDER_STRONG = (176, 167, 155)

checks = 0
failures = 0


def check(ok, what):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print("FAIL: " + what, file=sys.stderr)


def read_png(path):
    """Decode an 8-bit RGBA, non-interlaced PNG (what png_writer emits)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(path + ": not a PNG")
    pos, idat, width, height = 8, b"", 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or colour != 6 or interlace != 0:
                raise ValueError(path + ": expected 8-bit RGBA, not interlaced")
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    raw = zlib.decompress(idat)
    stride = width * 4
    rows, prev, at = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if kind == 1:
                line[i] = (line[i] + a) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + b) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        rows.append(bytes(line))
        prev = line
    return width, height, rows


class Shot:
    def __init__(self, path):
        self.name = os.path.basename(path)
        self.width, self.height, self.rows = read_png(path)

    def px(self, x, y):
        r = self.rows[y]
        return (r[4 * x], r[4 * x + 1], r[4 * x + 2])

    def expect(self, x, y, colour, what):
        got = self.px(x, y)
        check(got == colour, "%s: %s (pixel %d,%d is %s, want %s)"
              % (self.name, what, x, y, got, colour))

    def accent_columns(self, y, x0, x1):
        return [x for x in range(x0, x1) if self.px(x, y) == ACCENT]


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    gui = os.path.abspath(sys.argv[1])
    command = "gui" if sys.platform == "darwin" else "ctrl"
    with tempfile.TemporaryDirectory(prefix="library-gui-check-") as tmp:
        shot = lambda name: os.path.join(tmp, name + ".png")
        steps = [
            "snap:" + shot("a_empty"),
            "text:MMMM", "snap:" + shot("b_four"),
            "key:left", "snap:" + shot("c_three"),
            "key:shift+home", "snap:" + shot("d_select_three"),
            "key:end", "key:shift+left", "snap:" + shot("e_select_last"),
            "key:%s+a" % command, "key:%s+x" % command, "snap:" + shot("f_cut"),
            "key:%s+v" % command, "key:%s+v" % command, "snap:" + shot("g_pasted"),
            "key:%s+a" % command, "text:MMMMMMMM", "snap:" + shot("h_typed"),
            "clip:MMM", "key:%s+a" % command, "key:%s+v" % command,
            "snap:" + shot("l_system_clip"),
            "key:escape",
            "down:900,48", "up:900,48",            # + New connection
            "size:800x480", "snap:" + shot("i_editor_min"),
            "wheel:-1", "snap:" + shot("j_wheel"),
            "wheel:5", "key:tab", "key:tab", "key:tab", "snap:" + shot("k_tab"),
            "quit",
        ]
        env = dict(os.environ)
        env.update({
            "HOME": tmp,
            "XDG_CONFIG_HOME": os.path.join(tmp, "config"),
            "APPDATA": os.path.join(tmp, "config"),
            "SDL_VIDEODRIVER": "offscreen",
            "HAIKU_REMOTE_LIBRARY_EVENTS": ";".join(steps),
        })
        env.pop("HAIKU_REMOTE_LIBRARY_SNAPSHOT", None)
        run = subprocess.run([gui], env=env, capture_output=True, text=True, timeout=60)
        if "offscreen" in run.stderr and "not available" in run.stderr:
            print("SKIP: this SDL has no offscreen video driver")
            return 77
        if run.returncode != 0:
            print(run.stderr, file=sys.stderr)
            print("FAIL: haiku-remote-gui exited %d" % run.returncode, file=sys.stderr)
            return 1

        # 1024x700, scale 1. The search box is at (32, 188) 960x36 with 12 of
        # text padding, so its text origin is x 44; row 197 is inside the
        # caret/selection band (box top + 7 ..), above every glyph.
        y, x0, x1 = 197, 34, 990
        a = Shot(shot("a_empty"))
        check((a.width, a.height) == (1024, 700), "the library opens at 1024x700")
        check(a.accent_columns(y, x0, x1) == [44],
              "a_empty: the focused empty search shows one caret column, at x 44 (%s)"
              % a.accent_columns(y, x0, x1))
        b = Shot(shot("b_four"))
        c = Shot(shot("c_three"))
        cols_b, cols_c = b.accent_columns(y, x0, x1), c.accent_columns(y, x0, x1)
        check(len(cols_b) == 1 and len(cols_c) == 1 and cols_c[0] < cols_b[0],
              "after typing MMMM and Left, one caret each, Left of the first (%s, %s)"
              % (cols_b, cols_c))
        if len(cols_b) == 1 and len(cols_c) == 1:
            x4, x3 = cols_b[0], cols_c[0]
            d = Shot(shot("d_select_three"))
            check(d.accent_columns(y, x0, x1) == list(range(44, x3)),
                  "d: Shift-Home selects exactly from the origin to the caret (44..%d)" % x3)
            d.expect(43, y, FIELD, "no selection before the text origin")
            e = Shot(shot("e_select_last"))
            check(e.accent_columns(y, x0, x1) == list(range(x3, x4)),
                  "e: End, Shift-Left selects exactly the last M (%d..%d), got %s"
                  % (x3, x4, e.accent_columns(y, x0, x1)))
        f = Shot(shot("f_cut"))
        check(f.accent_columns(y, x0, x1) == [44],
              "f: Select All + Cut empties the field (caret back at 44)")
        g = Shot(shot("g_pasted"))
        h = Shot(shot("h_typed"))
        cols_g, cols_h = g.accent_columns(y, x0, x1), h.accent_columns(y, x0, x1)
        check(len(cols_g) == 1 and cols_g == cols_h and cols_g != [44],
              "g/h: pasting the cut MMMM twice puts the caret where typing 8 Ms does (%s vs %s)"
              % (cols_g, cols_h))

        l = Shot(shot("l_system_clip"))
        if len(cols_c) == 1:
            check(l.accent_columns(y, x0, x1) == cols_c,
                  "l: paste reads the system clipboard: MMM put there by another app "
                  "leaves the caret where three typed Ms did (%s vs %s)"
                  % (l.accent_columns(y, x0, x1), cols_c))

        # The editor at the minimum window. Host (row 1) is focused at y 200
        # with a 2px accent edge at x 32; the form scrolls 48 per notch.
        i = Shot(shot("i_editor_min"))
        check((i.width, i.height) == (800, 480),
              "the window shrinks to 800x480 (got %dx%d)" % (i.width, i.height))
        if (i.width, i.height) == (800, 480):
            i.expect(32, 216, ACCENT, "Host's focused edge, unscrolled")
            i.expect(32, 168, BACKGROUND, "between Name and Host, unscrolled")
            i.expect(34, 429, ACCENT, "the Save button in the action bar")
            j = Shot(shot("j_wheel"))
            j.expect(32, 168, ACCENT, "one notch down, Host's edge is at 152")
            j.expect(42, 152, ACCENT, "Host's top edge is at exactly 200 - 48")
            j.expect(42, 151, BACKGROUND, "...and not a row higher")
            j.expect(32, 238, BORDER_STRONG, "SSH user's edge is at 222")
            j.expect(34, 429, ACCENT, "the action bar did not scroll")
            k = Shot(shot("k_tab"))
            # Tab x3 from Host reaches Remote port (row 4, y 410), scrolled 62
            # so its error line clears the band's bottom at 400.
            k.expect(32, 410 - 62 + 16, ACCENT, "Tab brought Remote port into view at 348")
            k.expect(42, 200 - 62, BORDER_STRONG, "Host's top edge is at exactly 200 - 62")
            k.expect(42, 200 - 62 - 1, BACKGROUND, "...and not a row higher")

    if failures:
        print("%d of %d library GUI checks FAILED" % (failures, checks), file=sys.stderr)
        return 1
    print("PASS - %d library GUI checks (SDL offscreen, renderer readback)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
