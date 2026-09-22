#!/usr/bin/env python3
"""
rp_mock_server -- stand in for Haiku's app_server remote interface.

Speaks the server side of the RP_* protocol documented in PROTOCOL.md so the
macOS client can be built and validated without an EC2 instance in the loop.
It deliberately exercises the parts of the protocol that are easy to get wrong:

  * the handshake, in the real order, and the fact that nothing is drawn until
    the client sends RP_UPDATE_DISPLAY_MODE
  * token-addressed vs. session-level messages (§5.1)
  * a trailing field no client reads, so skip-by-declared-length is required (§2.1)
  * messages split across writes and several messages batched into one write (§2.2)
  * BRect's inclusive edges (§3.3)
  * bitmaps in B_RGB32 / B_RGBA32 / B_CMAP8 / B_GRAY8 (§6.3)
  * the blocking round-trips: RP_STRING_WIDTH and RP_DRAW_STRING expect replies,
    and this server reports how long the client took (§7.1)
  * incremental updates, for Phase 3
  * input events echoed back as cursor moves, for Phase 4

Usage:
    ./rp_mock_server.py --port 10900
    ./rp_mock_server.py --port 10900 --latency-ms 150   # simulate travel wifi
"""

import argparse
import math
import socket
import struct
import sys
import threading
import time

HEADER = 6

RP_INIT_CONNECTION = 1
RP_UPDATE_DISPLAY_MODE = 2
RP_CLOSE_CONNECTION = 3
RP_GET_SYSTEM_PALETTE = 4
RP_GET_SYSTEM_PALETTE_RESULT = 5
RP_HELLO = 6
RP_HELLO_ACK = 7

# URP/1 handshake values (RemoteMessage.h). This mock answers string-width
# queries, so it negotiates RP_CAP_STRING_WIDTH_REPLY when the client offers it.
RP_PROTOCOL_VERSION = 1
RP_CAP_STRING_WIDTH_REPLY = 1 << 0
RP_CREATE_STATE = 20
RP_DELETE_STATE = 21
RP_INVALIDATE_RECT = 24
RP_SET_OFFSETS = 40
RP_SET_HIGH_COLOR = 41
RP_SET_LOW_COLOR = 42
RP_SET_PEN_SIZE = 43
RP_SET_STROKE_MODE = 44
RP_SET_BLENDING_MODE = 45
RP_SET_PATTERN = 46
RP_SET_DRAWING_MODE = 47
RP_SET_FONT = 48
RP_SET_TRANSFORM = 49
RP_CONSTRAIN_CLIPPING_REGION = 60
RP_COPY_RECT_NO_CLIPPING = 61
RP_INVERT_RECT = 62
RP_DRAW_BITMAP = 63
RP_DRAW_BITMAP_RECTS = 64
RP_STROKE_ARC = 80
RP_STROKE_ELLIPSE = 82
RP_STROKE_RECT = 84
RP_STROKE_SHAPE = 86
RP_STROKE_LINE = 88
RP_FILL_ARC = 100
RP_FILL_RECT = 104
RP_FILL_ELLIPSE = 102
RP_FILL_POLYGON = 103
RP_FILL_SHAPE = 106
RP_FILL_REGION = 108
RP_FILL_ELLIPSE_GRADIENT = 122
RP_FILL_RECT_GRADIENT = 124
RP_STROKE_POINT_COLOR = 140
RP_STROKE_LINE_1PX_COLOR = 141
RP_STROKE_RECT_1PX_COLOR = 142
RP_FILL_RECT_COLOR = 160
RP_FILL_REGION_COLOR_NO_CLIPPING = 161
RP_DRAW_STRING = 180
RP_DRAW_STRING_RESULT = 182
RP_STRING_WIDTH = 183
RP_STRING_WIDTH_RESULT = 184
RP_READ_BITMAP = 185
RP_READ_BITMAP_RESULT = 186
RP_SET_CURSOR = 200
RP_SET_CURSOR_VISIBLE = 201
RP_MOVE_CURSOR_TO = 202
RP_MOUSE_MOVED = 220
RP_MOUSE_DOWN = 221
RP_MOUSE_UP = 222
RP_MOUSE_WHEEL_CHANGED = 223
RP_KEY_DOWN = 240
RP_KEY_UP = 241
RP_MODIFIERS_CHANGED = 244

# BGradient::Type
GRADIENT_LINEAR = 0
GRADIENT_RADIAL = 1
GRADIENT_RADIAL_FOCUS = 2
GRADIENT_DIAMOND = 3
GRADIENT_CONIC = 4

# BShape op flags (Shape.cpp). The low 24 bits are a point count.
OP_MOVETO = 0x80000000
OP_CLOSE = 0x40000000
OP_BEZIERTO = 0x20000000
OP_LINETO = 0x10000000
OP_SMALL_ARC_TO_CCW = 0x08000000
OP_SMALL_ARC_TO_CW = 0x04000000
OP_LARGE_ARC_TO_CCW = 0x02000000
OP_LARGE_ARC_TO_CW = 0x01000000

