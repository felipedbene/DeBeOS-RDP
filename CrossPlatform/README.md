# Haiku Remote cross-platform client

This is a new C++20 client for Haiku's `RP_*` remote drawing protocol. It is
kept separate from the working native macOS client so the latter can remain a
pixel-accurate behavioral oracle while this implementation grows.

The core has no window-system or Apple-framework dependency:

- `protocol` safely frames and decodes the packed little-endian wire format.
- `input_encoder` creates platform-neutral mouse, key, wheel, and modifier messages.
- `session` owns handshake and sticky per-token drawing state.
- `surface` is an opaque BGRA software framebuffer with Haiku drawing modes.
- `text_engine` shapes UTF-8 with HarfBuzz and rasterizes it with FreeType.
- `tcp_socket` has POSIX and Winsock implementations.
- `sdl_main` is the shared Windows, macOS, and Linux interactive presenter.
- `x11_main` is the interactive Linux presenter with keyboard and pointer input.
- `png_writer` provides deterministic headless captures and test automation.

Build and test with the lightweight local Makefile:

```sh
make -C CrossPlatform test
make -C CrossPlatform
```

From the repository root, `./build.sh`, `./build.sh test`, and
`./build.sh run --host ...` automatically select this client on non-macOS
hosts. When X11 is available, `run` opens the interactive window frontend;
otherwise it uses the PNG capture frontend. On macOS, the script continues to
build the native Swift client.

Use `--stats` with the X11 frontend to show rolling performance measurements in
the window title and stderr:

```sh
CrossPlatform/build/haiku-remote-x11 --host 127.0.0.1 --port 10900 --stats
```

The report separates protocol decode time, framebuffer copy time, X11
submission, redraw batching, server response latency, and input-to-present
latency. Latency samples are approximate because the drawing protocol does not
tag updates with the input event that caused them.

The same targets are available through CMake for Windows, Linux, and macOS:

```sh
cmake -S CrossPlatform -B CrossPlatform/out \
  -DHAIKU_REMOTE_FETCH_SDL2=ON
cmake --build CrossPlatform/out
ctest --test-dir CrossPlatform/out
```

`HAIKU_REMOTE_FETCH_SDL2=ON` downloads the pinned SDL 2.30.9 source release and
builds `haiku-remote-gui`, so a system SDL package is not required. Leave it off
to use an installed SDL2 package, or to build only the headless and optional X11
frontends.

Run the portable interactive client with:

```sh
CrossPlatform/out/haiku-remote-gui --host 127.0.0.1 --port 10900
```

## Transports

Every frontend speaks the `RP_*` protocol over a pluggable transport:

- **Raw TCP** (default): `--host HOST --port PORT`, or `--url tcp://HOST:PORT`.
  This is the direct connection to `app_server`'s remote interface, for
  loopback or an SSH tunnel. **It is authenticated**: `app_server` requires its
  per-boot *session cookie* as the very first frame of every connection and
  drops any connection that opens with anything else, so a direct client must
  present the cookie itself (see PROTOCOL.md §1.4). The tunnel provides
  confidentiality; the cookie provides authentication.
- **WebSocket / WebSocket-over-TLS**: `--url ws://…` or `--url wss://…`
  connects to the DeBeOS remote-desktop broker (`remote_broker`, default port
  10902). The `RP_*` byte stream rides in binary frames (subprotocol
  `binary`): each client message is sent as one frame, and received frame
  payloads are concatenated back into the stream, so the server may batch or
  split messages across frames freely.

**On the port in the examples above.** They use `10900` because that is what
the DeBeOS images configure (`TARGET_SCREEN=10900`, set for the whole user
session by `graviton/ssh/files/remote-desktop.sh:46-47`), and it matches this
client's own default (`CrossPlatform/include/haiku_remote/transport.hpp:53`).
It is *not* `app_server`'s built-in fallback, which is `10901`
(`RemoteHWInterface.cpp:113`) and applies only when nothing sets the target
port. The cookie file name is derived from whichever port the listener actually
bound, so check the port before reaching for the file.

Direct-connection options (raw TCP only; a `ws://`/`wss://` connection must
send **no** cookie, because the broker reads the file itself and presents the
cookie after its own token authentication has succeeded):

- `--cookie HEX` supplies the session cookie inline.
- `--cookie-file PATH` reads it from a file, trimming trailing whitespace —
  the published file ends in a newline that is *not* part of the secret.

