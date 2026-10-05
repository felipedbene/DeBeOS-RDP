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
make test
make
```

There are three front ends and they are built conditionally on what the host
provides: `haiku-remote` (headless PNG capture) always builds; `haiku-remote-x11`
builds when X11 development files are present; `haiku-remote-gui` (the SDL front
end) builds only when SDL2 development files are present. A build on a host
without one of these does **not** fail — but it is no longer silent about it.
`make all` ends by reporting exactly which front ends were built
and which were skipped, e.g.:

```
=== client frontends ===
haiku-remote:     built (headless / PNG capture)
haiku-remote-gui: SKIPPED (SDL2 development files not found; install SDL2 to build the SDL frontend)
haiku-remote-x11: built (X11 frontend)
built 2 of 3 frontends (SKIPPED:haiku-remote-gui)
```

This exists because a silently-skipped target reads exactly like a passing one:
`src/sdl_main.cpp` was shipped but compiled by nothing, so an edit to it could go
unverified. On a host that lacks the SDL2 *link* libraries but has the SDL2
*headers*, `make syntax-check` (or `./build.sh syntax-check`)
runs `g++ -fsyntax-only` over `src/sdl_main.cpp` so a typo there is still caught;
when even the headers are absent it says so plainly rather than passing silently.

From the repository root, `./build.sh`, `./build.sh test`, and
`./build.sh run --host ...` automatically select this client on non-macOS
hosts. When X11 is available, `run` opens the interactive window frontend;
otherwise it uses the PNG capture frontend. On macOS, the script continues to
build the native Swift client.

Use `--stats` with the X11 frontend to show rolling performance measurements in
the window title and stderr:

```sh
build/haiku-remote-x11 --host 127.0.0.1 --port 10900 --stats
```

The report separates protocol decode time, framebuffer copy time, X11
submission, redraw batching, server response latency, and input-to-present
latency. Latency samples are approximate because the drawing protocol does not
tag updates with the input event that caused them.

The same targets are available through CMake for Windows, Linux, and macOS:

```sh
cmake -S . -B out \
  -DHAIKU_REMOTE_FETCH_SDL2=ON
cmake --build out
ctest --test-dir out
```

`HAIKU_REMOTE_FETCH_SDL2=ON` downloads the pinned SDL 2.30.9 source release and
builds `haiku-remote-gui`, so a system SDL package is not required. Leave it off
to use an installed SDL2 package, or to build only the headless and optional X11
frontends.

Run the portable interactive client with:

```sh
out/haiku-remote-gui --host 127.0.0.1 --port 10900
```

## Connection library

Launched **with** a connection on the command line (`--host`, `--url`, or a
credential flag), the SDL and X11 frontends connect straight away — the
automation and debugging path is unchanged. Launched with **no** connection
target, they open the *connection library* instead: a list of saved connection
profiles with a prominent **New connection** button, a search box, and
per-profile **Connect / Edit / Duplicate / Delete** actions. Favorites sort
first, then the most recently connected. Choosing **Connect** opens the session;
closing the library window exits.

Profiles persist as JSON in a per-user config directory:

| OS      | Location                                                     |
|---------|--------------------------------------------------------------|
| Linux   | `$XDG_CONFIG_HOME/haiku-remote/connections.json` (else `~/.config/haiku-remote/…`) |
| macOS   | `~/Library/Application Support/Haiku Remote/connections.json` |
| Windows | `%APPDATA%\Haiku Remote\connections.json`                    |

### Cross-platform format and path parity (Part 4)

The three operating systems share **one** profile format and **one** workflow;
only the directory above differs. Two things make that a tested guarantee rather
than a hope:

- **The on-disk schema (version 1) is portable by construction.** `serialize()`
  and `parse()` in `profile_store.cpp` have no platform conditionals, so the
  `connections.json` a client writes for a given set of profiles is **byte-
  identical** on Linux, macOS, and Windows, and loads unchanged on any of them.
  Paths are stored *verbatim* and JSON-escaped — a Windows profile keeps its
  `C:\Users\…\id_ed25519` backslashes, a POSIX profile keeps its leading `~` —
  because a path is only resolved (tilde-expanded) at the moment it is used, not
  when it is stored. Part 4 **confirmed** this parity rather than changing the
  format, so there is **no schema version bump**: a v1 file written by any
  earlier part still loads. `tests/xplatform_tests.cpp` pins the exact serialized
  bytes to a golden string and round-trips a Windows-path profile byte-for-byte.
- **The per-OS directory rule is resolved by data, not by `#ifdef`.** The rule
  lives in one pure function, `ProfileStore::config_dir_for(platform, env)`,
  which takes the target OS as a parameter and reads the environment through a
  supplied lookup. `config_dir()` is a thin wrapper that passes the host's own
  platform and `std::getenv`. Because the OS and the environment are both
  parameters, each platform's rule is unit-tested from a single Linux host with
  a fake environment: `$XDG_CONFIG_HOME` honoured (and an empty value treated as
  unset), the `~/.config` fallback, macOS's `Application Support` folder, and
  `%APPDATA%\Haiku Remote` — plus the real `config_dir()` exercised against a
  live `XDG_CONFIG_HOME` override. The file name is `connections.json` on every
  platform.

