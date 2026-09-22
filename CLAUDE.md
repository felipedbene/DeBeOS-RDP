# HaikuRemote — working notes

Native macOS client for Haiku's `app_server` remote-desktop protocol. Read
`README.md` for status and `PROTOCOL.md` for the wire format before changing
anything in `Sources/HaikuRemoteCore/`.

## The one thing to internalise

`RP_*` ships **drawing commands, not pixels**. `app_server` holds no framebuffer,
so this client *is* the renderer — effectively a reimplementation of Haiku's
`DrawingEngine` on CoreGraphics. There is no full-frame message to fall back on,
and the canvas is the only copy of the screen.

## Build and test

SwiftPM cannot run here (Command Line Tools' `libPackageDescription.dylib`
exports no `Package.init`). Use `build.sh`:

```sh
./build.sh test     # 253-check suite, no XCTest
./build.sh app      # build/HaikuRemote.app
./build.sh icon     # regenerate the .icns from tools/make-icon.swift
./build.sh install  # build, then replace /Applications/HaikuRemote.app
./build.sh all
```

Use `install` rather than `cp -R`; a hand copy once left `/Applications` behind
`build/` and it read as the app breaking. After changing `LSUIElement` or the
icon, run `lsregister -f` on the bundle — LaunchServices caches that metadata and
the symptom is an app with a menu bar but no Dock tile.

`Package.swift` is kept only for someone with full Xcode. The test target links
`Sources/HaikuRemoteCore/*.swift` directly, so **logic that needs testing must
live in Core, not in the AppKit layer** — that is why `ReconnectPolicy` is a
separate type rather than methods on `AppDelegate`.

## Rules that were learned the hard way

**The Haiku C++ sources are the only valid oracle.** They are checked out at
`~/Projects/Haiku-Graviton`. The in-tree HTML5/JS reference client is *not* an
oracle beyond plain solid fills — matching it silently reproduced four real
defects. Cite `file:line` from the Haiku source in a comment for any behaviour
derived from it; the existing code does this throughout, e.g.
`DrawingModeSelect.h:31`, `PatternHandler.h:162`, `Painter.cpp:2101`.

Useful paths:

- `src/servers/app/drawing/Painter/drawing_modes/*.h` — the 11 drawing modes
- `src/servers/app/drawing/PatternHandler.h` — stipples, and their view-origin phase
- `src/servers/app/drawing/Painter/Painter.cpp` — gradients, shape playback
- `src/servers/app/drawing/interface/remote/` — the wire format itself

**Assert exact pixel values in tests, never inequalities.** A whole-session
colour shift survived a 171-check suite because render assertions read
`px.r > 200 && px.g < 60`. Anything looser than equality hides systematic error.
A *symmetric fixture* hides just as much: `copyRect` blitted every scroll and
window move vertically mirrored, and its test moved a uniform green square —
invariant under the flip. Give geometry tests an asymmetric subject.

**Every `CGImage` drawn into the canvas needs a local y-flip.**
`CGContextDrawImage` puts row 0 on the destination's max-y edge, which under the
canvas' flipped CTM is the *bottom*. `drawBitmap` and `copyRect` both do the
translate/translate/scale(1,-1) dance; anything new that draws an image must too.

**Never let CoreGraphics colour-manage.** Build every `CGColor` and `CGGradient`
in the canvas' own `DeviceRGB`. `CGColor(red:green:blue:alpha:)` is Generic RGB
and silently brightens every value (200 → 210). Haiku sends raw device pixels;
any conversion is a bug.

**Match `app_server`, including where it looks wrong.** Fidelity to the server is
the product. Deliberate quirk-matching so far: `B_GRADIENT_RADIAL_FOCUS` ignores
its focal point, diamond/conic gradients have a fixed 100px extent, and the
gradient LUT reproduces Haiku's `dist + 1` off-by-one.

**Only all-`0xff` and all-`0x00` patterns are solid.** "All eight bytes equal" is
a different, wrong test that flattens every column-striped stipple.

## Verifying changes

Unit tests are necessary but not sufficient — most of the interesting bugs were
only visible over the wire.

```sh
# protocol-accurate mock; --torture adds every drawing mode, stipple phases,
# all five gradient kinds, arcs and a BShape ArcTo, and disables the animation
# loop so captures are deterministic
python3 -u tools/rp_mock_server.py --port 10900 --torture &
./build/HaikuRemote --capture /tmp/f.png --port 10900 --width 1024 --height 700

# --torture also self-checks: it reads pixels back with RP_READ_BITMAP and
# compares them, so a decode bug that renders *something* still fails. --once
# serves one connection and exits non-zero if a gating case did not hold.
python3 -u tools/rp_mock_server.py --port 10900 --torture --once & MOCK=$!
./CrossPlatform/build/haiku-remote --port 10900 --width 1024 --height 700 \
    --seconds 2 --output /tmp/f.png
wait $MOCK

# against a real instance (prefers the Instance Connect Endpoint)
./tools/validate-live.sh
```

**A scene that only draws does not test a decoder.** Every case added to
`--torture` should state its expected output independently — from Haiku's own
reader, or from the geometry — and assert it. The five raster defects fixed in
`surface.cpp` were all live while this scene passed, because it drew those
opcodes and never looked at the result.

Run `tools/rp_probe.py` before the client whenever something looks wrong. It is
pure Python sharing no code with the Swift decoder, so it separates "the protocol
came up" from "the renderer works".

## AWS

**Do not add security-group rules based on a guessed source IP.** Corporate
egress NATs per destination, so `checkip`, `portquiz` and reality give three
different answers. Use the Instance Connect Endpoint
(`eice-0fdfd2611f6942f31`, endpoint SG `sg-0af2d8ca28fa9f5f7`), which sources
traffic inside the VPC and needs no agent on the guest — SSM cannot work here
because Haiku has no agent. If you must use a CIDR, get the truth from
`$SSH_CLIENT` on an instance you can already reach, and authorize the `/24`
because the low octet drifts.

Standing resources for this project:

| Thing | Id |
|---|---|
| test instance | `i-09eee41853815abfd` (t4g.medium, latest canonical) |
| instance SG | `sg-0974fc094328104fa` (`haiku-rdclient-ssh`) |
| endpoint / its SG | `eice-0fdfd2611f6942f31` / `sg-0af2d8ca28fa9f5f7` |
| keypair | `haiku-rdclient` → `~/.ssh/haiku-rdclient-ed25519` (ed25519 only; RSA is rejected) |
| SSH user | `baron`, remote port `10900` |

The canonical AMI is a tag with a one-and-only-one invariant and is **not**
necessarily the newest image — resolve it with
`~/Projects/Haiku-Graviton/graviton/scripts/haiku-canonical check`. It has moved
three times in two days.

The app has two connection modes, chosen in Settings: **Direct**
(`client → host:remotePort`, no ssh) and **SSH tunnel**
(`client → 127.0.0.1:localPort → ssh → host:remotePort`, app-managed). Direct mode
cannot reach the EC2 images — `app_server` binds loopback — so those need tunnel
mode. The app knows nothing about EC2 instance IDs or Instance Connect Endpoints;
that lives in `tools/validate-live.sh`.

Nothing needs starting on the guest: `launch_daemon` runs `remote-desktop.sh` at
boot, which exports `TARGET_SCREEN=10900` and brings up `input_server`, Tracker
and Deskbar. `app_server` binds **127.0.0.1 only, deliberately** — the protocol
has no authentication, so SSH is the authentication and a tunnel is mandatory.

Ask before terminating instances or widening security groups.