`app_server` publishes the cookie, owner-readable only, at
`<system settings>/remote_desktop/session_cookie.<listen port>`, i.e.
`/boot/system/settings/remote_desktop/session_cookie.10900` for a Desktop
listening on 10900. The number in that name is **`app_server`'s own listener
port**, which through a tunnel is not the port this client connects to:
`ssh -L 19900:127.0.0.1:10900` leaves the file called `session_cookie.10900`
while the client dials 19900, and looking for `session_cookie.19900` on the
server finds nothing. In the DeBeOS repo,
`graviton/scripts/haiku-remote-desktop` reads the cookie off the server for
you. It is minted fresh each time the interface is created, so
it changes across a reboot or a restart of the remote Desktop; copy it out over
the same SSH session that carries the tunnel. These two options are part of the
change that added cookie support to this client — a build predating it cannot
open a direct connection to a current `app_server` at all.

Broker options (ignored by raw TCP):

- `--token TOKEN` authenticates the connection. The broker requires
  `RP_AUTHENTICATE` as the very first message on the WebSocket and proxies
  nothing to the session until it has answered `RP_AUTH_RESULT` with success;
  the transport speaks that preamble during connect, so the session layer
  never sees it. The token is the content of the broker's
  `/boot/system/settings/remote_desktop/token` file.
- `--pin-sha256 DIGEST` pins the broker's TLS identity to its certificate's
  SHA-256 fingerprint — exactly what the broker writes to
  `broker.fingerprint` beside its key on first run (hex; an optional
  `sha256:` prefix, colon separators, or base64 are also accepted;
  `openssl x509 -in cert.pem -noout -fingerprint -sha256` prints the same
  value). With a pin, the fingerprint alone authenticates the server, so the
  broker's self-signed certificate needs no CA.
- `--ca-file FILE.pem` verifies the certificate chain against the given
  anchor instead of the system store (when no pin is set).
- `--insecure` disables server authentication entirely; testing only.

Builds without OpenSSL development files keep the raw TCP transport and
reject `ws://`/`wss://` URLs with a clear error.

## Exit status

Every frontend uses one scheme, also printed by `--help`:

| Code | Meaning |
| ---- | ------- |
| 0 | the session ran |
| 1 | the session failed, or the server refused it — a **wrong or stale cookie** lands here: the socket opens, the gate drops it on the wire, and nothing is drawn |
| 2 | bad arguments |
| 3 | **no credential supplied** — the connection needs a session cookie (direct) or a token (broker) and none was given, so no socket was opened |

1 and 3 are the two refusals a harness has to tell apart, and the split is
local-versus-remote: 3 is a misinvocation here, fixed by passing
`--cookie-file`/`--token-file`; 1 happened out on the wire and the remedy is
somewhere else. `tools/rp_probe.py` uses 3 for the same case.

On connect the client also performs the URP/1 `RP_HELLO`/`RP_HELLO_ACK`
capability handshake, advertising `RP_CAP_STRING_WIDTH_REPLY` (it measures
text itself), so a capability-aware server routes `RP_STRING_WIDTH` to it and
never stalls on a client that cannot answer. Pre-handshake servers ignore the
message.

Run against the protocol mock:

```sh
python3 -u tools/rp_mock_server.py --port 10900 &
CrossPlatform/build/haiku-remote --host 127.0.0.1 --port 10900 \
  --width 1024 --height 700 --seconds 2 --output /tmp/haiku-remote.png
```

No cookie appears in that command because the mock has no candidate gate: it
never looks at a session-cookie frame. Connecting without one therefore proves
nothing about a real `app_server`, which refuses exactly that connection.

The X11 frontend has been live-validated against Haiku on Linux, including
keyboard and pointer input, incremental redraws, text rendering, and performance
instrumentation. The SDL frontend has been integration-tested on ARM64 Linux
against the fragmented protocol mock; the same source uses SDL's Win32 and Cocoa
backends on Windows and macOS. Native builds on those two operating systems are
still required before publishing platform-specific binaries.

The renderer handles every server-to-client drawing opcode currently defined by
the protocol, including Beziers, all gradient shape variants, offset text,
affine-transformed geometry and bitmaps, and variable-width patterned strokes.
The client still logs the first occurrence of unknown future opcodes.

`RP_SET_CURSOR`, `RP_SET_CURSOR_VISIBLE` and `RP_MOVE_CURSOR_TO` are decoded and
kept as session state, reachable through `Session::cursor()`. The X11 frontend
turns the server's cursor into a native `XCreatePixmapCursor` -- exact shape and
hotspot, colour reduced to two tones because core Xlib has no ARGB cursor -- and
replaces it with an empty cursor while the server says the pointer is hidden.
`haiku-remote --draw-cursor` composites the cursor into the captured PNG, at full
colour and with alpha, so a headless capture can show it too. The SDL frontend
does not apply the cursor yet.
