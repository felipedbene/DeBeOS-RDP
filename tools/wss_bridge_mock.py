#!/usr/bin/env python3
"""
wss_bridge_mock -- a stand-in for the DeBeOS remote-desktop broker's transport.

Accepts a WebSocket (optionally TLS) connection, requires the broker's
transport-security preamble -- the first binary message must be an RP-framed
RP_AUTHENTICATE(10) carrying the shared token, answered by RP_AUTH_RESULT(11)
with status 0=ok / 1=denied -- and then bridges binary frames to a raw RP_
TCP backend (app_server's remote interface, or rp_mock_server.py). Test rig
only: it implements the broker's wire shape, not its session management,
rate limiting, or settings handling.

Usage:
    # self-signed cert:
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout key.pem -out cert.pem -days 7 -nodes -subj /CN=localhost

    ./wss_bridge_mock.py --listen 10944 --backend 127.0.0.1:10900 \
        --token secret --cert cert.pem --key key.pem

    # plain ws:// (no TLS):
    ./wss_bridge_mock.py --listen 10944 --backend 127.0.0.1:10900 --token secret
"""

import argparse
import base64
import hashlib
import socket
import ssl
import struct
import sys
import threading

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
RP_AUTHENTICATE = 10
RP_AUTH_RESULT = 11
AUTH_METHOD_SHARED_TOKEN = 1


