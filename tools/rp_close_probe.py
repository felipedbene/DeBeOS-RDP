#!/usr/bin/env python3
"""End-to-end probe: an orderly server close must not destroy the capture.

app_server hangs up on shutdown. RemoteHWInterface::_Disconnect() sends
RP_CLOSE_CONNECTION and then closes the listen endpoint
(RemoteHWInterface.cpp:706-717), and the native in-tree client treats that
opcode as "quit" (RemoteView.cpp:522-526). So a session that ends before the
capture deadline is the NORMAL case.

The client used to fold the following EOF into "receive failed" and `return 1`
BEFORE write_png, so every pixel that had already arrived and decoded was
thrown away. This probe drives the real binary over a real socket and checks
the file on disk, not the exit code alone.

Usage: tools/rp_close_probe.py build/haiku-remote

Three cases, all of which must write a PNG whose pixels are the two fills:
  * the server sends RP_CLOSE_CONNECTION and then closes,
  * the server just closes (a bare EOF, no in-band warning),
  * the server stays up until the client's own deadline (the control).
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

RP_INIT_CONNECTION = 1
RP_UPDATE_DISPLAY_MODE = 2
RP_CLOSE_CONNECTION = 3
RP_GET_SYSTEM_PALETTE = 4
RP_HELLO = 6
RP_HELLO_ACK = 7
RP_CREATE_STATE = 20
RP_FILL_RECT_COLOR = 160

WIDTH = 32
HEIGHT = 32


def msg(code, payload=b""):
    return struct.pack("<HI", code, len(payload) + 6) + payload


def fill_rect_color(token, left, top, right, bottom, rgba):
    return msg(RP_FILL_RECT_COLOR,
               struct.pack("<iffff", token, left, top, right, bottom)
               + bytes(rgba))


def scene():
    """RP_HELLO_ACK, a state, and two fills: red top-left, green bottom-right."""
    return (msg(RP_HELLO_ACK, struct.pack("<II", 1, 0))
            + msg(RP_CREATE_STATE, struct.pack("<i", 1))
            + fill_rect_color(1, 0, 0, 15, 15, (255, 0, 0, 255))
            + fill_rect_color(1, 16, 16, 31, 31, (0, 255, 0, 255)))


def read_png(path):
    """Minimal PNG reader: returns (width, height, rows of RGBA tuples)."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    offset = 8
    width = height = depth = color = None
    idat = b""
    while offset < len(data):
        (length,) = struct.unpack_from(">I", data, offset)
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        offset += 12 + length
    if depth != 8 or color not in (2, 6):
        raise ValueError("unexpected PNG format depth=%s color=%s"
                         % (depth, color))
    channels = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    pos = 0
    for _ in range(height):
        filter_type = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = previous[i]
            c = previous[i - channels] if i >= channels else 0
            if filter_type == 0:
                pass
            elif filter_type == 1:
                line[i] = (line[i] + a) & 0xFF
            elif filter_type == 2:
                line[i] = (line[i] + b) & 0xFF
            elif filter_type == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif filter_type == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
            else:
                raise ValueError("bad PNG filter %d" % filter_type)
        previous = line
        rows.append([tuple(line[x * channels:x * channels + channels])
                     for x in range(width)])
    return width, height, rows


def serve(listener, send_close_opcode, hold):
    """Accept one client, drive the handshake, paint, then end the session."""
    client, _ = listener.accept()
    client.settimeout(5.0)
    try:
        # Consume whatever the client opens with (RP_HELLO and/or
        # RP_INIT_CONNECTION) and answer the init so it starts drawing.
        deadline = time.time() + 3.0
        buffer = b""
        saw_init = False
        while time.time() < deadline and not saw_init:
            try:
                chunk = client.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            buffer += chunk
            while len(buffer) >= 6:
                code, total = struct.unpack_from("<HI", buffer, 0)
                if total < 6 or len(buffer) < total:
                    break
                if code == RP_INIT_CONNECTION:
                    saw_init = True
                buffer = buffer[total:]
        client.sendall(msg(RP_INIT_CONNECTION))
        client.sendall(scene())
        # Let the client decode before the stream ends, so a lost capture is
        # unambiguously the close handling and not a race on the pixels.
        time.sleep(0.4)
        if send_close_opcode:
            client.sendall(msg(RP_CLOSE_CONNECTION))
        if hold:
            time.sleep(hold)
    finally:
        try:
            client.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        client.close()


def run_case(binary, name, send_close_opcode, hold, seconds):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    thread = threading.Thread(target=serve,
                              args=(listener, send_close_opcode, hold),
                              daemon=True)
    thread.start()

    output = os.path.join(tempfile.mkdtemp(prefix="rp_close_"), "shot.png")
    started = time.time()
    completed = subprocess.run(
        [binary, "--host", "127.0.0.1", "--port", str(port),
         "--width", str(WIDTH), "--height", str(HEIGHT),
         "--seconds", str(seconds), "--output", output],
        capture_output=True, text=True, timeout=seconds + 30)
    elapsed = time.time() - started
    thread.join(timeout=5)
    listener.close()

    exists = os.path.exists(output) and os.path.getsize(output) > 0
    pixels_ok = False
    detail = ""
    if exists:
        try:
            width, height, rows = read_png(output)
            top_left = rows[8][8][:3]
            bottom_right = rows[24][24][:3]
            untouched = rows[8][24][:3]
            pixels_ok = (width == WIDTH and height == HEIGHT
                         and top_left == (255, 0, 0)
                         and bottom_right == (0, 255, 0)
                         and untouched == (0, 0, 0))
            detail = ("%dx%d (8,8)=%s (24,24)=%s (24,8)=%s"
                      % (width, height, top_left, bottom_right, untouched))
        except Exception as problem:  # noqa: BLE001 - probe, report anything
            detail = "PNG unreadable: %s" % problem
    else:
        detail = "no PNG written"

    ok = completed.returncode == 0 and pixels_ok
    print("%-22s exit=%d %5.1fs  png=%-3s pixels=%-3s  %s"
          % (name, completed.returncode, elapsed,
             "yes" if exists else "NO", "ok" if pixels_ok else "NO", detail))
    for line in (completed.stdout + completed.stderr).splitlines():
        print("    | %s" % line)
    return ok


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    results = [
        # The server says goodbye in band, then hangs up.
        run_case(binary, "RP_CLOSE_CONNECTION", True, 0.0, 10),
        # No warning at all: just EOF. Every pixel already arrived.
        run_case(binary, "bare EOF", False, 0.0, 10),
        # Control: the client's own deadline ends the capture.
        run_case(binary, "deadline (control)", False, 3.0, 2),
    ]
    print("%d of %d cases kept the capture" % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
