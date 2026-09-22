#!/usr/bin/env python3
"""
rp_probe -- speak Haiku's app_server RemoteDesktop protocol over a raw TCP
socket and print a decoded trace of what comes back.

This is the Phase 1 instrument: it proves (or disproves) that the protocol works
over a plain `ssh -L` forward with no websockify and no WebSocket upgrade, and
it doubles as an executable reference for the client's decoder.

Layouts follow PROTOCOL.md, which was derived from
src/servers/app/drawing/interface/remote/ in the DeBeOS checkout.

A direct connection to the session port must present app_server's per-boot
session cookie as its first frame, so every live run needs --cookie-file.

Usage
-----
    # in one shell: carry the guest's loopback port out
    ssh -N -L 10900:127.0.0.1:10900 baron@<host>

    # fetch the cookie app_server minted for that port (mode 0600 on the guest)
    ssh baron@<host> cat /boot/system/settings/remote_desktop/session_cookie.10900 \
        > /tmp/session-cookie

    # in another shell: handshake and trace
    ./rp_probe.py --port 10900 --cookie-file /tmp/session-cookie

    # handshake only, no display mode -- expect cursor messages then silence
    ./rp_probe.py --port 10900 --cookie-file /tmp/session-cookie \
        --no-display-mode

    # prove the decoder without a live instance
    ./rp_probe.py --self-test
"""

import argparse
import socket
import struct
import sys
import threading
import time

# ---------------------------------------------------------------------------
# Message codes (RemoteMessage.h:38-139)
# ---------------------------------------------------------------------------

CODES = {
    1: "RP_INIT_CONNECTION",
    2: "RP_UPDATE_DISPLAY_MODE",
    3: "RP_CLOSE_CONNECTION",
    4: "RP_GET_SYSTEM_PALETTE",
    5: "RP_GET_SYSTEM_PALETTE_RESULT",
    20: "RP_CREATE_STATE",
    21: "RP_DELETE_STATE",
    22: "RP_ENABLE_SYNC_DRAWING",
    23: "RP_DISABLE_SYNC_DRAWING",
    24: "RP_INVALIDATE_RECT",
    25: "RP_INVALIDATE_REGION",
    40: "RP_SET_OFFSETS",
    41: "RP_SET_HIGH_COLOR",
    42: "RP_SET_LOW_COLOR",
    43: "RP_SET_PEN_SIZE",
    44: "RP_SET_STROKE_MODE",
    45: "RP_SET_BLENDING_MODE",
    46: "RP_SET_PATTERN",
    47: "RP_SET_DRAWING_MODE",
    48: "RP_SET_FONT",
    49: "RP_SET_TRANSFORM",
    60: "RP_CONSTRAIN_CLIPPING_REGION",
    61: "RP_COPY_RECT_NO_CLIPPING",
    62: "RP_INVERT_RECT",
    63: "RP_DRAW_BITMAP",
    64: "RP_DRAW_BITMAP_RECTS",
    80: "RP_STROKE_ARC",
    81: "RP_STROKE_BEZIER",
    82: "RP_STROKE_ELLIPSE",
    83: "RP_STROKE_POLYGON",
    84: "RP_STROKE_RECT",
    85: "RP_STROKE_ROUND_RECT",
    86: "RP_STROKE_SHAPE",
    87: "RP_STROKE_TRIANGLE",
    88: "RP_STROKE_LINE",
    89: "RP_STROKE_LINE_ARRAY",
    100: "RP_FILL_ARC",
    101: "RP_FILL_BEZIER",
    102: "RP_FILL_ELLIPSE",
    103: "RP_FILL_POLYGON",
    104: "RP_FILL_RECT",
    105: "RP_FILL_ROUND_RECT",
    106: "RP_FILL_SHAPE",
    107: "RP_FILL_TRIANGLE",
    108: "RP_FILL_REGION",
    120: "RP_FILL_ARC_GRADIENT",
    121: "RP_FILL_BEZIER_GRADIENT",
    122: "RP_FILL_ELLIPSE_GRADIENT",
    123: "RP_FILL_POLYGON_GRADIENT",
    124: "RP_FILL_RECT_GRADIENT",
    125: "RP_FILL_ROUND_RECT_GRADIENT",
    126: "RP_FILL_SHAPE_GRADIENT",
    127: "RP_FILL_TRIANGLE_GRADIENT",
    128: "RP_FILL_REGION_GRADIENT",
    140: "RP_STROKE_POINT_COLOR",
    141: "RP_STROKE_LINE_1PX_COLOR",
    142: "RP_STROKE_RECT_1PX_COLOR",
    160: "RP_FILL_RECT_COLOR",
    161: "RP_FILL_REGION_COLOR_NO_CLIPPING",
    180: "RP_DRAW_STRING",
    181: "RP_DRAW_STRING_WITH_OFFSETS",
    182: "RP_DRAW_STRING_RESULT",
    183: "RP_STRING_WIDTH",
    184: "RP_STRING_WIDTH_RESULT",
    185: "RP_READ_BITMAP",
    186: "RP_READ_BITMAP_RESULT",
    200: "RP_SET_CURSOR",
    201: "RP_SET_CURSOR_VISIBLE",
    202: "RP_MOVE_CURSOR_TO",
    220: "RP_MOUSE_MOVED",
    221: "RP_MOUSE_DOWN",
    222: "RP_MOUSE_UP",
    223: "RP_MOUSE_WHEEL_CHANGED",
    240: "RP_KEY_DOWN",
    241: "RP_KEY_UP",
    242: "RP_UNMAPPED_KEY_DOWN",
    243: "RP_UNMAPPED_KEY_UP",
    244: "RP_MODIFIERS_CHANGED",
    260: "RP_STROKE_ARC_GRADIENT",
    261: "RP_STROKE_BEZIER_GRADIENT",
    262: "RP_STROKE_ELLIPSE_GRADIENT",
    263: "RP_STROKE_POLYGON_GRADIENT",
    264: "RP_STROKE_RECT_GRADIENT",
    265: "RP_STROKE_ROUND_RECT_GRADIENT",
    266: "RP_STROKE_SHAPE_GRADIENT",
    267: "RP_STROKE_TRIANGLE_GRADIENT",
    268: "RP_STROKE_LINE_GRADIENT",
}