def read_headers(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return None, b""
        data += chunk
        if len(data) > 65536:
            return None, b""
    headers, _, rest = data.partition(b"\r\n\r\n")
    return headers.decode("latin-1"), rest


def handshake(conn):
    headers, rest = read_headers(conn)
    if headers is None:
        return None
    request_line = headers.split("\r\n")[0]
    method, _, target_and_version = request_line.partition(" ")
    target = target_and_version.rsplit(" ", 1)[0]
    fields = {}
    for line in headers.split("\r\n")[1:]:
        name, _, value = line.partition(":")
        fields[name.strip().lower()] = value.strip()

    if method != "GET" or fields.get("upgrade", "").lower() != "websocket":
        conn.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")
        return None

    key = fields.get("sec-websocket-key", "")
    accept = base64.b64encode(
        hashlib.sha1((key + GUID).encode()).digest()).decode()
    response = ("HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n")
    if "binary" in fields.get("sec-websocket-protocol", ""):
        response += "Sec-WebSocket-Protocol: binary\r\n"
    conn.sendall((response + "\r\n").encode())
    print(f"<- upgraded {target}")
    return rest


class WsReader:
    """Parses client-to-server frames (masked) from a byte stream."""

    def __init__(self, conn, leftover):
        self.conn = conn
        self.buffer = bytearray(leftover)

    def _need(self, count):
        while len(self.buffer) < count:
            chunk = self.conn.recv(65536)
            if not chunk:
                return False
            self.buffer.extend(chunk)
        return True

    def next_frame(self):
        """Returns (opcode, payload) or None at end of stream."""
        if not self._need(2):
            return None
        opcode = self.buffer[0] & 0x0F
        masked = (self.buffer[1] & 0x80) != 0
        length = self.buffer[1] & 0x7F
        offset = 2
        if length == 126:
            if not self._need(4):
                return None
            length = struct.unpack(">H", self.buffer[2:4])[0]
            offset = 4
        elif length == 127:
            if not self._need(10):
                return None
            length = struct.unpack(">Q", self.buffer[2:10])[0]
            offset = 10
        mask = b"\0\0\0\0"
        if masked:
            if not self._need(offset + 4):
                return None
            mask = bytes(self.buffer[offset:offset + 4])
            offset += 4
        if not self._need(offset + length):
            return None
        payload = bytes(b ^ mask[i % 4]
                        for i, b in enumerate(
                            self.buffer[offset:offset + length]))
        del self.buffer[:offset + length]
        return opcode, payload


def send_auth_result(conn, status):
    body = struct.pack("<HII", RP_AUTH_RESULT, 10, status)
    conn.sendall(bytes([0x82, len(body)]) + body)


def authenticate(reader, conn, expected_token):
    """Requires the RP_AUTHENTICATE preamble; returns pipelined session bytes
    to forward, or None when authentication failed."""
    stream = bytearray()
    while True:
        if len(stream) >= 6:
            code, total = struct.unpack("<HI", stream[:6])
            if code != RP_AUTHENTICATE or total < 14 or total > 4096:
                print("!! first message is not a valid RP_AUTHENTICATE")
                send_auth_result(conn, 1)
                return None
            if len(stream) >= total:
                break
        frame = reader.next_frame()
        if frame is None:
            return None
        opcode, payload = frame
        if opcode in (0x0, 0x1, 0x2):
            stream.extend(payload)
        elif opcode == 0x9:
            conn.sendall(bytes([0x8A, len(payload)]) + payload)
        elif opcode == 0x8:
            return None
    method, token_length = struct.unpack("<II", stream[6:14])
    token = bytes(stream[14:total])
    if (method != AUTH_METHOD_SHARED_TOKEN or 14 + token_length != total
            or token.decode("latin-1") != expected_token):
        print("!! authentication denied")
        send_auth_result(conn, 1)
        conn.sendall(b"\x88\x02\x03\xf0")  # close 1008
        return None
    send_auth_result(conn, 0)
    return bytes(stream[total:])


def ws_to_backend(reader, conn, backend):
    while True:
        frame = reader.next_frame()
        if frame is None:
            return
        opcode, payload = frame
        if opcode in (0x0, 0x1, 0x2):
            backend.sendall(payload)
        elif opcode == 0x9:  # ping -> pong
            conn.sendall(bytes([0x8A, len(payload)]) + payload)
        elif opcode == 0x8:  # close
            conn.sendall(b"\x88\x00")
            return


def backend_to_ws(backend, conn):
    try:
        while True:
            data = backend.recv(65536)
            if not data:
                conn.sendall(b"\x88\x00")
                return
            if len(data) < 126:
                header = bytes([0x82, len(data)])
            elif len(data) <= 0xFFFF:
                header = b"\x82\x7e" + struct.pack(">H", len(data))
            else:
                header = b"\x82\x7f" + struct.pack(">Q", len(data))
            conn.sendall(header + data)
    except OSError:
        # The client side went away first; the serving thread owns cleanup.
        return


def serve(conn, address, backend_address, token):
    print(f"-- connection from {address}")
    try:
        leftover = handshake(conn)
        if leftover is None:
            return
        reader = WsReader(conn, leftover)
        pipelined = authenticate(reader, conn, token)
        if pipelined is None:
            return
        backend = socket.create_connection(backend_address)
        backend.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if pipelined:
            backend.sendall(pipelined)
        pump = threading.Thread(target=backend_to_ws, args=(backend, conn),
                                daemon=True)
        pump.start()
        ws_to_backend(reader, conn, backend)
        backend.close()
    except OSError as error:
        print(f"-- {address}: {error}")
    finally:
        try:
            conn.close()
        except OSError:
            pass
        print(f"-- connection from {address} ended")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", type=int, default=10944)
    parser.add_argument("--backend", default="127.0.0.1:10900")
    parser.add_argument("--token", required=True,
                        help="the shared token RP_AUTHENTICATE must carry")
    parser.add_argument("--cert", help="TLS certificate (PEM); enables wss")
    parser.add_argument("--key", help="TLS private key (PEM)")
    args = parser.parse_args()

    backend_host, _, backend_port = args.backend.rpartition(":")
    backend_address = (backend_host, int(backend_port))

    context = None
    if args.cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.cert, args.key or args.cert)

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", args.listen))
    listener.listen(4)
    scheme = "wss" if context else "ws"
    print(f"listening on {scheme}://127.0.0.1:{args.listen} -> "
          f"tcp://{args.backend} (RP_AUTHENTICATE required)")
    while True:
        conn, address = listener.accept()
        if context is not None:
            try:
                conn = context.wrap_socket(conn, server_side=True)
            except ssl.SSLError as error:
                print(f"-- TLS handshake with {address} failed: {error}")
                conn.close()
                continue
        threading.Thread(target=serve,
                         args=(conn, address, backend_address, args.token),
                         daemon=True).start()


if __name__ == "__main__":
    sys.exit(main())