The library is layered so none of it touches the protocol core:

- `connection_profile` — the profile schema and its pure validator (no UI, no
  filesystem). A profile stores a connection mode (`direct`, `ssh` tunnel, or
  `wss` broker) but never a secret: only a path to an identity file and a
  *source* for the session cookie, never the bytes.
- `profile_store` — the JSON envelope, atomic/durable save, corrupt-file
  recovery, the per-OS location, and the favorite-first ordering.
- `profile_library` — the in-memory model and its add / edit / duplicate /
  delete / mark-connected / search operations.
- `profile_launch` — the one *pure* bridge from a saved profile to a launch
  plan (`direct`, `ssh` tunnel, or `wss` broker, with the broker→tunnel
  fallback).
- `managed_transport` — the side-effecting half: it spawns and **owns** the SSH
  child that forwards `app_server`'s loopback port (or fetches the broker's
  token and certificate over SSH), and tears all of it down when the owning
  handle drops. Process spawn/kill is abstracted behind `ManagedProcess`, a
  move-only RAII handle whose destructor terminates and reaps its child so an
  `ssh` child is never orphaned. The POSIX implementation (fork/exec + waitpid)
  is the default everywhere and is the only one built and tested here; the
  Windows implementation (`CreateProcess` + a kill-on-close Job Object) is in
  the tree but **unverified** — see *Windows and SDL verification gaps* below.
- `library_screen` — the immediate-mode UI, drawn with the same `Surface` +
  `text_engine` software renderer the session uses and no windowing library, so
  the SDL and X11 frontends share it verbatim and only translate their events.

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
client's own default (`include/haiku_remote/transport.hpp:53`).
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
build/haiku-remote --host 127.0.0.1 --port 10900 \
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

## Windows and SDL verification gaps

Part 4 brought the profile format, the config-path rules, and the managed-
transport interface to parity across the three operating systems, and everything
that can be verified on a Linux host **is** verified: the whole core compiles,
all seven test suites pass (`core / render / profile / library / tunnel /
connect / xplatform`), and the cross-platform format and per-OS path rules are
driven by data so they are exercised here without being on macOS or Windows.

Two things are deliberately **not** claimed to work, because this builder has no
way to check them. They are the remaining steps for a maintainer on those
platforms:

- **The Windows `ManagedProcess` is written but unverified.** `src/managed_
  transport.cpp`'s `#ifdef _WIN32` path implements process spawn + guaranteed
  teardown with `CreateProcess` and a `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` Job
  Object (so the child dies with its handle, or with this process, and is never
  orphaned), plus the Winsock forms of `pick_free_local_port`, the loopback
  readiness probe, and `SystemCommandRunner`. It mirrors the POSIX
  SIGTERM→wait→reap contract and keeps the identical move-only RAII ownership.
  **It has never been compiled or run** — there is no Windows toolchain and no
  MinGW cross-compiler on the build host, so not even a syntax check was
  possible. A maintainer must build it with MSVC or MinGW and run the
  `tunnel`/`xplatform` suites (the tunnel suite's process-reap and
  unreachable-tunnel tests are `#ifdef`'d to POSIX today and need Windows
  equivalents) and confirm, with Task Manager or `handle.exe`, that no `ssh.exe`
  survives a disconnect or an app exit. The POSIX path is untouched and remains
  the default and the only tested one.
- **The SDL frontend still cannot be built here.** `src/sdl_main.cpp` needs
  SDL2, which is absent on this host, so `make` reports `haiku-remote-gui:
  SKIPPED` loudly rather than silently. Where the SDL2 *headers* are present a
  maintainer can `make syntax-check` to parse it; a real build and a live
  session against `app_server` on Windows (Win32 backend) and macOS (Cocoa
  backend) are still required before publishing platform-specific binaries. The
  SDL frontend also does not yet apply the server cursor.