RP_INIT_CONNECTION = 1
RP_UPDATE_DISPLAY_MODE = 2
RP_GET_SYSTEM_PALETTE = 4

# app_server's candidate gate (NetReceiver::_ReceiveCandidateData): the first
# frame of a direct connection to the session port must be RP_SESSION_COOKIE
# carrying the per-boot cookie, and anything else -- including a bare
# RP_INIT_CONNECTION, which is what this probe used to open with -- is dropped
# without a reply. app_server mints the cookie into
# <system settings>/remote_desktop/session_cookie.<port> (mode 0600) before it
# starts listening; read it there and pass --cookie-file. The gate consumes the
# frame, so nothing below this line has to know about it.
RP_SESSION_COOKIE = 12
RP_COOKIE_METHOD_PER_BOOT = 1
RP_SESSION_COOKIE_MAX_LENGTH = 256

HEADER_SIZE = 6

# Messages that carry no leading int32 token (PROTOCOL.md §5.1). Everything
# else is token-addressed. RP_CREATE_STATE/RP_DELETE_STATE are listed here
# because their int32 *is* the whole payload rather than a prefix.
SESSION_LEVEL = {1, 3, 5, 20, 21, 24, 25, 61, 161, 200, 201, 202}

COLOR_SPACES = {
    0x0000: "B_NO_COLOR_SPACE",
    0x0001: "B_GRAY1",
    0x0002: "B_GRAY8",
    0x0003: "B_RGB24",
    0x0004: "B_CMAP8",
    0x0005: "B_RGB16",
    0x0008: "B_RGB32",
    0x0010: "B_RGB15",
    0x2008: "B_RGBA32",
    0x2010: "B_RGBA15",
    0x1003: "B_RGB24_BIG",
    0x1005: "B_RGB16_BIG",
    0x1008: "B_RGB32_BIG",
    0x1010: "B_RGB15_BIG",
    0x3008: "B_RGBA32_BIG",
    0x3010: "B_RGBA15_BIG",
}

