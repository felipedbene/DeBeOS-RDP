#!/usr/bin/env python3
"""Wall-clock probe for the synchronous RP_READ_BITMAP round trip.

Mimics RemoteDrawingEngine::ReadBitmap -- flush the request, then block until the
result arrives or the 10 s timeout expires -- and reports the measured wait.

Usage: tools/rp_stall_probe.py CrossPlatform/build/haiku-remote

Every case must come back in single-digit milliseconds. A case that reports the
full 10 s is a client that skipped a mandatory reply: in a real session that is
10 s in which app_server holds the desktop drawing engine's exclusive lock and
nothing on the screen repaints.
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

RP_INIT_CONNECTION = 1
RP_READ_BITMAP = 185
RP_READ_BITMAP_RESULT = 186
READ_BITMAP_TIMEOUT = 10.0

# The candidate gate (NetReceiver::_ReceiveCandidateData). Modelled here, not
# just tolerated: the client must present the cookie before anything else, and a
# stand-in server that skipped the check would pass a client that app_server
# refuses outright.
RP_SESSION_COOKIE = 12
RP_COOKIE_METHOD_PER_BOOT = 1
RP_SESSION_COOKIE_MAX_LENGTH = 256


def msg(code, payload=b""):
    return struct.pack("<HI", code, len(payload) + 6) + payload


def require_cookie(connection, expected, timeout=10.0):
    """Reads the first frame and returns None when it is the right cookie.

    Otherwise returns why it was refused, having read no further -- the gate
    reads exactly the header and then exactly the rest of the length it
    declared, and closes without a reply.
    """
    connection.settimeout(timeout)

    def read_exactly(count):
        data = b""
        while len(data) < count:
            try:
                chunk = connection.recv(count - len(data))
            except (socket.timeout, OSError):
                return None
            if not chunk:
                return None
            data += chunk
        return data

    header = read_exactly(6)
    if header is None:
        return "closed or failed before the session cookie"
    code, length = struct.unpack("<HI", header)
    if (code != RP_SESSION_COOKIE or length < 14
            or length > 14 + RP_SESSION_COOKIE_MAX_LENGTH):
        return ("first frame is not a session cookie (code %d, length %d)"
                % (code, length))
    body = read_exactly(length - 6)
    if body is None:
        return "incomplete session cookie frame"
    method, cookie_length = struct.unpack_from("<II", body, 0)
    if method != RP_COOKIE_METHOD_PER_BOOT or 14 + cookie_length != length:
        return "malformed session cookie"
    if body[8:8 + cookie_length] != expected.encode():
        return "wrong session cookie"
    return None


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

    directory = tempfile.mkdtemp(prefix="rp_stall_")
    cookie = "".join("%02x" % byte for byte in os.urandom(32))
    cookie_path = os.path.join(directory, "session_cookie.%d" % port)
    handle = os.open(cookie_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(handle, "w") as f:
        # With the trailing newline app_server writes, so a client that forgets
        # to strip it fails here rather than on hardware.
        f.write(cookie + "\n")

    client = subprocess.Popen(
        [binary, "--host", "127.0.0.1", "--port", str(port),
         "--width", "60", "--height", "60", "--seconds", "20",
         "--cookie-file", cookie_path,
         "--output", os.path.join(directory, "stall-probe.png")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Bounded, because a client that exits before connecting (a wrong path, or a
    # binary that does not accept these arguments) used to hang here forever
    # instead of saying so.
    listener.settimeout(30)
    try:
        connection, _ = listener.accept()
    except socket.timeout:
        print("the client never connected within 30s (exit status %s); run it "
              "by hand to see what it said" % client.poll())
        client.terminate()
        client.wait(timeout=10)
        return 1
    refusal = require_cookie(connection, cookie)
    if refusal is not None:
        print("REFUSED AT THE GATE: %s" % refusal)
        print("  app_server would close here without replying, so every case "
              "below would report a stall that is really a refused connection.")
        connection.close()
        client.terminate()
        client.wait(timeout=10)
        return 1
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
