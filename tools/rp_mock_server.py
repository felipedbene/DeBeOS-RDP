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
  * bitmaps in B_RGB32 / B_RGBA32 / B_CMAP8 / B_GRAY8 / B_GRAY1 (§6.3)
  * the blocking round-trips: RP_STRING_WIDTH and RP_DRAW_STRING expect replies,
    and this server reports how long the client took (§7.1)
  * incremental updates, for Phase 3
  * input events echoed back as cursor moves, for Phase 4

With --torture it also runs a set of *self-checking* raster cases: each one
draws something whose correct output is fixed by construction, asks for it back
with RP_READ_BITMAP, and compares the pixels the client returns. That turns the
scene from "did it crash" into a pass/fail check, which is the only way a decode
bug that renders *something* gets caught. See raster_scene().

Like the real server it also enforces the candidate gate: the first frame of a
connection must be RP_SESSION_COOKIE carrying the per-boot cookie, and anything
else is closed without a reply. The cookie is minted and printed at startup (or
published with --cookie-file, the way app_server publishes it); --no-cookie
models the broker path, where the broker presents the cookie and the client
sends none.

Usage:
    ./rp_mock_server.py --port 10900 --cookie-file /tmp/mock-cookie
    ./rp_mock_server.py --port 10900 --latency-ms 150   # simulate travel wifi
    ./rp_mock_server.py --port 10900 --torture --once   # exit 1 if a check fails