DRAWING_MODES = {
    0: "B_OP_COPY", 1: "B_OP_OVER", 2: "B_OP_ERASE", 3: "B_OP_INVERT",
    4: "B_OP_ADD", 5: "B_OP_SUBTRACT", 6: "B_OP_BLEND", 7: "B_OP_MIN",
    8: "B_OP_MAX", 9: "B_OP_SELECT", 10: "B_OP_ALPHA",
}


class Reader:
    """Cursor over a message payload.

    Everything is little-endian and fully packed with no alignment padding
    (PROTOCOL.md §3), so struct.unpack_from at arbitrary offsets is exactly
    right -- it does unaligned loads by construction.
    """

    def __init__(self, buf, offset=0, limit=None):
        self.buf = buf
        self.pos = offset
        self.limit = len(buf) if limit is None else limit

    def left(self):
        return self.limit - self.pos

    def _take(self, fmt, size):
        if self.pos + size > self.limit:
            raise EOFError(f"want {size} bytes, {self.left()} left")
        v = struct.unpack_from(fmt, self.buf, self.pos)
        self.pos += size
        return v

    def u8(self):
        return self._take("<B", 1)[0]

    def i8(self):
        return self._take("<b", 1)[0]

    def bool_(self):
        return self._take("<B", 1)[0] != 0

    def u16(self):
        return self._take("<H", 2)[0]

    def i16(self):
        return self._take("<h", 2)[0]

    def u32(self):
        return self._take("<I", 4)[0]

    def i32(self):
        return self._take("<i", 4)[0]

    def f32(self):
        return self._take("<f", 4)[0]

    def f64(self):
        return self._take("<d", 8)[0]

    def point(self):
        x, y = self._take("<2f", 8)
        return (x, y)

    def rect(self):
        # BRect is inclusive on both edges; width == right - left + 1.
        l, t, r, b = self._take("<4f", 16)
        return (l, t, r, b)

    def color(self):
        r, g, b, a = self._take("<4B", 4)
        return (r, g, b, a)

    def raw(self, n):
        if self.pos + n > self.limit:
            raise EOFError(f"want {n} bytes, {self.left()} left")
        v = self.buf[self.pos:self.pos + n]
        self.pos += n
        return v

    def region(self):
        n = self.i32()
        return [self.rect() for _ in range(n)]

    def string(self):
        # AddString: uint32 length then raw UTF-8, NOT NUL-terminated.
        n = self.u32()
        return self.raw(n).decode("utf-8", "replace")

    def bitmap(self, minimal=False):
        w = self.i32()
        h = self.i32()
        bpr = self.i32()
        cs = flags = None
        if not minimal:
            cs = self.u32()
            flags = self.u32()
        bits_len = self.u32()
        self.raw(bits_len)
        return {
            "width": w, "height": h, "bytesPerRow": bpr,
            "colorSpace": COLOR_SPACES.get(cs, cs) if cs is not None else None,
            "flags": flags, "bitsLength": bits_len,
        }

    def transform(self):
        if self.bool_():
            return "identity"
        return tuple(self.f64() for _ in range(6))


def fmt_rect(r):
    l, t, rr, b = r
    return f"({l:g},{t:g})-({rr:g},{b:g}) {rr - l + 1:g}x{b - t + 1:g}"


