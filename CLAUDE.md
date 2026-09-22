# DeBeOS-RDP — working notes

Client for DeBeOS `app_server`'s remote-desktop protocol (`RP_*`, URP/1). Read
`README.md` for status and `PROTOCOL.md` for the wire format before changing
anything in `CrossPlatform/`.

**There is one client: the portable C++ one in `CrossPlatform/`.** The Swift/AppKit
macOS client is frozen in `archive/swift-prototype/` — do not extend it, do not fix
it, and do not treat it as a reference for current behaviour (it predates both the
session cookie and zstd compression). See that directory's README for why.

## The one thing to internalise

`RP_*` ships **drawing commands, not pixels**. `app_server` holds no framebuffer, so
this client *is* the renderer — effectively a reimplementation of Haiku's
`DrawingEngine`. There is no full-frame message to fall back on, and the canvas is
the only copy of the screen.

## Build and test

```sh
./build.sh test          # protocol test suite
./build.sh app           # build the client binaries
./build.sh all           # both (default)
./build.sh run [opts]    # build, then run the best available front end
```

`build.sh` is a thin front end over `CrossPlatform/Makefile`; `make -C CrossPlatform`
works directly too. There is also `CrossPlatform/CMakeLists.txt`.

## Rules that were learned the hard way

**The DeBeOS C++ sources are the only valid oracle.** Checked out at
`~/Projects/Haiku-Graviton`. The in-tree HTML5/JS reference client is *not* an oracle
beyond plain solid fills — matching it silently reproduced four real defects. Cite
`file:line` from the server source in a comment for any behaviour derived from it.

Useful paths in that tree:

- `src/servers/app/drawing/interface/remote/` — the wire format itself
  (`RemoteMessage.h` is the opcode authority)
- `src/servers/app/drawing/Painter/drawing_modes/*.h` — the 11 drawing modes
- `src/servers/app/drawing/PatternHandler.h` — stipples and their view-origin phase
- `src/servers/app/drawing/Painter/Painter.cpp` — gradients, shape playback
- `graviton/docs/remote-desktop-unified-design.md` — design of record, incl. the
  D1-D10 defect table (§8) and the milestone map (§10)

**Assert exact pixel values in tests, never inequalities.** A whole-session colour
shift survived a 171-check suite because render assertions read
`px.r > 200 && px.g < 60`. Anything looser than equality hides systematic error.

**A symmetric fixture hides just as much.** `copyRect` blitted every scroll and
window move vertically mirrored, and its test moved a uniform green square —
invariant under the flip. Give geometry tests an asymmetric subject.

**A test that cannot fail is not a test.** Mutation-test what you add: break the
code, confirm the assertion goes red, restore, confirm green. Beware
*self-consistent* checks in particular — if the expected value is derived from the
same code that produces it, it can never fail. A wire-format check in this project
passed with the opcode mutated from 12 to 13; the fix was pinning a hardcoded golden
vector. This is the single most common way verification here has silently stopped
verifying.

**A scene that only draws does not test a decoder.** Every case added to `--torture`
must state its expected output independently — from the server's own reader, or from
the geometry — and assert it. The five raster defects fixed in `surface.cpp` were all
live while that scene passed, because it drew those opcodes and never looked at the
result.

**Match `app_server`, including where it looks wrong.** Fidelity to the server is the
product. Deliberate quirk-matching so far: `B_GRADIENT_RADIAL_FOCUS` ignores its
focal point, diamond/conic gradients have a fixed 100px extent, and the gradient LUT
reproduces the server's `dist + 1` off-by-one.

**Only all-`0xff` and all-`0x00` patterns are solid.** "All eight bytes equal" is a
different, wrong test that flattens every column-striped stipple.

**Never let the host's colour management touch a pixel.** The server sends raw device
pixels; any conversion is a bug. (The archived Swift client hit this as Generic RGB
silently brightening 200 → 210 — the general lesson survives the implementation.)

## Verifying changes

Unit tests are necessary but not sufficient — most of the interesting bugs were only
visible over the wire.

