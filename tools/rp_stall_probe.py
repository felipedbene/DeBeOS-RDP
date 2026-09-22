#!/usr/bin/env python3
"""Wall-clock probe for the synchronous RP_READ_BITMAP round trip.

Mimics RemoteDrawingEngine::ReadBitmap -- flush the request, then block until the
result arrives or the 10 s timeout expires -- and reports the measured wait.

Usage: tools/rp_stall_probe.py build/haiku-remote

Every case must come back in single-digit milliseconds. A case that reports the
full 10 s is a client that skipped a mandatory reply: in a real session that is
10 s in which app_server holds the desktop drawing engine's exclusive lock and
nothing on the screen repaints.
"""
import socket
import struct
import subprocess
import sys
import time

RP_INIT_CONNECTION = 1
RP_READ_BITMAP = 185
RP_READ_BITMAP_RESULT = 186
READ_BITMAP_TIMEOUT = 10.0


def msg(code, payload=b""):
    return struct.pack("<HI", code, len(payload) + 6) + payload


def read_bitmap(token, left, top, right, bottom):
    return msg(RP_READ_BITMAP,
               struct.pack("<iffff?", token, left, top, right, bottom, False))


def messages(buffer):
    out = []
    while len(buffer) >= 6:
        code, total = struct.unpack_from("<HI", buffer, 0)
        if total < 6 or len(buffer) < total:
            break
        out.append((code, buffer[6:total]))
        buffer = buffer[total:]
    return out, buffer


def main():
    binary = sys.argv[1]
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]

    client = subprocess.Popen(
        [binary, "--host", "127.0.0.1", "--port", str(port),
         "--width", "60", "--height", "60", "--seconds", "20",
         "--output", "/tmp/stall-probe.png"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    connection, _ = listener.accept()
    connection.sendall(msg(RP_INIT_CONNECTION))

    pending = b""
    for label, request in (
            ("in-bounds BRect(0,0,9,9)", read_bitmap(1, 0, 0, 9, 9)),
            ("off-surface BRect(500,500,540,540)",
             read_bitmap(1, 500, 500, 540, 540)),
            ("empty BRect(0,0,-1,-1)", read_bitmap(1, 0, 0, -1, -1))):
        connection.sendall(request)
        started = time.monotonic()
        answered = False
        while not answered:
            remaining = READ_BITMAP_TIMEOUT - (time.monotonic() - started)
            if remaining <= 0:
                break
            connection.settimeout(remaining)
            try:
                chunk = connection.recv(1 << 16)
            except socket.timeout:
                break
            if not chunk:
                break
            found, pending = messages(pending + chunk)
            for code, payload in found:
                if code == RP_READ_BITMAP_RESULT:
                    width, height = struct.unpack_from("<ii", payload, 4)
                    answered = True
                    print(f"{label:38s} answered in "
                          f"{(time.monotonic() - started) * 1000:8.1f} ms "
                          f"({width}x{height})")
        if not answered:
            print(f"{label:38s} NO REPLY -- server drawing thread blocked for "
                  f"{time.monotonic() - started:.1f} s (full timeout)")

    connection.close()
    client.terminate()
    client.wait(timeout=10)


if __name__ == "__main__":
    main()