def describe(code, payload):
    """Decode a payload into a short human-readable summary.

    Deliberately does not attempt every op -- it decodes enough to prove the
    stream is coherent and the framing/token rules in PROTOCOL.md hold. An
    unknown or short payload is reported, never silently ignored.
    """
    r = Reader(payload)
    try:
        token = None
        if code not in SESSION_LEVEL:
            token = r.i32()
        tok = f"tok={token} " if token is not None else ""

        if code == 1:
            return "(handshake ack, empty)"
        if code == 5:
            n = r.u32()
            first = [r.color() for _ in range(min(n, 4))]
            return f"{n} palette entries, first={first}"
        if code in (20, 21):
            return f"token={r.i32()}"
        if code == 24:
            return f"rect {fmt_rect(r.rect())}"
        if code in (25, 60):
            rects = r.region()
            return f"{tok}region, {len(rects)} rects" + (
                f", first {fmt_rect(rects[0])}" if rects else "")
        if code == 61:
            dx, dy = r.i32(), r.i32()
            return f"offset=({dx},{dy}) src {fmt_rect(r.rect())}"
        if code == 161:
            rects = r.region()
            return (f"{len(rects)} rects, color={r.color()}"
                    + (f", first {fmt_rect(rects[0])}" if rects else ""))
        if code == 200:
            hs = r.point()
            return f"hotspot={hs} cursor={r.bitmap()}"
        if code == 201:
            return f"visible={r.bool_()}"
        if code == 202:
            return f"pos=({r.f32():g},{r.f32():g})"
        if code == 40:
            return f"{tok}offsets=({r.i32()},{r.i32()})"
        if code in (41, 42):
            return f"{tok}color={r.color()}"
        if code == 43:
            return f"{tok}penSize={r.f32():g}"
        if code == 44:
            return f"{tok}cap={r.u32()} join={r.u32()} miter={r.f32():g}"
        if code == 45:
            return f"{tok}sourceAlpha={r.u32()} alphaFunc={r.u32()}"
        if code == 46:
            return f"{tok}pattern={r.raw(8).hex()}"
        if code == 47:
            m = r.u32()
            return f"{tok}mode={DRAWING_MODES.get(m, m)}"
        if code == 48:
            direction, encoding = r.u8(), r.u8()
            flags = r.u32()
            spacing = r.u8()
            shear, rotation, fbw, size = r.f32(), r.f32(), r.f32(), r.f32()
            face = r.u16()
            fam_style = r.u32()
            return (f"{tok}size={size:g} face=0x{face:04x} spacing={spacing} "
                    f"family={fam_style >> 16} style={fam_style & 0xffff} "
                    f"flags=0x{flags:x} shear={shear:g} rot={rotation:g}")
        if code == 49:
            return f"{tok}transform={r.transform()}"
        if code == 62:
            return f"{tok}rect {fmt_rect(r.rect())}"
        if code == 63:
            br, vr = r.rect(), r.rect()
            opts = r.u32()
            return (f"{tok}src {fmt_rect(br)} -> dst {fmt_rect(vr)} "
                    f"options={opts} {r.bitmap()}")
        if code == 64:
            opts, cs, flags = r.u32(), r.u32(), r.u32()
            n = r.i32()
            first = None
            for i in range(n):
                rect = r.rect()
                bm = r.bitmap(minimal=True)
                if i == 0:
                    first = (rect, bm)
            return (f"{tok}options={opts} cs={COLOR_SPACES.get(cs, cs)} "
                    f"{n} rects" + (f", first {fmt_rect(first[0])} "
                                    f"{first[1]['bitsLength']}B" if first else ""))
        if code in (84, 104, 82, 102):
            return f"{tok}rect {fmt_rect(r.rect())}"
        if code in (85, 105):
            rect = r.rect()
            return f"{tok}rect {fmt_rect(rect)} rx={r.f32():g} ry={r.f32():g}"
        if code in (80, 100):
            rect = r.rect()
            return f"{tok}rect {fmt_rect(rect)} angle={r.f32():g} span={r.f32():g}"
        if code == 88:
            return f"{tok}{r.point()} -> {r.point()}"
        if code == 89:
            n = r.i32()
            return f"{tok}{n} lines"
        if code == 140:
            return f"{tok}point={r.point()} color={r.color()}"
        if code == 141:
            return f"{tok}{r.point()} -> {r.point()} color={r.color()}"
        if code == 142:
            return f"{tok}rect {fmt_rect(r.rect())} color={r.color()}"
        if code == 160:
            return f"{tok}rect {fmt_rect(r.rect())} color={r.color()}"
        if code == 108:
            rects = r.region()
            return f"{tok}{len(rects)} rects"
        if code in (83, 103):
            bounds = r.rect()
            closed = r.bool_()
            n = r.i32()
            return f"{tok}bounds {fmt_rect(bounds)} closed={closed} {n} points"
        if code in (87, 107):
            pts = [r.point() for _ in range(3)]
            return f"{tok}points={pts} bounds {fmt_rect(r.rect())}"
        if code in (86, 106):
            bounds = r.rect()
            op_count = r.i32()
            ops = [r.u32() for _ in range(op_count)]
            pt_count = r.i32()
            for _ in range(pt_count):
                r.point()
            off = r.point()
            return (f"{tok}bounds {fmt_rect(bounds)} {op_count} ops "
                    f"{pt_count} points offset={off} scale={r.f32():g}")
        if code == 180:
            where = r.point()
            s = r.string()
            has_delta = r.bool_() if r.left() >= 1 else None
            return f"{tok}at ({where[0]:g},{where[1]:g}) {s!r} hasDelta={has_delta}"
        if code == 181:
            s = r.string()
            return f"{tok}{s!r} + {r.left() // 8} offsets"
        if code == 183:
            return f"{tok}{r.string()!r}"
        if code == 185:
            bounds = r.rect()
            return f"{tok}bounds {fmt_rect(bounds)} drawCursor={r.bool_()}"
        if code in (22, 23):
            return f"{tok}(no payload)"
        return f"{tok}{len(payload)}B payload, not decoded"
    except EOFError as e:
        return f"TRUNCATED: {e} (payload {len(payload)}B: {payload[:32].hex()})"