"""

import argparse
import math
import os
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

# The candidate gate (NetReceiver::_ReceiveCandidateData): the first frame of a
# direct connection must be RP_SESSION_COOKIE{uint32 method, length-prefixed
# cookie}. See Session.validate_cookie() -- this mock enforces it, because a
# mock that accepts a shape the real server refuses cannot catch a client that
# sends that shape.
RP_SESSION_COOKIE = 12
RP_COOKIE_METHOD_PER_BOOT = 1
RP_SESSION_COOKIE_MAX_LENGTH = 256
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
RP_STROKE_ROUND_RECT = 85
RP_STROKE_SHAPE = 86
RP_STROKE_LINE = 88
RP_FILL_ARC = 100
RP_FILL_RECT = 104
RP_FILL_ELLIPSE = 102
RP_FILL_ROUND_RECT = 105
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
B_GRAY1 = 0x0001
B_RGB24 = 0x0003

# The `options` word of RP_DRAW_BITMAP and RP_DRAW_BITMAP_RECTS, straight from
# BView::DrawBitmap (headers/os/interface/InterfaceDefs.h:306-323).
B_TILE_BITMAP_X = 0x0001
B_TILE_BITMAP_Y = 0x0002
B_TILE_BITMAP = B_TILE_BITMAP_X | B_TILE_BITMAP_Y
B_FILTER_BITMAP_BILINEAR = 0x0100

# B_TRANSPARENT_MAGIC_RGBA32 is 0x00777477 (GraphicsDefs.cpp:33). B_RGB32 has no
# alpha channel, so that reserved pixel value is how BeOS and Haiku spell
# "see-through": Painter rewrites it to alpha 0 before blending in every drawing
# mode except B_OP_COPY and B_OP_ALPHA (BitmapPainter.cpp:262-307). The
# comparison is against the whole uint32 (_TransparentMagicToAlpha,
# BitmapPainter.cpp:334-352), so in a little-endian B_RGB32 row the bytes are
# B=0x77 G=0x74 R=0x77 *and a reserved byte of 0x00* -- all four are the value.
MAGIC_R, MAGIC_G, MAGIC_B = 0x77, 0x74, 0x77
MAGIC_RESERVED = 0x00

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


def bitmap_gray1(w, h, rows, bpr=None):
    """B_GRAY1. `rows` is one iterable of byte values per row.

    Bit order and polarity are Haiku's, not the JS client's: ReadGray1 takes
    shift = 7 - (index % 8), so the *most significant* bit of a byte is the
    leftmost pixel, and a *set* bit is black -- it maps to 0x00, a clear bit to
    0xFF (ColorConversion.cpp:556-567). So 0x80 is one black pixel followed by
    seven white ones.

    BytesPerRow is (width + 7) / 8 with no further padding (Bitmap.cpp:119-121);
    ServerBitmap takes max_c(requested, that), so a wider row is legal and `bpr`
    sends one.
    """
    minimum = (w + 7) // 8
    if bpr is None:
        bpr = minimum
    if bpr < minimum:
        raise ValueError(f"bytesPerRow {bpr} is shorter than {minimum}")
    bits = b"".join(bytes(r) + bytes(bpr - len(bytes(r))) for r in rows)
    return (struct.pack("<iii", w, h, bpr)
            + struct.pack("<II", B_GRAY1, 0)
            + struct.pack("<I", len(bits)) + bits)


def bitmap_cmap8_sized(w, h, fn, bpr=None):
    """B_CMAP8 with an explicit bytesPerRow, for a drag bitmap of any width."""
    if bpr is None:
        bpr = w
    rows = []
    for y in range(h):
        row = bytearray(bytes(fn(x, y) for x in range(w)))
        row += bytes(bpr - w)
        rows.append(bytes(row))
    bits = b"".join(rows)
    return (struct.pack("<iii", w, h, bpr)
            + struct.pack("<II", B_CMAP8, 0)
            + struct.pack("<I", len(bits)) + bits)


class Frame:
    """The pixels an RP_READ_BITMAP_RESULT brought back.

    The client answers in B_RGB24 -- three bytes per pixel in B, G, R order,
    rows padded to bytesPerRow -- which is what RemoteDrawingEngine::ReadBitmap
    then ImportBits() into the screenshot bitmap
    (RemoteDrawingEngine.cpp:1154-1161).
    """

    def __init__(self, w, h, bpr, colorspace, bits):
        self.w = w
        self.h = h
        self.bpr = bpr
        self.colorspace = colorspace
        self.bits = bits

    def px(self, x, y):
        o = y * self.bpr + x * 3
        return (self.bits[o + 2], self.bits[o + 1], self.bits[o])

    def is_(self, x, y, rgb):
        return self.px(x, y) == tuple(rgb)


class Check:
    """One self-checking raster case: a read-back rect plus its expected pixels.

    `verify(frame)` returns a list of complaints -- empty means the case passed.
    `catches` records what the case would have caught, so a failure explains
    itself without going back to the commit that added it.
    """

    def __init__(self, name, rect, verify, catches="", fatal=True):
        self.name = name
        self.rect = rect
        self.verify = verify
        self.catches = catches
        self.fatal = fatal


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
    def __init__(self, conn, addr, latency_ms, verbose, torture=False,
                 raster_checks=True, expect_cookie=None):
        # The cookie this connection must present as its first frame, or None to
        # model the broker path, where the broker has already presented one to
        # app_server and the client itself must send none.
        self.expect_cookie = expect_cookie
        self.gate_refusal = None
        self.conn = conn
        self.addr = addr
        self.latency = latency_ms / 1000.0
        self.verbose = verbose
        self.torture = torture
        self.raster_checks = raster_checks
        self.pending_read_back = False
        self.read_backs = 0
        # Queue of Check, in the order their RP_READ_BITMAP went out; replies
        # come back in the same order because the client answers in order.
        self.checks = []
        self.passed = []
        self.failed = []
        self.notes = []
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

        def legacy(frame):
            # Unchanged from the original scene: B_RGB24 needs three bytes per
            # pixel, so a shorter row means the reply is not what it claims.
            if frame.bpr < frame.w * 3:
                return [f"bytesPerRow {frame.bpr} < width*3 ({frame.w * 3})"]
            return []

        self.checks.append(Check("readback-shape", (80, 80, 111, 111), legacy,
                                 "a reply whose rows are too short to hold "
                                 "B_RGB24 pixels"))
        out += msg(RP_READ_BITMAP, tok() + rect(80, 80, 80 + 31, 80 + 31)
                   + b"\x01")
        return bytes(out)

    # -- self-checking raster cases ----------------------------------------

    def raster_scene(self):
        """Cases whose correct output is fixed by construction, then read back.

        Everything above renders *something* for a human to look at, which is
        why a whole class of decode bug survived it: a scene that draws a
        B_GRAY1 bitmap and never inspects the pixels passes just as happily
        with the bit order and the polarity both inverted. Each case here
        states its expected pixels independently -- from Haiku's own reader, or
        from the geometry -- and RP_READ_BITMAP brings the client's answer back
        for comparison.

        Laid out in the column right of x=560, which the scenes above never
        touch, so adding these changes not one pixel of the existing frame.
        """
        PX = 560
        BG = (16, 24, 32)
        FILL = (255, 255, 255)
        GREEN = (0, 200, 0)
        RED = (255, 0, 0)
        MARK = (255, 0, 255)
        BLACK = (0, 0, 0)
        WHITE = (255, 255, 255)
        MAGIC_PAINTED = (MAGIC_R, MAGIC_G, MAGIC_B)

        out = bytearray()

        def prime(r, c):
            """Paint a cell to a known colour through an explicit-colour fill.

            B_OP_COPY and a solid pattern, so the value that lands is the value
            asked for and every case below starts from a known background.
            """
            return (msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 0))
                    + msg(RP_SET_PATTERN, tok() + bytes([0xFF] * 8))
                    + msg(RP_FILL_RECT_COLOR, tok() + rect(*r) + color(*c)))

        def probe(name, r, verify, catches="", fatal=True, draw_cursor=0):
            self.checks.append(Check(name, r, verify, catches, fatal))
            # RP_READ_BITMAP: token, bounds, drawCursor
            # (RemoteDrawingEngine.cpp:1136-1141).
            return msg(RP_READ_BITMAP,
                       tok() + rect(*r) + bytes((draw_cursor,)))

        def scan(frame, expected):
            """Compare every pixel against expected(x, y) -> (r, g, b)."""
            bad = []
            for y in range(frame.h):
                for x in range(frame.w):
                    want = expected(x, y)
                    got = frame.px(x, y)
                    if got != tuple(want):
                        bad.append(f"({x},{y}) is {got}, expected {tuple(want)}")
                        if len(bad) >= 6:
                            bad.append("...")
                            return bad
            return bad

        # -- 1. an empty clipping region must suppress drawing entirely -----
        # RP_CONSTRAIN_CLIPPING_REGION carries a rect count and zero is a legal,
        # meaningful value (AddRegion, RemoteMessage.h:421-429: a count, then
        # that many rects). app_server reaches it: ServerWindow.cpp constrains
        # the engine to an empty region for AS_VIEW_END_LAYER and replays the
        # layer anyway. Using a separate token keeps the state off token 1.
        cell = (PX, 60, PX + 63, 60 + 31)
        out += prime(cell, BG)
        out += msg(RP_CREATE_STATE, tok(2))
        out += msg(RP_SET_DRAWING_MODE, tok(2) + struct.pack("<i", 0))
        out += msg(RP_SET_PATTERN, tok(2) + bytes([0xFF] * 8))
        out += msg(RP_CONSTRAIN_CLIPPING_REGION, tok(2) + region([]))
        out += msg(RP_FILL_RECT_COLOR, tok(2) + rect(*cell) + color(*FILL))
        out += msg(RP_DELETE_STATE, tok(2))
        out += probe("clip-empty-suppresses-all", cell,
                     lambda f: scan(f, lambda x, y: BG),
                     "a client that reads 'no rects' as 'no clipping' and so "
                     "paints over the whole screen")

        # -- 2. ...and a non-empty region must still clip --------------------
        # The guard for the other direction: it is no good suppressing an empty
        # region by suppressing every region.
        cell = (PX, 100, PX + 63, 100 + 31)
        out += prime(cell, BG)
        out += msg(RP_CREATE_STATE, tok(3))
        out += msg(RP_SET_DRAWING_MODE, tok(3) + struct.pack("<i", 0))
        out += msg(RP_SET_PATTERN, tok(3) + bytes([0xFF] * 8))
        out += msg(RP_CONSTRAIN_CLIPPING_REGION,
                   tok(3) + region([(PX, 100, PX + 31, 100 + 31)]))
        out += msg(RP_FILL_RECT_COLOR, tok(3) + rect(*cell) + color(*FILL))
        out += msg(RP_DELETE_STATE, tok(3))
        out += probe("clip-partial-still-clips", cell,
                     lambda f: scan(f, lambda x, y: FILL if x < 32 else BG),
                     "clipping suppressed altogether, or a region read with "
                     "the wrong rect count")

        # -- 3. an asymmetric round rect ------------------------------------
        # RP_FILL_ROUND_RECT: token, rect, xRadius, yRadius -- in that order
        # (RemoteDrawingEngine.cpp:809-826). With xRadius 4 and yRadius 32 the
        # corner is a narrow, tall sliver: the corner bites at most xRadius off
        # the left edge, and about half of yRadius off the top. Exchange the two
        # and both numbers exchange with them, which is what the assertion
        # below measures -- no dependence on how the curve is approximated.
        RR_X, RR_Y = 4.0, 32.0
        cell = (PX, 140, PX + 79, 140 + 79)
        out += prime(cell, BG)
        out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 0))
        out += msg(RP_SET_PATTERN, tok() + bytes([0xFF] * 8))
        out += msg(RP_SET_HIGH_COLOR, tok() + color(*FILL))
        out += msg(RP_FILL_ROUND_RECT,
                   tok() + rect(*cell) + struct.pack("<ff", RR_X, RR_Y))

        def check_round_rect(f):
            bad = []
            inset_x = []          # per row, how far in the first filled pixel is
            inset_y = []          # per column, ditto
            for y in range(f.h):
                row = [x for x in range(f.w) if f.is_(x, y, FILL)]
                if row:
                    inset_x.append(min(row))
            for x in range(f.w):
                col = [y for y in range(f.h) if f.is_(x, y, FILL)]
                if col:
                    inset_y.append(min(col))
            if not inset_x or not inset_y:
                return ["nothing was filled at all"]
            worst_x, worst_y = max(inset_x), max(inset_y)
            # xRadius is 4, so no row may start more than a few pixels in;
            # yRadius is 32, so the leftmost column must start well down.
            if worst_x > 8:
                bad.append(f"deepest horizontal corner inset is {worst_x}px, "
                           f"expected <=8 for xRadius {RR_X:g} "
                           f"(radii exchanged? yRadius is {RR_Y:g})")
            if worst_y < 12:
                bad.append(f"deepest vertical corner inset is {worst_y}px, "
                           f"expected >=12 for yRadius {RR_Y:g} "
                           f"(radii exchanged? xRadius is {RR_X:g})")
            return bad

        out += probe("round-rect-asymmetric-radii", cell, check_round_rect,
                     "xRadius and yRadius exchanged -- which is what happens "
                     "when both are read as arguments to one call, because C++ "
                     "leaves argument evaluation order unspecified")

        # -- 4. B_GRAY1: MSB first, and a set bit is BLACK ------------------
        # Expected output by construction: byte 0x80 is one black pixel then
        # seven white, 0x01 is seven white then one black, 0x00 is all white and
        # 0xFF all black (ColorConversion.cpp:556-567).
        GRAY1 = [0x80, 0x01, 0xF0, 0x0F, 0xAA, 0x55, 0x00, 0xFF]
        dest = (PX, 230, PX + 7, 230 + 7)
        out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 0))
        out += msg(RP_DRAW_BITMAP,
                   tok() + rect(0, 0, 7, 7) + rect(*dest)
                   + struct.pack("<I", 0)
                   + bitmap_gray1(8, 8, [[b] for b in GRAY1]))
        out += probe("gray1-bit-order-and-polarity", dest,
                     lambda f: scan(f, lambda x, y:
                                    BLACK if (GRAY1[y] >> (7 - x)) & 1 else WHITE),
                     "B_GRAY1 read LSB-first, or a set bit taken for white -- "
                     "the HTML5 client does both, and it is not the oracle")

        # -- 5. ...across a byte boundary and past the row padding ----------
        # 12 pixels is a byte and a half, and ServerBitmap keeps whatever
        # bytesPerRow it was given as long as it is not below the minimum, so
        # this row is padded to 4. Pixel 8 lives in the second byte's top bit;
        # the padding bytes must never reach the screen.
        STRIDE = [[0xF0, 0x00], [0x00, 0xF0]]
        dest = (PX + 20, 230, PX + 20 + 11, 230 + 1)
        out += msg(RP_DRAW_BITMAP,
                   tok() + rect(0, 0, 11, 1) + rect(*dest)
                   + struct.pack("<I", 0)
                   + bitmap_gray1(12, 2, STRIDE, bpr=4))
        out += probe("gray1-row-stride", dest,
                     lambda f: scan(f, lambda x, y:
                                    BLACK if (STRIDE[y][x // 8] >> (7 - x % 8)) & 1
                                    else WHITE),
                     "a row walked by pixel index instead of by bytesPerRow, "
                     "or padding bytes rendered as pixels")

        # -- 6. B_RGB32 transparent magic must not paint --------------------
        def magic_bitmap(reserved):
            def px(x, y):
                if (x + y) % 2 == 0:
                    return (MAGIC_R, MAGIC_G, MAGIC_B, reserved)
                return (255, 0, 0, 0xFF)
            return bitmap_rgb32(8, 8, px, colorspace=B_RGB32)

        dest = (PX, 245, PX + 7, 245 + 7)
        out += prime(dest, GREEN)
        out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 1))  # B_OP_OVER
        out += msg(RP_DRAW_BITMAP,
                   tok() + rect(0, 0, 7, 7) + rect(*dest)
                   + struct.pack("<I", 0) + magic_bitmap(MAGIC_RESERVED))
        out += probe("rgb32-transparent-magic-over", dest,
                     lambda f: scan(f, lambda x, y:
                                    GREEN if (x + y) % 2 == 0 else RED),
                     "the reserved see-through value painted as an ordinary "
                     "grey, so every B_RGB32 bitmap with a transparent border "
                     "gets a box around it")

        # -- 7. ...except in B_OP_COPY, where it is an ordinary colour ------
        # The control for the case above: B_OP_COPY keeps the value, and so does
        # B_OP_ALPHA (BitmapPainter.cpp:265-278, a deliberate BeOS
        # compatibility). Suppressing magic in every mode is also wrong.
        dest = (PX + 20, 245, PX + 20 + 7, 245 + 7)
        out += prime(dest, GREEN)
        out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 0))  # B_OP_COPY
        out += msg(RP_DRAW_BITMAP,
                   tok() + rect(0, 0, 7, 7) + rect(*dest)
                   + struct.pack("<I", 0) + magic_bitmap(MAGIC_RESERVED))
        out += probe("rgb32-transparent-magic-copy", dest,
                     lambda f: scan(f, lambda x, y:
                                    MAGIC_PAINTED if (x + y) % 2 == 0 else RED),
                     "transparency applied in B_OP_COPY, where app_server "
                     "treats the reserved value as the colour it is")

        # -- 8. the reserved byte is part of the value (report only) --------
        # _TransparentMagicToAlpha compares the whole uint32 against
        # 0x00777477, so a pixel with the same RGB but a reserved byte of 0xFF
        # is NOT transparent to app_server. Reported rather than asserted: it
        # is a divergence to know about, not a regression to gate on.
        dest = (PX + 40, 245, PX + 40 + 7, 245 + 7)
        out += prime(dest, GREEN)
        out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 1))  # B_OP_OVER
        out += msg(RP_DRAW_BITMAP,
                   tok() + rect(0, 0, 7, 7) + rect(*dest)
                   + struct.pack("<I", 0) + magic_bitmap(0xFF))
        out += probe("rgb32-magic-reserved-byte", dest,
                     lambda f: scan(f, lambda x, y:
                                    MAGIC_PAINTED if (x + y) % 2 == 0 else RED),
                     "RGB matched without the reserved byte, so a pixel "
                     "app_server would paint is dropped", fatal=False)

        # -- 9. fractional rect edges truncate, they do not round up --------
        # Painter::FillRect aligns both corners with (int32)coord and then
        # covers them inclusively (Painter.cpp:970-978, :1010-1023,
        # :1083-1086, :1648-1652). So left 0.5 still starts at 0 and right 9.5
        # still stops at 9: a 10-pixel span, not 11. floor/ceil is right for a
        # bounding box and one pixel too generous for a fill.
        window = (PX, 260, PX + 15, 260 + 15)
        out += prime(window, BG)
        out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 0))
        out += msg(RP_FILL_RECT_COLOR,
                   tok() + rect(PX + 0.5, 260.5, PX + 9.5, 260 + 9.5)
                   + color(*FILL))
        out += probe("fill-rect-fractional-edges", window,
                     lambda f: scan(f, lambda x, y:
                                    FILL if x <= 9 and y <= 9 else BG),
                     "a fill one column and one row too wide whenever a rect "
                     "has a fractional edge -- which text backgrounds and "
                     "scaled layouts produce constantly")

        # -- 10. cursor operations -----------------------------------------
        # Three things at once. During a drag RP_SET_CURSOR does not carry the
        # usual 16x16 B_RGBA32 arrow: RemoteHWInterface::SetDragBitmap sends
        # AddCursor(CursorAndDragBitmap()), whose bitmap is the union of the
        # cursor and drag frames in the *drag bitmap's* colour space with the
        # combining shift as its hotspot (RemoteHWInterface.cpp:895-903,
        # HWInterface.cpp:930-946). So the payload is a hotspot BPoint followed
        # by a full, arbitrarily sized, arbitrarily coloured bitmap record
        # (AddCursor, RemoteMessage.cpp:190-194), and it arrives mid-stream in
        # the same write as drawing. A client that walked it by fields rather
        # than by the declared length would eat the fill that follows, so the
        # marker below is the desync detector.
        patch = (PX, 285, PX + 31, 285 + 31)
        marker = (PX + 4, 289, PX + 7, 292)
        out += prime(patch, BG)
        out += msg(RP_SET_CURSOR,
                   point(11, 7)
                   + bitmap_cmap8_sized(40, 28,
                                        lambda x, y: (x * 5 + y * 3) % 256,
                                        bpr=44))
        out += msg(RP_FILL_RECT_COLOR, tok() + rect(*marker) + color(*MARK))
        out += msg(RP_SET_CURSOR_VISIBLE, b"\x01")
        out += msg(RP_MOVE_CURSOR_TO, point(PX + 16, 301))

        def in_marker(x, y):
            return 4 <= x <= 7 and 4 <= y <= 7

        out += probe("cursor-ops-are-not-drawing", patch,
                     lambda f: scan(f, lambda x, y:
                                    MARK if in_marker(x, y) else BG),
                     "a cursor blitted destructively into the framebuffer, or "
                     "an RP_SET_CURSOR payload that desyncs the stream and "
                     "swallows the next drawing message")

        # And the same patch with drawCursor set. RemoteDrawingEngine::ReadBitmap
        # passes the flag through (RemoteDrawingEngine.cpp:1136-1141) because a
        # screenshot wants the pointer in it. Reported, not asserted: see the
        # note in report().
        def cursor_composited(f):
            painted = sum(1 for y in range(f.h) for x in range(f.w)
                          if not (f.is_(x, y, MARK) if in_marker(x, y)
                                  else f.is_(x, y, BG)))
            if painted == 0:
                return ["drawCursor was set and no cursor pixel came back -- "
                        "the client ignores the flag, so screenshots taken "
                        "through RP_READ_BITMAP have no pointer in them"]
            return []

        out += probe("read-bitmap-draw-cursor-flag", patch, cursor_composited,
                     "a screenshot with no pointer in it", fatal=False,
                     draw_cursor=1)

        # -- 11. RP_DRAW_BITMAP's options word: B_TILE_BITMAP ---------------
        # BView::DrawTiledBitmap sets B_TILE_BITMAP_X|_Y and RemoteDrawingEngine
        # forwards the word verbatim (RemoteDrawingEngine.cpp:470). A tiled draw
        # does not scale -- app_server pins scaleX/scaleY to 1 and wraps
        # (BitmapPainter.cpp:196-227, DrawBitmapGeneric.h:26-30) -- so the
        # expectation here is geometric: destination pixel (x, y) is source pixel
        # (x % 4, y % 4). A client that discards the word draws one stretched
        # copy, which triples every source pixel instead.
        def tile_px(x, y):
            return (15 + 60 * x, 15 + 60 * y, 128, 255)

        cell = (PX, 320, PX + 11, 320 + 11)
        out += prime(cell, BG)
        out += msg(RP_SET_DRAWING_MODE, tok() + struct.pack("<i", 0))
        out += msg(RP_DRAW_BITMAP,
                   tok() + rect(0, 0, 3, 3) + rect(*cell)
                   + struct.pack("<I", B_TILE_BITMAP)
                   + bitmap_rgb32(4, 4, tile_px))
        out += probe("draw-bitmap-tiling-options", cell,
                     lambda f: scan(f, lambda x, y: tile_px(x % 4, y % 4)[:3]),
                     "the RP_DRAW_BITMAP options word discarded, so every "
                     "BView::DrawTiledBitmap arrives as one stretched copy")

        # -- 12. ...and B_FILTER_BITMAP_BILINEAR ----------------------------
        # Magnify a two-colour checker. The property asserted is the
        # discriminating one rather than the pixel values: nearest-neighbour can
        # only ever emit the two colours that are in the bitmap, so *any* value
        # between them proves a filter ran. app_server's exact weights are
        # pinned in the client's own render tests; over the wire the question is
        # only whether the filter bit survived the trip.
        cell = (PX + 20, 320, PX + 20 + 8, 320 + 8)
        out += prime(cell, BG)
        out += msg(RP_DRAW_BITMAP,
                   tok() + rect(0, 0, 1, 1) + rect(*cell)
                   + struct.pack("<I", B_FILTER_BITMAP_BILINEAR)
                   + bitmap_rgb32(2, 2, lambda x, y:
                                  (0, 0, 0, 255) if x == y
                                  else (255, 255, 255, 255)))

        def filtered(f):
            bad = []
            blended = sum(1 for y in range(f.h) for x in range(f.w)
                          if f.px(x, y)[0] == f.px(x, y)[1] == f.px(x, y)[2]
                          and 8 < f.px(x, y)[0] < 247)
            if blended < 40:
                bad.append(f"only {blended} of {f.w * f.h} pixels are an "
                           "intermediate grey -- a bilinear magnification of a "
                           "black/white checker is mostly intermediate, and "
                           "nearest-neighbour has none")
            if f.px(0, 0) != BLACK:
                bad.append(f"(0,0) is {f.px(0, 0)}, expected the source "
                           f"corner {BLACK}")
            return bad

        out += probe("draw-bitmap-bilinear-filter", cell, filtered,
                     "B_FILTER_BITMAP_BILINEAR discarded, so a magnified "
                     "bitmap is nearest-neighbour where the local desktop "
                     "smooths it")

        return bytes(out)

    def send_raster_scene(self):
        """Send the self-checking cases, if there is room and they are wanted."""
        if not self.raster_checks:
            print("   (raster checks disabled)")
            return
        # The cases live in the column right of x=560, down to y=317.
        if self.width < 660 or self.height < 340:
            print(f"   !! {self.width}x{self.height} is too small for the "
                  f"self-checking raster cases (needs 660x340) -- SKIPPED")
            return
        cases = self.raster_scene()
        print(f"-> raster checks, {len(cases)} bytes, "
              f"{len(self.checks)} read-backs queued")
        self.send_split(cases, 1400)

    # -- round-trip probes -------------------------------------------------

    def _settle(self, frame):
        """Match a read-back against the head of the check queue."""
        if not self.checks:
            return
        check = self.checks.pop(0)
        want_w = int(check.rect[2]) - int(check.rect[0]) + 1
        want_h = int(check.rect[3]) - int(check.rect[1]) + 1
        if (frame.w, frame.h) != (want_w, want_h):
            self.failed.append(
                (check, [f"reply is {frame.w}x{frame.h}, asked for "
                         f"{want_w}x{want_h}"]))
            print(f"   FAIL {check.name}: wrong reply size")
            return
        if frame.colorspace != B_RGB24:
            self.failed.append(
                (check, [f"reply colour space is 0x{frame.colorspace:04x}, "
                         f"expected B_RGB24 (0x{B_RGB24:04x})"]))
            print(f"   FAIL {check.name}: wrong reply colour space")
            return
        try:
            bad = check.verify(frame)
        except Exception as e:      # a truncated reply, most likely
            bad = [f"verification raised {e!r}"]
        if not bad:
            self.passed.append(check)
            print(f"   ok   {check.name}")
        elif check.fatal:
            self.failed.append((check, bad))
            print(f"   FAIL {check.name}")
            for line in bad:
                print(f"        {line}")
            if check.catches:
                print(f"        would have caught: {check.catches}")
        else:
            self.notes.append((check, bad))
            print(f"   note {check.name}")
            for line in bad:
                print(f"        {line}")

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
                    self.send_raster_scene()
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
                    self.send_raster_scene()

        elif code == RP_GET_SYSTEM_PALETTE:
            print("<- RP_GET_SYSTEM_PALETTE")
            self.send(msg(RP_GET_SYSTEM_PALETTE_RESULT, system_palette()))

        elif code == RP_READ_BITMAP_RESULT:
            t = r.i32()
            bw, bh, bpr = r.i32(), r.i32(), r.i32()
            cs, flags, nbytes = r.u32(), r.u32(), r.u32()
            bits = bytes(payload[r.p:r.p + nbytes])
            self.read_backs += 1
            ok = bpr >= bw * 3
            print(f"<- RP_READ_BITMAP_RESULT {bw}x{bh} bytesPerRow={bpr} "
                  f"cs=0x{cs:04x} bytes={nbytes} "
                  f"{'ok' if ok else '!! bytesPerRow < width*3'}")
            self._settle(Frame(bw, bh, bpr, cs, bits))

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

    # -- the candidate gate ------------------------------------------------

    def validate_cookie(self, timeout=10.0):
        """app_server's candidate gate, modelled as it actually behaves.

        NetReceiver::_ReceiveCandidateData() reads exactly the six byte header,
        then exactly the rest of the length that header declared, and never a
        byte further -- so the rest of the client's stream stays in the kernel
        buffer, unread and unforwarded. It requires code RP_SESSION_COOKIE, a
        declared length between HEADER + 8 and HEADER + 8 + 256, method
        RP_COOKIE_METHOD_PER_BOOT, a cookie length that agrees with the frame,
        and the right cookie. Anything else closes the connection WITHOUT a
        reply, which is why a client that gets this wrong sees only silence.

        This is here to be a mutation detector: with it, a client that stops
        sending the cookie frame fails against this mock the way it fails
        against app_server. Without it, the mock accepted every shape and no
        offline test could see the difference.
        """
        if self.expect_cookie is None:
            return True

        expected = self.expect_cookie
        if isinstance(expected, str):
            expected = expected.encode()

        def read_exactly(count):
            data = b""
            self.conn.settimeout(timeout)
            while len(data) < count:
                try:
                    chunk = self.conn.recv(count - len(data))
                except (socket.timeout, OSError):
                    return None
                if not chunk:
                    return None
                data += chunk
            return data

        header = read_exactly(HEADER)
        if header is None:
            self.gate_refusal = "closed or failed before the session cookie"
            return False
        code, length = struct.unpack("<HI", header)
        if (code != RP_SESSION_COOKIE or length < HEADER + 8
                or length > HEADER + 8 + RP_SESSION_COOKIE_MAX_LENGTH):
            # Includes the pre-cookie client shape (a bare RP_INIT_CONNECTION).
            self.gate_refusal = (
                "first frame is not a session cookie (code %d, length %d)"
                % (code, length))
            return False
        body = read_exactly(length - HEADER)
        if body is None:
            self.gate_refusal = "incomplete session cookie frame"
            return False
        method, cookie_length = struct.unpack_from("<II", body, 0)
        if (method != RP_COOKIE_METHOD_PER_BOOT
                or HEADER + 8 + cookie_length != length):
            self.gate_refusal = ("malformed session cookie (method %d, cookie "
                                 "length %d)" % (method, cookie_length))
            return False
        if body[8:8 + cookie_length] != expected:
            self.gate_refusal = "wrong session cookie"
            return False
        print(f"<- RP_SESSION_COOKIE accepted from {self.addr}")
        return True

    def run(self):
        if not self.validate_cookie():
            print(f"!! dropping takeover candidate: {self.gate_refusal}")
            print("   (app_server closes without replying here, so the client "
                  "sees only silence -- that is the bug this models)")
            self.closing = True
            try:
                self.conn.close()
            except OSError:
                pass
            return

        self.conn.settimeout(0.5)
        try:
            while not self.closing:
                try:
                    data = self.conn.recv(65536)
                except socket.timeout:
                    continue
                except OSError as e:
                    # A client that goes away mid-session resets rather than
                    # closing, and that used to take the whole server down with
                    # a traceback -- losing the end-of-session report, which is
                    # the one part worth having. It is also the normal case
                    # rather than an odd one: a real app_server hangs up by
                    # itself, so the client side of a finished session is often
                    # already gone by the time the last reply is read.
                    print(f"<- connection dropped ({e.__class__.__name__})")
                    break
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
        self.report_checks()

    def unanswered(self):
        """Checks whose RP_READ_BITMAP never came back, gating ones first."""
        return ([c for c in self.checks if c.fatal],
                [c for c in self.checks if not c.fatal])

    def checks_ok(self):
        """The verdict, in one place so the report and the exit code agree."""
        return not self.failed and not self.unanswered()[0]

    def report_checks(self):
        """Summarise the self-checking cases."""
        if not (self.passed or self.failed or self.notes or self.checks):
            return True
        missing, missing_notes = self.unanswered()
        # Only gating cases are counted: a non-gating one reports either way and
        # would otherwise move the denominator around depending on its result.
        held = [c for c in self.passed if c.fatal]
        total = len(held) + len(self.failed) + len(missing)
        print(f"\n   raster checks: {len(held)}/{total} gating cases passed")
        for check, bad in self.failed:
            print(f"   FAIL {check.name}")
            for line in bad:
                print(f"        {line}")
            if check.catches:
                print(f"        would have caught: {check.catches}")
        for check in missing:
            print(f"   FAIL {check.name}: no RP_READ_BITMAP_RESULT ever came "
                  f"back. A decode error sends no reply at all, so a real "
                  f"app_server would sit out ReadBitmap's full 10s timeout "
                  f"here (RemoteDrawingEngine.cpp:1145-1152)")
        for check, bad in self.notes:
            print(f"   note {check.name} (not a gate)")
            for line in bad:
                print(f"        {line}")
        for check in missing_notes:
            print(f"   note {check.name} (not a gate): no reply came back")
        return self.checks_ok()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=10900)
    p.add_argument("--latency-ms", type=float, default=0.0,
                   help="added one-way delay on every send, to simulate wifi")
    p.add_argument("--verbose", action="store_true")
    p.add_argument("--torture", action="store_true",
                   help="also draw every drawing mode, stipple phases, all five "
                        "gradient kinds, arcs and a BShape ArcTo, plus the "
                        "self-checking raster cases")
    p.add_argument("--no-raster-checks", action="store_true",
                   help="with --torture, draw the scene but leave out the "
                        "self-checking cases and their RP_READ_BITMAP probes")
    p.add_argument("--once", action="store_true",
                   help="serve a single connection, then exit 1 if any "
                        "self-checking raster case failed -- the form to run "
                        "from a script")
    p.add_argument("--cookie",
                   help="require this session cookie as the first frame. "
                        "Default: mint one, the way app_server does")
    p.add_argument("--cookie-file",
                   help="publish the cookie here (mode 0600), the way "
                        "app_server publishes session_cookie.<port>, so a "
                        "client can be pointed at it with --cookie-file")
    p.add_argument("--no-cookie", action="store_true",
                   help="model the broker path: expect NO cookie frame, "
                        "because on ws/wss the broker presents its own")
    a = p.parse_args()

    # app_server mints the cookie before it binds, so a listener without one
    # never exists. Same here, and for the same reason: the gate is what makes
    # reaching the port insufficient.
    expect_cookie = None
    if not a.no_cookie:
        expect_cookie = a.cookie or "".join(
            "%02x" % byte for byte in os.urandom(32))
        if a.cookie_file:
            handle = os.open(a.cookie_file,
                             os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(handle, "w") as f:
                f.write(expect_cookie + "\n")
            print(f"session cookie published in {a.cookie_file}")
        else:
            print(f"session cookie: {expect_cookie}")
            print("   pass it to the client with --cookie, or re-run this with "
                  "--cookie-file PATH")

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((a.host, a.port))
    srv.listen(1)
    print(f"mock app_server listening on {a.host}:{a.port}"
          + (f" (+{a.latency_ms:g}ms simulated latency)" if a.latency_ms else ""))
    if expect_cookie is None:
        print("waiting for a client to connect and send RP_INIT_CONNECTION "
              "(no cookie expected: modelling the broker path)...")
    else:
        print("waiting for a client to connect and send RP_SESSION_COOKIE, "
              "then RP_INIT_CONNECTION...")
    status = 0
    try:
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print(f"\n== connection from {addr} ==")
            session = Session(conn, addr, a.latency_ms, a.verbose, a.torture,
                              raster_checks=not a.no_raster_checks,
                              expect_cookie=expect_cookie)
            session.run()
            if a.once:
                # A connection the gate refused ran no raster case at all, so
                # "no failures" must not read as PASS here.
                if session.gate_refusal is not None:
                    print("\nSESSION REFUSED AT THE GATE: "
                          + session.gate_refusal)
                    status = 1
                    break
                ok = session.checks_ok()
                print("\nRASTER CHECKS: " + ("PASS" if ok else "FAIL"))
                status = 0 if ok else 1
                break
    except KeyboardInterrupt:
        print("\nshutting down")
    finally:
        srv.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