B_RGB32 = 0x0008
B_RGBA32 = 0x2008
B_CMAP8 = 0x0004
B_GRAY8 = 0x0002

TOKEN = 1


def msg(code, body=b""):
    return struct.pack("<HI", code, HEADER + len(body)) + body


def rect(l, t, r, b):
    return struct.pack("<4f", l, t, r, b)


def point(x, y):
    return struct.pack("<2f", x, y)


def color(r, g, b, a=255):
    return bytes((r, g, b, a))


def tok(t=TOKEN):
    return struct.pack("<i", t)


def gradient(kind, stops, center=(0, 0), start=(0, 0), end=(0, 0),
             focal=(0, 0), radius=0.0, angle=0.0):
    """A BGradient as RemoteMessage::Add(BGradient) writes it.

    Stop offsets are 0-255, not 0-1: Painter divides by 255 when building its
    colour LUT (Painter.cpp:2186).
    """
    out = struct.pack("<I", kind)
    if kind == GRADIENT_LINEAR:
        out += point(*start) + point(*end)
    elif kind == GRADIENT_RADIAL:
        out += point(*center) + struct.pack("<f", radius)
    elif kind == GRADIENT_RADIAL_FOCUS:
        out += point(*center) + point(*focal) + struct.pack("<f", radius)
    elif kind == GRADIENT_DIAMOND:
        out += point(*center)
    elif kind == GRADIENT_CONIC:
        out += point(*center) + struct.pack("<f", angle)
    out += struct.pack("<i", len(stops))
    for (c, off) in stops:
        out += color(*c) + struct.pack("<f", off)
    return out


def shape(ops, points, offset=(0.0, 0.0), scale=1.0):
    """A BShape as RemoteMessage writes it: ops, then points, then offset+scale."""
    out = struct.pack("<i", len(ops))
    for o in ops:
        out += struct.pack("<I", o)
    out += struct.pack("<i", len(points))
    for p in points:
        out += point(*p)
    out += point(*offset) + struct.pack("<f", scale)
    return out


def region(rects):
    return struct.pack("<i", len(rects)) + b"".join(rect(*r) for r in rects)


def font_record(size=12.0, face=0, spacing=0, family=3, style=5):
    return (bytes((0, 0))                       # direction, encoding
            + struct.pack("<I", 0)              # flags
            + bytes((spacing,))
            + struct.pack("<4f", 0.0, 0.0, 0.0, size)   # shear, rot, fbw, size
            + struct.pack("<H", face)
            + struct.pack("<I", (family << 16) | style))


def bitmap_rgb32(w, h, fn, colorspace=B_RGB32, minimal=False):
    """Build a bitmap record. `fn(x, y) -> (r, g, b, a)`.

    Haiku's B_RGB32/B_RGBA32 are byte-order BGRA, so emit B, G, R, A -- this is
    the layout a macOS client can hand straight to CoreGraphics with
    .byteOrder32Little (PROTOCOL.md §6.3).
    """
    bpr = w * 4
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b, a = fn(x, y)
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    bits = b"".join(rows)
    out = struct.pack("<iii", w, h, bpr)
    if not minimal:
        out += struct.pack("<II", colorspace, 0)
    return out + struct.pack("<I", len(bits)) + bits


def bitmap_gray8(w, h, fn):
    """B_GRAY8, with bytesPerRow padded past width to exercise row padding."""
    bpr = (w + 3) & ~3
    rows = []
    for y in range(h):
        row = bytearray(bytes(fn(x, y) for x in range(w)))
        row += bytes(bpr - w)
        rows.append(bytes(row))
    bits = b"".join(rows)
    return (struct.pack("<iii", w, h, bpr)
            + struct.pack("<II", B_GRAY8, 0)
            + struct.pack("<I", len(bits)) + bits)


def bitmap_cmap8(w, h, fn):
    bpr = (w + 3) & ~3
    rows = []
    for y in range(h):
        row = bytearray(bytes(fn(x, y) for x in range(w)))
        row += bytes(bpr - w)
        rows.append(bytes(row))
    bits = b"".join(rows)
    return (struct.pack("<iii", w, h, bpr)
            + struct.pack("<II", B_CMAP8, 0)
            + struct.pack("<I", len(bits)) + bits)


def cursor_bitmap():
    """A 16x16 arrow, opaque where drawn and fully transparent elsewhere."""
    def px(x, y):
        if x == 0 and y == 0:
            return (0, 0, 0, 255)
        if y < 12 and x < 12 and x <= y:
            edge = (x == 0 or x == y or y == 11)
            return (0, 0, 0, 255) if edge else (255, 255, 255, 255)
        return (0, 0, 0, 0)
    return bitmap_rgb32(16, 16, px, colorspace=B_RGBA32)


# Haiku's default 256-entry system palette is not reproduced faithfully here; a
# gentle ramp is enough to prove the client wires RP_GET_SYSTEM_PALETTE_RESULT
# into its B_CMAP8 decoder.
def system_palette():
    out = struct.pack("<I", 256)
    for i in range(256):
        r = (i * 7) % 256
        g = (i * 5) % 256
        b = (i * 3) % 256
        out += color(r, g, b, 255)
    return out