def build(code, body=b""):
    return struct.pack("<HI", code, HEADER_SIZE + len(body)) + body


class Trace:
    """Streaming framer, mirroring the receive loop in PROTOCOL.md §9.1."""

    def __init__(self):
        self.buf = bytearray()
        self.count = 0
        self.by_code = {}
        self.t0 = time.monotonic()

    def feed(self, data, out=sys.stdout, quiet=False):
        self.buf += data
        while len(self.buf) >= HEADER_SIZE:
            code, total = struct.unpack_from("<HI", self.buf, 0)
            if total < HEADER_SIZE:
                raise ValueError(
                    f"message claims {total} bytes, header alone is {HEADER_SIZE}")
            if len(self.buf) < total:
                break
            payload = bytes(self.buf[HEADER_SIZE:total])
            # Advance by the DECLARED length, never by fields consumed
            # (PROTOCOL.md §2.1) -- several messages carry trailing fields no
            # handler reads.
            del self.buf[:total]
            self.count += 1
            name = CODES.get(code, f"UNKNOWN({code})")
            self.by_code[name] = self.by_code.get(name, 0) + 1
            if not quiet:
                dt = (time.monotonic() - self.t0) * 1000
                print(f"[{dt:8.1f}ms] #{self.count:<5} {code:>3} "
                      f"{name:<34} {describe(code, payload)}", file=out)
        return self.count

    def summary(self, out=sys.stdout):
        print(f"\n-- {self.count} messages decoded, "
              f"{len(self.buf)} bytes left unparsed in buffer --", file=out)
        for name, n in sorted(self.by_code.items(), key=lambda kv: -kv[1]):
            print(f"   {n:>6}  {name}", file=out)
        unknown = [n for n in self.by_code if n.startswith("UNKNOWN")]
        if unknown:
            print(f"   !! unknown codes seen: {unknown}", file=out)