```sh
# protocol-accurate mock; --torture adds every drawing mode, stipple phases, all
# five gradient kinds, arcs and a BShape ArcTo, and disables the animation loop so
# captures are deterministic. It also self-checks: it reads pixels back with
# RP_READ_BITMAP and compares, so a decode bug that renders *something* still fails.
# --once serves one connection and exits non-zero if a gating case did not hold.
python3 -u tools/rp_mock_server.py --port 10900 --torture --once & MOCK=$!
./CrossPlatform/build/haiku-remote --port 10900 --width 1024 --height 700 \
    --seconds 2 --output /tmp/f.png
wait $MOCK

# against a real instance
./tools/validate-live.sh
```

Run `tools/rp_probe.py` before the client whenever something looks wrong. It is pure
Python sharing no code with the C++ decoder, so it separates "the protocol came up"
from "the renderer works".

## Protocol state you must know about (and two notes that used to be wrong)

**The loopback hop now has authentication.** As of DeBeOS #482, `app_server` requires
`RP_SESSION_COOKIE` (opcode 12: `uint32 method` + length-prefixed secret) as the
**first frame** on its session port, and fails **closed**. The secret is published on
the guest at `/boot/system/settings/remote_desktop/session_cookie.<port>`, mode 0600.
The broker injects it after its own authentication; a **direct** connection must
present it itself. This client does not yet have a `--cookie-file`/`--cookie` option
and that is owed — until it does, direct connections to a current image will be
refused.

> This supersedes the previous note here that "the protocol has no authentication, so
> SSH is the authentication and a tunnel is mandatory." The tunnel is still how you
> reach a loopback-bound port across the network, but it is no longer the *only* thing
> standing between a local process and the session.

**Capabilities are negotiated** via `RP_HELLO`/`RP_HELLO_ACK`. Live bits include
`RP_CAP_STRING_WIDTH_REPLY = 1<<0` and `RP_CAP_COMPRESS_ZSTD = 1<<1` (zstd wire
compression shipped in DeBeOS #416). **Do not advertise a capability you cannot
honour** — the server changes the wire on the strength of the claim, so a false claim
is worse than silence.

**`RP_STRING_WIDTH` is not worth chasing, and the reason is worth reading.** It is a
server-to-client query. The folklore — repeated in this file until it was checked — is
that every unanswered query stalls the server for a full second. **That is stale.**
Since defects D1/D10 were fixed, `RemoteDrawingEngine::StringWidth()` only issues the
query when the client is connected **and** advertised `RP_CAP_STRING_WIDTH_REPLY`;
otherwise it computes from the server's own authoritative font metrics with no wait
(see the comment at `RemoteDrawingEngine.cpp:1071-1077` in the DeBeOS tree). So not
answering costs **nothing**, because the server never asks.

Which inverts the advice: answering is not a missing feature, and for an *instrument*
it is actively undesirable — it replaces the server's authoritative layout metrics with
the client's, so the measurement perturbs what it measures. Advertise the capability
only if you intend to own text layout.

**A refused connection closes with RST, not FIN**, because its pipelined bytes are
unread. Anything that treats `ECONNRESET` as an error rather than a refusal will
misreport.

## AWS

**Test instances are SSM-managed nodes.** A native SSM agent runs on DeBeOS arm64
(`PlatformName` reports `Haiku`), so `aws ssm start-session`, `send-command` and
`AWS-StartPortForwardingSession` all work — port-forwarding over SSM is the normal way
to reach `app_server` on `127.0.0.1:10900` without opening anything.

> This supersedes the previous note that "SSM cannot work here because Haiku has no
> agent." It could not, then it was made to. Prefer SSM over adding network exposure.

**Do not add security-group rules based on a guessed source IP.** Corporate egress
NATs per destination, so `checkip`, `portquiz` and reality give three different
answers. If you must use a CIDR, get the truth from `$SSH_CLIENT` on an instance you
can already reach, and authorize the `/24` because the low octet drifts. An Instance
Connect Endpoint is the alternative that sources traffic inside the VPC.

The canonical AMI is a tag with a one-and-only-one invariant and is **not** necessarily
the newest image — resolve it with
`~/Projects/Haiku-Graviton/graviton/scripts/haiku-canonical check`, never by date. It
moves often.

Nothing needs starting on the guest: `launch_daemon` runs `remote-desktop.sh` at boot,
which exports `TARGET_SCREEN=10900` and brings up `input_server`, Tracker and Deskbar.
`app_server` binds **127.0.0.1 only, deliberately.**

**Ask before terminating instances or widening security groups.** Several long-lived
instances belong to other work at any given time.