class Reader:
    def __init__(self, buf):
        self.b = buf
        self.p = 0

    def i32(self):
        v = struct.unpack_from("<i", self.b, self.p)[0]; self.p += 4; return v

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.p)[0]; self.p += 4; return v

    def f32(self):
        v = struct.unpack_from("<f", self.b, self.p)[0]; self.p += 4; return v

    def u8(self):
        v = self.b[self.p]; self.p += 1; return v

    def left(self):
        return len(self.b) - self.p


class Session:
    def __init__(self, conn, addr, latency_ms, verbose, torture=False):
        self.conn = conn
        self.addr = addr
        self.latency = latency_ms / 1000.0
        self.verbose = verbose
        self.torture = torture
        self.pending_read_back = False
        self.read_backs = 0
        self.width = 0
        self.height = 0
        self.buf = bytearray()
        self.started = False
        self.lock = threading.Lock()
        self.pending_width = {}
        self.pending_draw = {}
        self.stats = {"width_rtt": [], "draw_rtt": [], "events": 0}
        self.cursor = (0.0, 0.0)
        self.closing = False

    # -- send helpers ------------------------------------------------------

    def send(self, data):
        if self.latency:
            time.sleep(self.latency)
        with self.lock:
            try:
                self.conn.sendall(data)
            except OSError:
                self.closing = True

    def send_split(self, data, chunk):
        """Send deliberately fragmented, to exercise client reassembly."""
        if self.latency:
            time.sleep(self.latency)
        with self.lock:
            try:
                for i in range(0, len(data), chunk):
                    self.conn.sendall(data[i:i + chunk])
                    time.sleep(0.002)
            except OSError:
                self.closing = True

    # -- scene -------------------------------------------------------------

    def initial_scene(self):
        w, h = self.width, self.height
        out = bytearray()

        out += msg(RP_CREATE_STATE, tok())
        # Desktop background, drawn session-level with no clipping, as Haiku does.
        out += msg(RP_FILL_REGION_COLOR_NO_CLIPPING,
                   region([(0, 0, w - 1, h - 1)]) + color(51, 102, 152))
        out += msg(RP_CONSTRAIN_CLIPPING_REGION,
                   tok() + region([(0, 0, w - 1, h - 1)]))
        out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 1))  # B_OP_OVER
        out += msg(RP_SET_PATTERN, tok() + bytes([0xFF] * 8))
        out += msg(RP_SET_TRANSFORM, tok() + b"\x01")                  # identity
        out += msg(RP_SET_PEN_SIZE, tok() + struct.pack("<f", 1.0))

        # A window: yellow tab, grey body, dark border -- recognisably Haiku.
        win = (60, 60, 60 + 420, 60 + 260)
        out += msg(RP_FILL_RECT_COLOR, tok() + rect(win[0], win[1] - 22,
                                                    win[0] + 150, win[1] - 1)
                   + color(255, 203, 0))
        out += msg(RP_STROKE_RECT_1PX_COLOR, tok() + rect(win[0], win[1] - 22,
                                                          win[0] + 150, win[1] - 1)
                   + color(0, 0, 0))
        out += msg(RP_FILL_RECT_COLOR, tok() + rect(*win) + color(216, 216, 216))
        out += msg(RP_STROKE_RECT_1PX_COLOR, tok() + rect(*win) + color(0, 0, 0))

        # Title text. Note the trailing hasDelta bool: a client that advances by
        # fields consumed rather than declared length desyncs right here.
        out += msg(RP_SET_FONT, tok() + font_record(size=12.0, face=0x0020))
        out += msg(RP_SET_HIGH_COLOR, tok() + color(0, 0, 0))
        title = "MockTracker".encode()
        out += msg(RP_DRAW_STRING, tok() + point(win[0] + 8, win[1] - 7)
                   + struct.pack("<I", len(title)) + title + b"\x00")

        # Body text in the default face.
        out += msg(RP_SET_FONT, tok() + font_record(size=12.0))
        for i, line in enumerate([
                "Phase 2: this frame proves handshake + decode + render.",
                "BRect edges are inclusive: the box below is 200x40, not 199x39.",
                "Bitmaps below are B_RGB32, B_RGBA32, B_GRAY8 and B_CMAP8.",
        ]):
            bs = line.encode()
            out += msg(RP_DRAW_STRING,
                       tok() + point(win[0] + 10, win[1] + 24 + i * 18)
                       + struct.pack("<I", len(bs)) + bs + b"\x00")

        # An exactly-200x40 box: left=x, right=x+199 inclusive.
        bx, by = win[0] + 10, win[1] + 90
        out += msg(RP_STROKE_RECT_1PX_COLOR,
                   tok() + rect(bx, by, bx + 199, by + 39) + color(200, 0, 0))

        # Bitmaps, one per colour space.
        def ramp(x, y):
            return (x * 16 % 256, y * 16 % 256, 128, 255)

        bm_y = win[1] + 140
        out += msg(RP_DRAW_BITMAP,
                   tok() + rect(0, 0, 15, 15) + rect(bx, bm_y, bx + 31, bm_y + 31)
                   + struct.pack("<I", 0) + bitmap_rgb32(16, 16, ramp))

        def translucent(x, y):
            return (255, 0, 0, 255 if (x + y) % 2 == 0 else 60)

        out += msg(RP_DRAW_BITMAP,
                   tok() + rect(0, 0, 15, 15)
                   + rect(bx + 40, bm_y, bx + 40 + 31, bm_y + 31)
                   + struct.pack("<I", 0)
                   + bitmap_rgb32(16, 16, translucent, colorspace=B_RGBA32))

        out += msg(RP_DRAW_BITMAP,
                   tok() + rect(0, 0, 15, 15)
                   + rect(bx + 80, bm_y, bx + 80 + 31, bm_y + 31)
                   + struct.pack("<I", 0)
                   + bitmap_gray8(16, 16, lambda x, y: (x * 16) % 256))

        out += msg(RP_DRAW_BITMAP,
                   tok() + rect(0, 0, 15, 15)
                   + rect(bx + 120, bm_y, bx + 120 + 31, bm_y + 31)
                   + struct.pack("<I", 0)
                   + bitmap_cmap8(16, 16, lambda x, y: (x + y * 16) % 256))

        # DRAW_BITMAP_RECTS: colour space once, then minimal bitmaps per rect.
        rects_body = struct.pack("<III", 0, B_RGB32, 0) + struct.pack("<i", 2)
        for i in range(2):
            rx = bx + 170 + i * 34
            rects_body += rect(rx, bm_y, rx + 31, bm_y + 31)
            rects_body += bitmap_rgb32(
                16, 16, lambda x, y, i=i: (0, 200 if i else 100, 255, 255),
                minimal=True)
        out += msg(RP_DRAW_BITMAP_RECTS, tok() + rects_body)

        # Some vector ops.
        out += msg(RP_SET_HIGH_COLOR, tok() + color(255, 255, 255))
        out += msg(RP_FILL_ELLIPSE, tok() + rect(bx + 250, bm_y, bx + 250 + 31,
                                                 bm_y + 31))
        out += msg(RP_STROKE_LINE_1PX_COLOR,
                   tok() + point(bx, by + 60) + point(bx + 380, by + 60)
                   + color(0, 128, 0))
        for i in range(8):
            out += msg(RP_STROKE_POINT_COLOR,
                       tok() + point(bx + 300 + i * 3, bm_y + 10)
                       + color(255, 0, 255))
        return bytes(out)

    def torture_scene(self):
        """Exercise the parts of the protocol the baseline scene never reaches.

        The baseline scene is a realistic desktop, which is exactly why it misses
        so much: real Haiku UI is almost entirely B_OP_COPY/B_OP_OVER solid fills.
        The conditional drawing modes, stipple phase, exotic gradients and shape
        arcs only show up in drawing applications, so they need a scene of their
        own or they go untested.
        """
        out = bytearray()
        label_y = 360

        out += msg(RP_SET_FONT, tok() + font_record(size=10.0))
        out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 1))  # B_OP_OVER
        out += msg(RP_SET_HIGH_COLOR, tok() + color(255, 255, 255))

        def label(x, y, text):
            bs = text.encode()
            return msg(RP_DRAW_STRING, tok() + point(x, y)
                       + struct.pack("<I", len(bs)) + bs + b"\x00")

        # -- every drawing mode, over a fixed base colour -------------------
        # Each cell is primed to (200,100,40) with an explicit-colour fill (which
        # bypasses the drawing mode), then filled again through the mode under
        # test with source (100,150,40).
        out += label(20, label_y, "drawing modes over base(200,100,40) "
                                  "src(100,150,40):")
        modes = [(0, "copy"), (1, "over"), (2, "erase"), (3, "invert"),
                 (4, "add"), (5, "sub"), (6, "blend"), (7, "min"),
                 (8, "max"), (9, "select"), (10, "alpha")]
        cw, ch, y0 = 70, 44, label_y + 10
        for i, (mode, name) in enumerate(modes):
            x = 20 + i * (cw + 4)
            out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 0))
            out += msg(RP_SET_PATTERN, tok() + bytes([0xFF] * 8))
            out += msg(RP_FILL_RECT_COLOR,
                       tok() + rect(x, y0, x + cw - 1, y0 + ch - 1)
                       + color(200, 100, 40))
            out += msg(RP_SET_HIGH_COLOR, tok() + color(100, 150, 40))
            out += msg(RP_SET_LOW_COLOR, tok() + color(0, 0, 255))
            out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", mode))
            out += msg(RP_FILL_RECT,
                       tok() + rect(x + 8, y0 + 8, x + cw - 9, y0 + ch - 9))
            out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 1))
            out += msg(RP_SET_HIGH_COLOR, tok() + color(255, 255, 255))
            out += label(x + 4, y0 + ch + 12, name)

        # -- stipple patterns, including a shifted phase --------------------
        y1 = y0 + ch + 26
        out += label(20, y1, "stipples (last two are the same pattern, "
                             "offset by 0 and 4):")
        patterns = [
            ("50%", [0xAA, 0x55] * 4, 0),
            ("rows", [0xFF, 0x00] * 4, 0),
            ("diag", [0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01], 0),
            ("cols", [0xCC] * 8, 0),
            ("cols+0", [0xF0] * 8, 0),
            ("cols+4", [0xF0] * 8, 4),
        ]
        for i, (name, bits, off) in enumerate(patterns):
            x = 20 + i * (cw + 4)
            out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 1))
            out += msg(RP_SET_HIGH_COLOR, tok() + color(255, 220, 0))
            out += msg(RP_SET_LOW_COLOR, tok() + color(40, 40, 80))
            out += msg(RP_SET_PATTERN, tok() + bytes(bits))
            out += msg(RP_SET_OFFSETS, tok() + struct.pack("<ii", off, 0))
            out += msg(RP_FILL_RECT, tok() + rect(x, y1 + 10, x + cw - 1,
                                                  y1 + 10 + ch - 1))
            out += msg(RP_SET_OFFSETS, tok() + struct.pack("<ii", 0, 0))
            out += msg(RP_SET_PATTERN, tok() + bytes([0xFF] * 8))
            out += msg(RP_SET_HIGH_COLOR, tok() + color(255, 255, 255))
            out += label(x + 4, y1 + 10 + ch + 12, name)

        # A stippled stroke: the pattern has to apply to the pen too, not just
        # to fills.
        out += msg(RP_SET_PEN_SIZE, tok() + struct.pack("<f", 5.0))
        out += msg(RP_SET_PATTERN, tok() + bytes([0xAA, 0x55] * 4))
        out += msg(RP_SET_HIGH_COLOR, tok() + color(255, 255, 255))
        out += msg(RP_SET_LOW_COLOR, tok() + color(200, 0, 0))
        out += msg(RP_STROKE_ELLIPSE, tok() + rect(460, y1 + 10, 540, y1 + 50))
        out += msg(RP_SET_PEN_SIZE, tok() + struct.pack("<f", 1.0))
        out += msg(RP_SET_PATTERN, tok() + bytes([0xFF] * 8))
        out += label(464, y1 + 10 + ch + 12, "stroke")

        # -- gradients, one per kind ----------------------------------------
        y2 = y1 + 10 + ch + 26
        out += label(20, y2, "gradients: diamond and conic have a FIXED 100px "
                             "extent; radial-focus ignores its focus")
        ramp = [((0, 0, 0), 0.0), ((255, 255, 255), 255.0)]
        gw = 96
        specs = [
            ("linear", gradient(GRADIENT_LINEAR, ramp,
                                start=(0, 0), end=(0, 0))),
            ("radial r=48", None),
            ("focus", None),
            ("diamond", None),
            ("conic", None),
        ]
        for i, (name, _) in enumerate(specs):
            x = 20 + i * (gw + 8)
            gy = y2 + 10
            cx, cy = x + gw / 2, gy + ch / 2
            if name == "linear":
                g = gradient(GRADIENT_LINEAR, ramp,
                             start=(x, gy), end=(x + gw - 1, gy))
            elif name.startswith("radial"):
                g = gradient(GRADIENT_RADIAL, ramp, center=(cx, cy), radius=48.0)
            elif name == "focus":
                # A focal point far off centre. If the client honours it the two
                # circles differ; app_server does not, so they must match.
                g = gradient(GRADIENT_RADIAL_FOCUS, ramp, center=(cx, cy),
                             focal=(x + 4, gy + 4), radius=48.0)
            elif name == "diamond":
                g = gradient(GRADIENT_DIAMOND, ramp, center=(cx, cy))
            else:
                g = gradient(GRADIENT_CONIC, ramp, center=(cx, cy), angle=45.0)
            out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 1))
            out += msg(RP_FILL_RECT_GRADIENT,
                       tok() + rect(x, gy, x + gw - 1, gy + ch - 1) + g)
            out += label(x + 4, gy + ch + 12, name)

        # -- arcs and a shape containing an arc -----------------------------
        y3 = y2 + 10 + ch + 26
        out += label(20, y3, "arcs: fill, stroke, and a BShape ArcTo "
                             "(neither reference client draws the last one)")
        ay = y3 + 10
        out += msg(RP_SET_HIGH_COLOR, tok() + color(255, 160, 0))
        out += msg(RP_FILL_ARC, tok() + rect(20, ay, 20 + 79, ay + 79)
                   + struct.pack("<ff", 30.0, 120.0))
        out += msg(RP_SET_HIGH_COLOR, tok() + color(0, 255, 200))
        out += msg(RP_SET_PEN_SIZE, tok() + struct.pack("<f", 3.0))
        out += msg(RP_STROKE_ARC, tok() + rect(120, ay, 120 + 79, ay + 79)
                   + struct.pack("<ff", 200.0, 140.0))
        out += msg(RP_SET_PEN_SIZE, tok() + struct.pack("<f", 1.0))

        # A quarter-circle wedge built from MoveTo + ArcTo + LineTo + Close.
        # The angle field is in RADIANS, not degrees.
        out += msg(RP_SET_HIGH_COLOR, tok() + color(255, 80, 200))
        ops = [OP_MOVETO | 1, OP_SMALL_ARC_TO_CW | 3, OP_LINETO | 1, OP_CLOSE]
        pts = [(300, ay), (40, 40), (0.0, 0.0), (340, ay + 40), (300, ay + 40)]
        out += msg(RP_FILL_SHAPE, tok() + rect(250, ay, 350, ay + 80)
                   + shape(ops, pts))
        out += label(24, ay + 92, "fill arc")
        out += label(124, ay + 92, "stroke arc")
        out += label(254, ay + 92, "shape arc")

        # -- RP_INVERT_RECT: not the same thing as B_OP_INVERT --------------
        # Painter::_InvertRect32 inverts every channel of every pixel in the
        # rect, ignoring both the drawing mode and the pattern, where B_OP_INVERT
        # is gated on the stipple. Half of this band overlaps the arc so the
        # difference is visible.
        out += msg(RP_SET_PATTERN, tok() + bytes([0xAA, 0x55] * 4))
        out += msg(RP_INVERT_RECT, tok() + rect(420, ay, 420 + 119, ay + 39))
        out += msg(RP_SET_PATTERN, tok() + bytes([0xFF] * 8))
        out += label(424, ay + 92, "invert rect")

        # -- RP_COPY_RECT_NO_CLIPPING: scrolling, and the flip trap ---------
        # app_server keeps no framebuffer, so it scrolls a text view and moves a
        # window by telling the client to copy its own pixels
        # (RemoteDrawingEngine.cpp:292 -- xOffset, yOffset, rect, no token).
        # The trap: a client whose canvas has a y-flipped CTM and hands the
        # snapshot straight to CGContextDrawImage gets the block *vertically
        # mirrored*, because the image's first row lands on the destination's
        # max-y edge. On a uniform rect that is invisible; here it is not.
        y4 = ay + 106
        out += label(20, y4, "copy rect: middle block must match the left; "
                             "right one is that scrolled up a band ('top' gone)")
        by = y4 + 10
        bands = [((220, 60, 60), "top"), ((60, 220, 60), "mid"),
                 ((60, 60, 220), "bottom")]
        for i, (c, name) in enumerate(bands):
            out += msg(RP_FILL_RECT_COLOR,
                       tok() + rect(20, by + i * 18, 20 + 119,
                                    by + i * 18 + 17) + color(*c))
            out += msg(RP_SET_HIGH_COLOR, tok() + color(255, 255, 255))
            out += label(24, by + i * 18 + 14, name)
        # Two sideways copies of the same source block.
        for dx in (140, 280):
            out += msg(RP_COPY_RECT_NO_CLIPPING,
                       struct.pack("<ii", dx, 0)
                       + rect(20, by, 20 + 119, by + 53))
        # Then scroll the rightmost copy up by one band in place, the way a
        # Terminal scrolls: 'top' goes, leaving mid above bottom.
        out += msg(RP_COPY_RECT_NO_CLIPPING,
                   struct.pack("<ii", 0, -18)
                   + rect(300, by, 300 + 119, by + 53))

        # -- a read-back that asks for the cursor ---------------------------
        # The cursor is deliberately not in the client's canvas, so this checks it
        # gets composited in on the way out.
        self.pending_read_back = True
        out += msg(RP_READ_BITMAP, tok() + rect(80, 80, 80 + 31, 80 + 31)
                   + b"\x01")
        return bytes(out)

    # -- round-trip probes -------------------------------------------------

    def probe_string_width(self, text="The quick brown fox"):
        bs = text.encode()
        self.pending_width[text] = time.monotonic()
        self.send(msg(RP_STRING_WIDTH,
                      tok() + struct.pack("<I", len(bs)) + bs))

    # -- incremental updates (Phase 3) -------------------------------------

    def update_loop(self):
        """Redraw a moving indicator, the way a real session repaints."""
        frame = 0
        w, h = self.width, self.height
        base_x, base_y = 70, 400
        prev = None
        while not self.closing:
            time.sleep(0.1)
            frame += 1
            out = bytearray()

            x = base_x + int(180 + 170 * math.sin(frame / 12.0))
            y = base_y

            # Erase the previous position, draw the new one -- a dirty-rect
            # repaint expressed the only way this protocol can: as drawing ops.
            if prev is not None:
                out += msg(RP_FILL_RECT_COLOR,
                           tok() + rect(prev[0], prev[1], prev[0] + 23,
                                        prev[1] + 23) + color(51, 102, 152))
            out += msg(RP_FILL_RECT_COLOR,
                       tok() + rect(x, y, x + 23, y + 23)
                       + color(255, 203, 0))
            out += msg(RP_STROKE_RECT_1PX_COLOR,
                       tok() + rect(x, y, x + 23, y + 23) + color(0, 0, 0))
            prev = (x, y)

            # A ticking counter, to show text repaints live.
            out += msg(RP_FILL_RECT_COLOR,
                       tok() + rect(70, 440, 70 + 300, 440 + 16)
                       + color(51, 102, 152))
            out += msg(RP_SET_HIGH_COLOR, tok() + color(255, 255, 255))
            label = f"frame {frame}  rtt-probes {len(self.stats['width_rtt'])}  events {self.stats['events']}".encode()
            out += msg(RP_DRAW_STRING,
                       tok() + point(70, 452)
                       + struct.pack("<I", len(label)) + label + b"\x00")

            # Every 20th frame, fragment the write to keep the client's
            # reassembly path honest.
            if frame % 20 == 0:
                self.send_split(bytes(out), 7)
            else:
                self.send(bytes(out))

            if frame % 30 == 0:
                self.probe_string_width(f"probe at frame {frame}")

    # -- receive -----------------------------------------------------------

    def handle(self, code, payload):
        r = Reader(payload)
        if code == RP_INIT_CONNECTION:
            print(f"<- RP_INIT_CONNECTION from {self.addr}")
            # Real order: ack, then cursor state.
            self.send(msg(RP_INIT_CONNECTION))
            self.send(msg(RP_SET_CURSOR, point(1, 1) + cursor_bitmap()))
            self.send(msg(RP_SET_CURSOR_VISIBLE, b"\x01"))
            self.send(msg(RP_MOVE_CURSOR_TO, point(100.0, 100.0)))
            print("-> ack + cursor; waiting for RP_UPDATE_DISPLAY_MODE "
                  "(nothing is drawn until then)")

        elif code == RP_HELLO:
            version, caps = r.u32(), r.u32()
            # max decode width/height and requested width/height follow; read
            # for forward compatibility, act on none of them (like the real
            # server at M0).
            negotiated_version = min(version, RP_PROTOCOL_VERSION)
            negotiated_caps = caps & RP_CAP_STRING_WIDTH_REPLY
            print(f"<- RP_HELLO version={version} caps={caps:#x} -> "
                  f"ack version={negotiated_version} caps={negotiated_caps:#x}")
            self.send(msg(RP_HELLO_ACK, struct.pack("<II", negotiated_version,
                                                    negotiated_caps)))

        elif code == RP_UPDATE_DISPLAY_MODE:
            self.width, self.height = r.i32(), r.i32()
            print(f"<- RP_UPDATE_DISPLAY_MODE {self.width}x{self.height}")
            if not self.started:
                self.started = True
                scene = self.initial_scene()
                print(f"-> initial scene, {len(scene)} bytes "
                      f"(sent fragmented to test reassembly)")
                self.send_split(scene, 1400)
                if self.torture:
                    extra = self.torture_scene()
                    print(f"-> torture scene, {len(extra)} bytes")
                    self.send_split(extra, 1400)
                self.probe_string_width()
                if not self.torture:
                    threading.Thread(target=self.update_loop,
                                     daemon=True).start()
                else:
                    print("   (update loop disabled in --torture mode so the "
                          "frame is deterministic and comparable)")
            else:
                # A mode change on a live session is legal and is the only way to
                # force a full repaint: app_server answers _NotifyScreenChanged()
                # and redraws everything, because it holds no framebuffer to
                # resend. A client resizing its window relies on this.
                print("   (live resolution change -- repainting, as "
                      "_NotifyScreenChanged does)")
                scene = self.initial_scene()
                self.send_split(scene, 1400)
                if self.torture:
                    self.send_split(self.torture_scene(), 1400)

        elif code == RP_GET_SYSTEM_PALETTE:
            print("<- RP_GET_SYSTEM_PALETTE")
            self.send(msg(RP_GET_SYSTEM_PALETTE_RESULT, system_palette()))

        elif code == RP_READ_BITMAP_RESULT:
            t = r.i32()
            bw, bh, bpr = r.i32(), r.i32(), r.i32()
            cs, flags, nbytes = r.u32(), r.u32(), r.u32()
            self.read_backs += 1
            ok = bpr >= bw * 3
            print(f"<- RP_READ_BITMAP_RESULT {bw}x{bh} bytesPerRow={bpr} "
                  f"cs=0x{cs:04x} bytes={nbytes} "
                  f"{'ok' if ok else '!! bytesPerRow < width*3'}")

        elif code == RP_STRING_WIDTH_RESULT:
            t = r.i32()
            width = r.f32()
            if self.pending_width:
                text, t0 = next(iter(self.pending_width.items()))
                del self.pending_width[text]
                rtt = (time.monotonic() - t0) * 1000
                self.stats["width_rtt"].append(rtt)
                print(f"<- RP_STRING_WIDTH_RESULT tok={t} width={width:.2f} "
                      f"rtt={rtt:.1f}ms  {'OK' if rtt < 1000 else 'TOO SLOW: app_server would have timed out'}")

        elif code == RP_DRAW_STRING_RESULT:
            t = r.i32()
            x, y = r.f32(), r.f32()
            self.stats["draw_rtt"].append(0)
            if self.verbose:
                print(f"<- RP_DRAW_STRING_RESULT tok={t} pen=({x:.1f},{y:.1f})")

        elif code in (RP_MOUSE_MOVED, RP_MOUSE_DOWN, RP_MOUSE_UP):
            x, y = r.f32(), r.f32()
            extra = ""
            if code != RP_MOUSE_MOVED:
                buttons = r.i32()
                extra = f" buttons={buttons}"
                if code == RP_MOUSE_DOWN and r.left() >= 4:
                    extra += f" clicks={r.i32()}"
            self.stats["events"] += 1
            name = {RP_MOUSE_MOVED: "MOVED", RP_MOUSE_DOWN: "DOWN",
                    RP_MOUSE_UP: "UP"}[code]
            if self.verbose or code != RP_MOUSE_MOVED:
                print(f"<- RP_MOUSE_{name} ({x:.1f},{y:.1f}){extra}")
            # Echo the position back as a cursor move, so Phase 4 shows a
            # visible server-driven response to input.
            self.cursor = (x, y)
            self.send(msg(RP_MOVE_CURSOR_TO, point(x, y)))

        elif code == RP_MOUSE_WHEEL_CHANGED:
            dx, dy = r.f32(), r.f32()
            self.stats["events"] += 1
            print(f"<- RP_MOUSE_WHEEL_CHANGED dx={dx:.2f} dy={dy:.2f}")

        elif code in (RP_KEY_DOWN, RP_KEY_UP):
            n = r.i32()
            raw = bytes(payload[r.p:r.p + n]); r.p += n
            rawchar = r.i32() if r.left() >= 4 else None
            key = r.i32() if r.left() >= 4 else None
            self.stats["events"] += 1
            print(f"<- RP_KEY_{'DOWN' if code == RP_KEY_DOWN else 'UP'} "
                  f"bytes={raw!r} raw_char={rawchar} key={key}")

        elif code == RP_MODIFIERS_CHANGED:
            self.stats["events"] += 1
            print(f"<- RP_MODIFIERS_CHANGED 0x{r.u32():08x}")

        elif code == RP_CLOSE_CONNECTION:
            print("<- RP_CLOSE_CONNECTION")
            self.closing = True

        else:
            print(f"<- unexpected code {code} ({len(payload)}B) -- clients "
                  f"should not send this")

    def run(self):
        self.conn.settimeout(0.5)
        try:
            while not self.closing:
                try:
                    data = self.conn.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    break
                self.buf += data
                while len(self.buf) >= HEADER:
                    code, total = struct.unpack_from("<HI", self.buf, 0)
                    if total < HEADER:
                        print(f"!! client sent bad length {total}")
                        self.closing = True
                        break
                    if len(self.buf) < total:
                        break
                    payload = bytes(self.buf[HEADER:total])
                    del self.buf[:total]
                    try:
                        self.handle(code, payload)
                    except Exception as e:
                        print(f"!! error handling code {code}: {e!r}")
        finally:
            self.closing = True
            try:
                self.conn.close()
            except OSError:
                pass
            self.report()

    def report(self):
        w = self.stats["width_rtt"]
        print(f"\n-- session with {self.addr} ended --")
        print(f"   input events received: {self.stats['events']}")
        print(f"   RP_DRAW_STRING_RESULT replies: {len(self.stats['draw_rtt'])}")
        if w:
            print(f"   RP_STRING_WIDTH round trips: {len(w)}, "
                  f"min {min(w):.1f}ms avg {sum(w)/len(w):.1f}ms max {max(w):.1f}ms")
            over = [x for x in w if x >= 1000]
            if over:
                print(f"   !! {len(over)} exceeded app_server's 1s timeout")
        else:
            print("   !! client never answered RP_STRING_WIDTH -- real app_server "
                  "would stall 1s per query and fall back to its own metrics")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=10900)
    p.add_argument("--latency-ms", type=float, default=0.0,
                   help="added one-way delay on every send, to simulate wifi")
    p.add_argument("--verbose", action="store_true")
    p.add_argument("--torture", action="store_true",
                   help="also draw every drawing mode, stipple phases, all five "
                        "gradient kinds, arcs and a BShape ArcTo")
    a = p.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((a.host, a.port))
    srv.listen(1)
    print(f"mock app_server listening on {a.host}:{a.port}"
          + (f" (+{a.latency_ms:g}ms simulated latency)" if a.latency_ms else ""))
    print("waiting for a client to connect and send RP_INIT_CONNECTION...")
    try:
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print(f"\n== connection from {addr} ==")
            Session(conn, addr, a.latency_ms, a.verbose, a.torture).run()
    except KeyboardInterrupt:
        print("\nshutting down")
    finally:
        srv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