def cookie_frame(cookie):
    """The RP_SESSION_COOKIE frame app_server's candidate gate requires."""
    if isinstance(cookie, str):
        cookie = cookie.encode()
    if len(cookie) > RP_SESSION_COOKIE_MAX_LENGTH:
        raise ValueError("cookie is longer than %d characters"
                         % RP_SESSION_COOKIE_MAX_LENGTH)
    return build(RP_SESSION_COOKIE,
                 struct.pack("<II", RP_COOKIE_METHOD_PER_BOOT, len(cookie))
                 + cookie)


def probe(host, port, width, height, seconds, send_display_mode, quiet,
          cookie):
    print(f"== connecting to {host}:{port} (raw TCP, no WebSocket) ==")
    s = socket.create_connection((host, port), timeout=10)
    s.settimeout(1.0)

    trace = Trace()

    # Before any other byte: the gate reads exactly this frame and decides the
    # connection's fate on it. It sends nothing back either way, so the only
    # success signal is that the handshake below is answered at all.
    gate = cookie_frame(cookie)
    print(f"-> RP_SESSION_COOKIE   {gate[:HEADER_SIZE + 8].hex(' ')} "
          f"+ {len(gate) - HEADER_SIZE - 8} cookie bytes")
    s.sendall(gate)

    handshake = build(RP_INIT_CONNECTION)
    print(f"-> RP_INIT_CONNECTION  {handshake.hex(' ')}")
    s.sendall(handshake)

    # Wait for the ack before advertising a display mode: the ack is what
    # proves there is a live app_server rather than a silent listener.
    deadline = time.monotonic() + 5
    acked = False
    while time.monotonic() < deadline and not acked:
        try:
            data = s.recv(65536)
        except socket.timeout:
            continue
        if not data:
            print("!! server closed the connection during handshake")
            return 1
        trace.feed(data, quiet=quiet)
        acked = "RP_INIT_CONNECTION" in trace.by_code

    if not acked:
        print("!! no RP_INIT_CONNECTION ack within 5s. Either the "
              "remote_desktop service is not running on the guest, or the "
              "candidate gate refused the cookie we just sent -- a wrong or "
              "stale cookie looks exactly like a silent server from here, "
              "because the gate closes without replying. The cookie is "
              "per-boot: re-read "
              f"<system settings>/remote_desktop/session_cookie.{port} "
              "after a reboot.")
        return 1
    print("   ok: app_server acked the handshake over a raw socket")

    if send_display_mode:
        # This is what actually starts the session: it sets fIsConnected and
        # calls _NotifyScreenChanged(), which forces a full repaint.
        msg = build(RP_UPDATE_DISPLAY_MODE, struct.pack("<ii", width, height))
        print(f"-> RP_UPDATE_DISPLAY_MODE {width}x{height}")
        s.sendall(msg)
        print("-> RP_GET_SYSTEM_PALETTE")
        s.sendall(build(RP_GET_SYSTEM_PALETTE))
    else:
        print("   (skipping RP_UPDATE_DISPLAY_MODE: expect no drawing traffic)")

    print(f"== tracing for {seconds}s ==")
    end = time.monotonic() + seconds
    total_bytes = 0
    while time.monotonic() < end:
        try:
            data = s.recv(65536)
        except socket.timeout:
            continue
        if not data:
            print("!! server closed the connection")
            break
        total_bytes += len(data)
        trace.feed(data, quiet=quiet)

    s.close()
    trace.summary()
    print(f"-- {total_bytes} bytes received --")
    return 0


# ---------------------------------------------------------------------------
# Self-test: replay hand-built messages matching the documented layouts, so
# the decoder and framer can be validated with no instance in the loop.
# ---------------------------------------------------------------------------

