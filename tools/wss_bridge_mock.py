#!/usr/bin/env python3
"""
wss_bridge_mock -- a stand-in for the DeBeOS remote-desktop broker's transport.

Accepts a WebSocket (optionally TLS) connection, checks the session token in
the request target's query string, and bridges binary frames to a raw RP_ TCP
backend (app_server's remote interface, or rp_mock_server.py). Test rig only:
it implements the transport shape -- wss + token-in-query + RP_ bytes in
binary frames -- not the real broker's session management.

Usage:
    # self-signed cert:
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout key.pem -out cert.pem -days 7 -nodes -subj /CN=localhost

    ./wss_bridge_mock.py --listen 10944 --backend 127.0.0.1:10900 \
        --token secret --cert cert.pem --key key.pem

    # plain ws:// (no TLS):
    ./wss_bridge_mock.py --listen 10944 --backend 127.0.0.1:10900
"""

import argparse
import base64
import hashlib
import socket
import ssl
import struct
import sys
import threading
from urllib.parse import parse_qs, urlsplit

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


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


def handshake(conn, expected_token):
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
    if expected_token is not None:
        query = parse_qs(urlsplit(target).query)
        if query.get("token", [None])[0] != expected_token:
            print(f"!! rejected {target}: bad or missing token")
            conn.sendall(b"HTTP/1.1 403 Forbidden\r\n\r\n")
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


def recv_exact(conn, count):
    data = b""
    while len(data) < count:
        chunk = conn.recv(count - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def ws_to_backend(conn, backend, leftover):
    buffer = bytearray(leftover)

    def need(count):
        while len(buffer) < count:
            chunk = conn.recv(65536)
            if not chunk:
                return False
            buffer.extend(chunk)
        return True

    while True:
        if not need(2):
            return
        opcode = buffer[0] & 0x0F
        masked = (buffer[1] & 0x80) != 0
        length = buffer[1] & 0x7F
        offset = 2
        if length == 126:
            if not need(4):
                return
            length = struct.unpack(">H", buffer[2:4])[0]
            offset = 4
        elif length == 127:
            if not need(10):
                return
            length = struct.unpack(">Q", buffer[2:10])[0]
            offset = 10
        mask = b"\0\0\0\0"
        if masked:
            if not need(offset + 4):
                return
            mask = bytes(buffer[offset:offset + 4])
            offset += 4
        if not need(offset + length):
            return
        payload = bytes(b ^ mask[i % 4]
                        for i, b in enumerate(buffer[offset:offset + length]))
        del buffer[:offset + length]

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
        leftover = handshake(conn, token)
        if leftover is None:
            return
        backend = socket.create_connection(backend_address)
        backend.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        pump = threading.Thread(target=backend_to_ws, args=(backend, conn),
                                daemon=True)
        pump.start()
        ws_to_backend(conn, backend, leftover)
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
    parser.add_argument("--token", default=None,
                        help="require this token in the request query")
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
          f"tcp://{args.backend}"
          + (f" (token required)" if args.token else ""))
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