def self_test():
    def cursor_bitmap(w, h):
        bpr = w * 4
        bits = bytes(bpr * h)
        return (struct.pack("<iii", w, h, bpr)
                + struct.pack("<II", 0x2008, 0)          # B_RGBA32, flags
                + struct.pack("<I", len(bits)) + bits)

    msgs = [
        build(RP_INIT_CONNECTION),
        build(200, struct.pack("<2f", 1.0, 1.0) + cursor_bitmap(16, 16)),
        build(201, b"\x01"),
        build(202, struct.pack("<2f", 320.0, 240.0)),
        build(5, struct.pack("<I", 2) + bytes([0, 0, 0, 255, 255, 255, 255, 255])),
        build(20, struct.pack("<i", 7)),
        build(41, struct.pack("<i", 7) + bytes([10, 20, 30, 255])),
        build(47, struct.pack("<ii", 7, 0)),
        build(46, struct.pack("<i", 7) + bytes([0xff] * 8)),
        build(49, struct.pack("<i", 7) + b"\x01"),
        build(60, struct.pack("<ii", 7, 1) + struct.pack("<4f", 0, 0, 799, 599)),
        build(160, struct.pack("<i", 7) + struct.pack("<4f", 0, 0, 799, 599)
              + bytes([51, 102, 152, 255])),
        build(142, struct.pack("<i", 7) + struct.pack("<4f", 4, 4, 100, 30)
              + bytes([0, 0, 0, 255])),
        # RP_DRAW_STRING with the trailing hasDelta bool the JS client never reads
        build(180, struct.pack("<i", 7) + struct.pack("<2f", 12.0, 24.0)
              + struct.pack("<I", 5) + b"Haiku" + b"\x00"),
        build(48, struct.pack("<i", 7) + bytes([0, 0])
              + struct.pack("<I", 0) + bytes([0])
              + struct.pack("<4f", 0.0, 0.0, 0.0, 12.0)
              + struct.pack("<H", 0x0020) + struct.pack("<I", (3 << 16) | 5)),
        build(61, struct.pack("<ii", 0, -20) + struct.pack("<4f", 0, 20, 799, 599)),
        build(161, struct.pack("<i", 1) + struct.pack("<4f", 0, 0, 799, 599)
              + bytes([0, 0, 0, 255])),
        build(89, struct.pack("<ii", 7, 2)
              + (struct.pack("<2f", 0, 0) + struct.pack("<2f", 9, 9)
                 + bytes([1, 2, 3, 255])) * 2),
        build(86, struct.pack("<i", 7) + struct.pack("<4f", 0, 0, 10, 10)
              + struct.pack("<i", 2)
              + struct.pack("<II", 0x80000000 | 1, 0x10000000 | 1)
              + struct.pack("<i", 2)
              + struct.pack("<2f", 0, 0) + struct.pack("<2f", 10, 10)
              + struct.pack("<2f", 0.5, 0.5) + struct.pack("<f", 1.0)),
        build(64, struct.pack("<i", 7) + struct.pack("<III", 0, 0x0008, 0)
              + struct.pack("<i", 1) + struct.pack("<4f", 0, 0, 3, 3)
              + struct.pack("<iii", 4, 4, 16) + struct.pack("<I", 64) + bytes(64)),
        build(21, struct.pack("<i", 7)),
        build(3),
    ]
    stream = b"".join(msgs)

    print(f"== self-test: {len(msgs)} synthetic messages, {len(stream)} bytes ==\n")

    # Feed the stream in deliberately awkward chunk sizes to exercise the
    # reassembly path: 1 byte at a time, then 7, then 4096.
    failures = []
    for chunk in (1, 7, 13, 4096):
        t = Trace()
        try:
            for i in range(0, len(stream), chunk):
                t.feed(stream[i:i + chunk], quiet=(chunk != 4096))
        except Exception as e:
            failures.append(f"chunk={chunk}: raised {e!r}")
            continue
        if t.count != len(msgs):
            failures.append(
                f"chunk={chunk}: decoded {t.count} messages, expected {len(msgs)}")
        if len(t.buf) != 0:
            failures.append(f"chunk={chunk}: {len(t.buf)} bytes left over")

    t = Trace()
    t.feed(stream)
    t.summary()

    # Every message must decode without reporting truncation.
    trunc = []
    off = 0
    while off < len(stream):
        code, total = struct.unpack_from("<HI", stream, off)
        d = describe(code, stream[off + HEADER_SIZE:off + total])
        # RP_INIT_CONNECTION and RP_CLOSE_CONNECTION legitimately carry no payload.
        empty_ok = code in (1, 3) and total == HEADER_SIZE
        if not empty_ok and ("TRUNCATED" in d or "not decoded" in d):
            trunc.append((CODES.get(code, code), d))
        off += total
    for name, d in trunc:
        failures.append(f"{name}: {d}")

    # A malformed length must be rejected, not silently accepted.
    try:
        Trace().feed(struct.pack("<HI", 1, 3))
        failures.append("undersized totalLength was accepted")
    except ValueError:
        pass

    # The cookie frame, against a golden vector spelled out byte by byte. Built
    # from the same constants cookie_frame() uses, a comparison would agree with
    # itself no matter which opcode or method those constants held; what is
    # pinned here is the wire as the other implementations spell it --
    # RP_SESSION_COOKIE = 12 and method 1 in RemoteMessage.h -- and
    # little-endian framing.
    secret = b"a" * 64
    golden = (bytes((12, 0))            # code 12
              + bytes((78, 0, 0, 0))    # total length 6 + 8 + 64
              + bytes((1, 0, 0, 0))     # method 1, per-boot cookie
              + bytes((64, 0, 0, 0))    # cookie length 64
              + secret)
    if cookie_frame(secret) != golden:
        failures.append("cookie frame does not match the golden wire bytes: %s"
                        % cookie_frame(secret)[:14].hex())
    if cookie_frame("a" * 64) != golden:
        failures.append("a str cookie encodes differently from bytes")
    try:
        cookie_frame(b"a" * (RP_SESSION_COOKIE_MAX_LENGTH + 1))
        failures.append("a cookie longer than the protocol allows was accepted")
    except ValueError:
        pass

    print()
    if failures:
        print("FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("PASS: framing survives 1/7/13/4096-byte chunking, all "
          f"{len(msgs)} messages decode cleanly, short length rejected")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=10900,
                   help="local end of the ssh -L forward (default 10900)")
    p.add_argument("--width", type=int, default=1280)
    p.add_argument("--height", type=int, default=800)
    p.add_argument("--seconds", type=float, default=10.0)
    p.add_argument("--no-display-mode", action="store_true",
                   help="skip RP_UPDATE_DISPLAY_MODE; expect no drawing traffic")
    p.add_argument("--quiet", action="store_true", help="summary only")
    p.add_argument("--cookie",
                   help="app_server's per-boot session cookie, required for a "
                        "direct connection to the session port. Prefer "
                        "--cookie-file: an argument is visible in ps and in "
                        "shell history")
    p.add_argument("--cookie-file",
                   help="file containing the session cookie. On the server it "
                        "is <system settings>/remote_desktop/session_cookie."
                        "<port>, readable only by the user app_server runs as; "
                        "read it there and point this at a local copy")
    p.add_argument("--self-test", action="store_true",
                   help="validate the decoder against synthetic messages")
    a = p.parse_args()

    if a.self_test:
        return self_test()

    cookie = a.cookie
    if a.cookie_file:
        try:
            with open(a.cookie_file) as f:
                cookie = f.read().strip()
        except OSError as exc:
            sys.stderr.write("rp_probe: cannot read cookie file: %s\n" % exc)
            return 2

    # Refused here rather than on the wire. The gate closes a connection whose
    # first frame is not a cookie without answering it, so the failure would
    # otherwise arrive as "no ack within 5s -- is the service running?", which
    # is a lie about a server that is running perfectly well.
    if not cookie:
        sys.stderr.write(
            "rp_probe: a direct connection to the session port requires "
            "app_server's per-boot session cookie: pass --cookie-file (the "
            "server's <system settings>/remote_desktop/session_cookie.%d)\n"
            % a.port)
        return 2

    return probe(a.host, a.port, a.width, a.height, a.seconds,
                 not a.no_display_mode, a.quiet, cookie)


if __name__ == "__main__":
    sys.exit(main())
