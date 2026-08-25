# HaikuRemote — native macOS client for Haiku's `app_server` remote protocol

A menu-bar macOS app that speaks Haiku's `RP_*` remote-desktop protocol directly
over a raw TCP socket through an `ssh -L` forward, replacing the
websockify + browser-tab path.

**Read [`PROTOCOL.md`](PROTOCOL.md) first.** It documents the wire protocol as it
actually exists in the `Haiku-Graviton` sources, and it contains one finding that
reshapes the whole project — see below.

---

## The headline finding

`RP_*` is **not a framebuffer protocol.** It is a remote *drawing-command*
protocol, closer to X11 or RDP orders than to VNC/RFB.

`app_server` holds no framebuffer at all (`FrontBuffer()` returns `NULL`). It
ships the drawing calls themselves — "stroke this rect", "fill this region",
"draw this string", "play back this Bézier path" — and **the client is the
renderer**. There is no full-frame message, no dirty-rect tile encoding, and no
compression anywhere.

That means the original phase plan's Phase 2 ("decode the initial full-frame
message") and Phase 3 ("port the delta/dirty-rect handling") describe things that
do not exist. What those phases actually require is a reimplementation of Haiku's
`DrawingEngine` on top of CoreGraphics: ~70 ops, 11 drawing modes, 8×8 stipple
patterns, 5 gradient types, 8 bitmap colour spaces, path playback — **and
client-side text layout, because the server blocks waiting for the client to
measure strings.** That last point is the protocol's main weakness on a slow
link, and it is per-string, not per-frame.

Two consequences worth knowing before reading the code:

- **Bandwidth is inverted from what you'd expect.** A `RP_FILL_RECT_COLOR` is 30
  bytes; a full-screen 1920×1080 bitmap is 8.3 MB uncompressed. Don't optimise
  the vector path; avoid bitmap traffic. This is why your image forces a flat
  desktop colour.
- **The client's canvas is the only copy of the screen.** Nothing can resend it.
  The one resync lever is re-sending `RP_UPDATE_DISPLAY_MODE`, which triggers
  `_NotifyScreenChanged()` and a full repaint.

---

## Status by phase

| Phase | Status |
|---|---|
| 0 — investigate, write `PROTOCOL.md` | Done. Sourced from the real driver + reference JS client. |
| 1 — validate raw-TCP transport | **Live-validated** against a real instance (2026-08-23). |
| 2 — static frame render | **Live-validated**: renders a real Haiku desktop, no unhandled ops. |
| 3 — incremental updates | Mock-verified, plus live window drags and menus driven by hand. |
| 4 — input round-trip | Mock-verified for every event type; **live-validated interactively** — clicks and keystrokes drive a real session. |
| 5 — tunnel automation + menu bar | Done, lifecycle guarantees tested. |
| 6 — render fidelity against `app_server` | Done. All 11 drawing modes, stipple phase, all 5 gradient kinds and shape arcs now match Haiku's own `Painter`, verified numerically over the wire. |
| 7 — unattended operation | Done. Auto-reconnect with backoff, and resolution changes without a reconnect. |

### What phase 6 changed, and why it mattered

Phases 2–5 were validated against the *reference JS client*, which turns out to
be the wrong oracle for anything but plain fills. Re-deriving the drawing model
from `app_server`'s own sources found four defects that no amount of comparing
against the browser demo would have surfaced — two of them in code that looked
finished:

1. **Every colour was wrong.** `CGColor(red:green:blue:alpha:)` creates a colour
   in *Generic RGB* (gamma 1.8); the canvas is DeviceRGB. Drawing one into the
   other colour-matches it, so a desktop background of `(51,102,152)` was landing
   as `(64,116,163)`. Every single colour in the session was shifted. Haiku sends
   raw device pixels, so the fix is to build colours in the canvas' own space and
   let nothing convert. The old tests missed it because they asserted things like
   `px.r > 200 && px.g < 60` rather than exact values.
2. **`B_OP_MIN`/`B_OP_MAX` were per-channel.** They are not: Haiku compares one
   whole-pixel brightness value and replaces the entire pixel or none of it.
   `CGBlendMode.darken`/`.lighten` can emit a colour present in neither operand.
3. **Vertical-stripe stipples rendered flat.** The "is this pattern solid" test
   was "are all 8 bytes equal", copied from the reference client. That is true of
   every column-striped pattern — eight bytes of `0xf0` is four columns on, four
   off — so those fills silently collapsed to one colour.
4. **Stipples were anchored to the shape, not the screen.** Haiku phases the 8×8
   pattern off the view origin, so abutting fills line up. Tiling from each
   shape's bounding box makes them not.

Three of the eleven drawing modes are conditional per-pixel rewrites gated on the
stipple, and none of the eight non-Porter-Duff modes can be expressed as a
`CGBlendMode`. They are now rasterised to a coverage mask and blended in software
(`SoftBlend.swift`), transcribed macro-for-macro from
`src/servers/app/drawing/Painter/drawing_modes/` with the source cited at each
one. `PROTOCOL.md` §6.5.1 has the full table.

### Phase 1: websockify is only a browser shim

The server contains zero HTTP/WebSocket code; it is a plain `BNetEndpoint`
listener. The reference client uses a WebSocket purely because browsers cannot
open raw TCP sockets. **A raw-socket client is correct; no embedded WebSocket
implementation is needed.**

You had in fact already proven this. `graviton/scripts/haiku-remote-desktop`
opens a raw socket through `ssh -L`, sends `RP_INIT_CONNECTION` as the six bytes
`01 00 06 00 00 00`, and refuses to start websockify unless the server answers.
That check is a precondition of your working browser demo, so it has been passing
every time you've used the tool.

---

## Live validation status

**The client drives a real Haiku desktop.** First achieved 2026-08-23 against
`hrev59996` on the canonical AMI: `rp_probe.py` pulled 384 KB of live drawing
traffic, the client decoded it with **no unhandled ops**, and the GUI ran an
interactive session — opening apps, dragging windows, typing.

The best single piece of evidence is that **DiskUsage renders its donut chart**.
Those segments are arc ops. Before the phase 6 work they were parsed, their points
consumed, and nothing drawn — so that window was an empty grey box, and it still
is in the reference JS client, which does not implement arcs at all.

Getting there needed the network problem below solved. It is kept because the
diagnosis is reusable and the conclusion changed twice.

### History: why this took so long

The following was all true when measured, and the first bullet is **no longer
true** — reachability changed without notice, so re-measure rather than trusting
either answer:

- Launched two Haiku instances from what was then the canonical AMI
  (`ami-0ddfb52be8c83949a`, `haiku-graviton-1787351833`). Canonical is a tag with
  a one-and-only-one invariant, checked with
  `graviton/scripts/haiku-canonical check`, and it is **not** necessarily the
  newest image — see the note on the current canonical below.
- Neither was reachable on port 22. Diagnosed by launching a **stock Amazon Linux
  instance in the same subnet with the same security groups** — that was also
  unreachable, which rules out Haiku, the ENA driver, and the AMI.
- The VPC is fully permissive (IGW route present, default open NACL, security
  group admits my address). A port with *nothing listening* also timed out
  rather than returning RST, so packets never arrive at all.
- `checkip.amazonaws.com` reports `52.94.133.137`, but the address the open
  internet actually sees for raw TCP is `52.46.80.28` — the discrepancy you
  flagged about the VPN/web proxy. Adding the real one did not help either.
- Outbound TCP to port 10900 is blocked outright, while port 22 to
  `github.com` works. So this network permits SSH generally but blocks traffic to
  EC2 public addresses.

### The tunnel is mandatory by design, not by convention

Worth stating outright, because it rules out every clever workaround: the server
binds **loopback only, deliberately**
(`RemoteHWInterface.cpp:77-88`, commit `8446374dda`):

```cpp
// Loopback only, never INADDR_ANY. The remote protocol has no
// authentication and no encryption of any kind: anything that can reach this
// port gets full control of the session, including every keystroke.
// [...] Access is therefore deliberately only possible through an SSH
// tunnel, so that SSH provides the authentication the protocol lacks.
BNetAddress loopback((uint32)htonl(INADDR_LOOPBACK), fListenPort);
```

So port 10900 is not reachable from anywhere but the guest itself. Adding a
security-group rule for it does nothing, and relaying via another host in the VPC
(over SSM, say) still needs an SSH tunnel *and* therefore a private key on that
host. There is no path to a live session that does not go through `ssh -L`.

**Conclusion: corporate egress filtering, not a bug in this project.** From a
network that can reach EC2, Phase 1 should be a single command:

```sh
ssh -N -L 10900:127.0.0.1:10900 baron@<host>     # in one shell
python3 tools/rp_probe.py --port 10900           # in another
```

One instance is **stopped** and waiting for you to validate against, on the
current canonical AMI:

```sh
# start it, then read the NEW public IP
aws ec2 start-instances --region us-west-2 --instance-ids i-05ee26e14ef49540c

aws ec2 describe-instances --region us-west-2 \
  --instance-ids i-05ee26e14ef49540c \
  --query 'Reservations[].Instances[].[InstanceId,State.Name,PublicIpAddress]' \
  --output text
```

**The address changes every time.** These instances have auto-assigned public
IPs, not Elastic IPs, so stopping releases the address and starting allocates a
different one. Read the IP from `describe-instances` each time rather than
trusting anything written down, and re-check the security group still admits your
current egress address, which also moves (see the `checkip` note above).

The two earlier instances (`i-0ba496a81d6e46538` t4g.medium and
`i-051e2a3112387f7cf` c7g.large, on the superseded `ami-0ddfb52be8c83949a`) were
**terminated** on 2026-08-23 along with their root volumes. Nothing was lost with
them: neither had ever been reachable, and the AMI they came from still exists, so
an instance-type A/B can be recreated at any time with

```sh
aws ec2 run-instances --region us-west-2 --image-id <ami> --instance-type c7g.large \
  --subnet-id subnet-0888405da8f10d1b2 \
  --security-group-ids sg-0974fc094328104fa sg-008114891fd207df1 \
  --key-name haiku-rdclient --associate-public-ip-address
```

To stop paying for the remaining one entirely, terminate it the same way.

I also created, for this purpose only: keypair `haiku-rdclient` (private key at
`~/.ssh/haiku-rdclient-ed25519`) and security group `sg-0974fc094328104fa`
(`haiku-rdclient-ssh`). **No existing security group was modified** — the
instances additionally have your `haiku-graviton-ssh` attached. The Amazon Linux
control instance has been terminated. Note the images accept **ed25519 keys
only** (OpenSSH built `--without-openssl`), so the RSA `haiku-graviton.pem` will
not work.

### Current canonical AMI (checked 2026-08-23)

`ami-0d3f218d86ec93745` — `haiku-graviton-1787461126`, baked 05:06 UTC
2026-08-23, `haiku-revision: hrev59996`, `baked-by: cdk-pipeline`. The invariant
holds (exactly one canonical); `ami-0ddfb52be8c83949a` has been un-tagged. Note
the previous candidate `ami-088ccccc0948e0579` carries the *same* `hrev59996`, so
this is a rebake of the same Haiku revision rather than new OS code.

Two things were verified against it without needing a live session:

- **It boots.** A t4g.medium launched from it reached `running` with both EC2
  status checks `ok`. The instance check exercises the guest's own network stack,
  so that is a real signal Haiku came up, not just that the hypervisor did. (Boot
  evidence on Graviton is limited to this: `get-console-screenshot` is
  unsupported on arm64, and Haiku writes nothing to EC2's serial console, so
  `get-console-output` comes back empty.)
- **The wire protocol is unchanged**, so this AMI needs no client changes.
  `RemoteMessage.h` and `RemoteDrawingEngine.cpp` last changed 2025-09-03 (the
  gradient-stroke opcodes `260`–`268`, which this client implements) and
  `RemoteMessage.cpp` in 2019. Everything `PROTOCOL.md` documents still holds at
  `hrev59996`.
- The loopback-bind commit is an ancestor of the baked branch, so **this AMI also
  requires an SSH tunnel** — see above.

What is still unverified on any AMI: that `app_server` actually serves a session
end to end, and that this client renders a real desktop rather than a mock one.
That needs `ssh -L`, and therefore a network that can reach EC2.

An instance on this AMI is stopped and waiting for that:

| Instance | Type | AMI | State |
|---|---|---|---|
| `i-05ee26e14ef49540c` | t4g.medium | `ami-0d3f218d86ec93745` (current canonical) | stopped |

This is now the only instance this project owns.

### How to run the live test

**From a network that can reach EC2** (this one cannot — see above):

```sh
./tools/validate-live.sh --stop-when-done
```

That starts the instance, resolves the address, opens the tunnel, runs the
protocol probe, renders a PNG with the real client, and stops the instance again.
Add `--gui` to hold the tunnel open and drive the menu-bar app instead.

Nothing has to be started inside the guest. `launch_daemon` runs
`remote-desktop.sh` at boot via `/boot/system/settings/launch/remote_desktop`,
which sets `TARGET_SCREEN=10900` for the whole user session and brings up
`input_server`, Tracker and Deskbar against the remote Desktop. A booted instance
is already listening on `127.0.0.1:10900`.

By hand, if you would rather see each step:

```sh
aws ec2 start-instances --region us-west-2 --instance-ids i-05ee26e14ef49540c
aws ec2 wait instance-running --region us-west-2 --instance-ids i-05ee26e14ef49540c
IP=$(aws ec2 describe-instances --region us-west-2 \
  --instance-ids i-05ee26e14ef49540c \
  --query 'Reservations[].Instances[].PublicIpAddress' --output text)

# no source-IP rule is needed: go through the Instance Connect Endpoint
aws ec2-instance-connect open-tunnel --region us-west-2 \
  --instance-id i-09eee41853815abfd --local-port 2222 &

ssh -N -L 10900:127.0.0.1:10900 -p 2222 -i ~/.ssh/haiku-rdclient-ed25519 \
  -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no baron@127.0.0.1 &
python3 tools/rp_probe.py --port 10900                 # protocol first
./build/HaikuRemote --capture /tmp/haiku-live.png --port 10900
```

Run `rp_probe.py` before the client every time. It is pure Python and shares no
code with the Swift decoder, so it separates "the protocol came up" from "the
renderer works" — otherwise a blank PNG has two possible causes.

**Do not authorize a guessed source address.** It cannot be done correctly:
corporate egress NATs per destination, so every external probe disagrees. Measured
on one machine within one minute — `checkip` said `52.94.133.140`, `portquiz.net:22`
said `52.46.80.16`, and the instance's own `$SSH_CLIENT` said `205.251.233.176`.
Only the last is real, and its low octet drifts between connections (`.176`, then
`.236`), so a `/32` is stale immediately.

Access therefore goes through an **EC2 Instance Connect Endpoint**
(`eice-0fdfd2611f6942f31`), which sources traffic from its own ENI inside the VPC:
`sg-0974fc094328104fa` allows port 22 from the endpoint's security group
(`sg-0af2d8ca28fa9f5f7`) and no client address appears anywhere. It needs **no
agent on the guest**, which is why it works for Haiku where SSM cannot. It is
free, and `validate-live.sh` finds and uses it automatically (`--direct` forces
the public-IP path).

Create one with `--no-preserve-client-ip`; with preservation on, the instance sees
the client address again and the whole problem returns. Through an endpoint every
instance appears as `127.0.0.1:<port>`, so pass
`-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no` or ssh will refuse
on a host-key mismatch against the previous instance.

If you do fall back to CIDRs, get the address from `$SSH_CLIENT` on an instance
you can already reach and authorize its `/24`.

| Symptom | Cause |
|---|---|
| `ssh` times out | egress block, or your current address is not in the SG |
| `ssh` refuses the key | RSA key — the images accept ed25519 only |
| `ssh` connects, forward fails | `app_server` not listening; re-run `remote-desktop.sh` on the guest |
| probe connects, no drawing | client never sent `RP_UPDATE_DISPLAY_MODE` — nothing draws until it does |
| PNG is blank but probe traced messages | renderer bug, not a connectivity one |

---

## Build and run

There is no Xcode on this machine, and **SwiftPM cannot run here**: the Command
Line Tools' `libPackageDescription.dylib` exports no `Package.init` symbol, so
every manifest fails to link. `build.sh` compiles with `swiftc` directly.
`Package.swift` is kept for anyone with a full Xcode install.

```sh
./build.sh test     # protocol test suite (253 checks)
./build.sh app      # build build/HaikuRemote.app
./build.sh all
```

`./build.sh install` builds and copies to `/Applications`, which exists because
copying by hand once left `/Applications` nineteen minutes behind `build/` and
that looked exactly like "the app stopped working".

It is a regular Mac app: Dock icon, and a menu bar with **HaikuRemote / Session /
Edit / Window**. The menu-bar status item is kept as well, since a glance at the
glyph is the quickest way to see the link state — `◌` idle, `●` connected, `◍`
failed.

The Edit menu is not decoration. Without `NSApp.mainMenu` no key equivalents are
registered at all, so ⌘C/⌘V do nothing anywhere — including in the Settings
fields, where you have to paste a host and a key path. That was a real bug.

The icon is generated, not a checked-in binary: `tools/make-icon.swift` draws it
with CoreGraphics in Haiku's own colours, and `build.sh icon` fans it out with
`sips` and packs it with `iconutil`.

**If you ever change `LSUIElement` or the icon, re-register the bundle.**
LaunchServices caches bundle metadata, so replacing the app in place leaves it
believing the old value — the symptom is a menu bar with no Dock tile:

```sh
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
  -f /Applications/HaikuRemote.app
```

Run it against the mock with no EC2 involved:

```sh
python3 -u tools/rp_mock_server.py --port 10900 &
defaults write dev.benfelip.HaikuRemote useTunnel -bool false   # Direct mode
defaults write dev.benfelip.HaikuRemote host -string 127.0.0.1
defaults write dev.benfelip.HaikuRemote remotePort -int 10900
open build/HaikuRemote.app --args --autoconnect
```

Add `--torture` to the mock for a second scene that draws all 11 drawing modes
over a fixed base colour, stipples at two different phases, a stippled stroke,
all five gradient kinds, filled and stroked arcs, a `BShape` containing an
`ArcTo`, and an `RP_INVERT_RECT` band. Ordinary Haiku UI is almost entirely
solid `B_OP_COPY`/`B_OP_OVER` fills, so the realistic scene exercises none of
that. `--torture` also disables the animation loop so captures are deterministic
and comparable:

```sh
python3 -u tools/rp_mock_server.py --port 10900 --torture &
HaikuRemote --capture /tmp/torture.png --port 10900 --width 1024 --height 700
```

### The two connection modes

Settings has a **Connection** popup, and it decides which endpoint is dialled:

| Mode | Path | Fields that matter |
|---|---|---|
| **Direct** | `client → host:remotePort` | Haiku host, Haiku port |
| **SSH tunnel (key-based)** | `client → 127.0.0.1:localPort → ssh → host:remotePort` | all of them, plus SSH user / identity file / SSH port |

The window greys out the fields the chosen mode ignores and prints the resulting
path under the popup, because dialling the wrong endpoint just looks like a hang.

**Direct mode cannot work against the EC2 images.** `RemoteHWInterface` binds
127.0.0.1 deliberately (the protocol has no authentication), so nothing is
listening on a reachable interface and tunnel mode is mandatory there. Direct mode
is for a Haiku you control — a VM or LAN box with the bind changed — or for
`host = 127.0.0.1` when pointing at the mock server or a forward you opened
yourself.

For a real instance: choose **SSH tunnel**, set Haiku host, SSH user (`baron`) and
identity file, then Connect. The app spawns and owns the `ssh -L` itself, so no
external tunnel or helper script is needed.

### Headless modes

These exist because they make the render and input paths verifiable without
clicking anything:

```sh
# connect, render, write a PNG, print stats — the Phase 2/3 check
HaikuRemote --capture /tmp/frame.png --port 10900 --width 1024 --height 640

# drive the real InputEncoder and send a scripted burst — the Phase 4 check
HaikuRemote --input-test --port 10900

# verify the tunnel cannot leak an ssh process — the Phase 5 check
HaikuRemote --tunnel-test

# change resolution on a live connection and prove the repaint arrives
HaikuRemote --resize-test --port 10900 --width 800 --height 600 \
            --to-width 1100 --to-height 720
```

`--resize-test` exists because it is the one feature no unit test can cover: it
needs a real server on the other end choosing to repaint in response to a second
`RP_UPDATE_DISPLAY_MODE`.

---

## Layout

```
PROTOCOL.md                      the wire protocol, sourced and annotated
tools/rp_probe.py                raw-TCP handshake + decoding tracer (Phase 1)
tools/rp_mock_server.py          protocol-accurate fake app_server (--torture)
tools/validate-live.sh           stopped instance -> rendered frame, one command
build.sh                         swiftc build (SwiftPM is unusable here)
Sources/HaikuRemoteCore/
  Wire.swift                     codes, value types, reader/writer, framer
  Bitmaps.swift                  8 colour spaces -> BGRA, zero-copy where possible
  DrawState.swift                per-token sticky view state
  Canvas.swift                   BGRA CGBitmapContext + CoreText measurement
  SoftBlend.swift                the 8 drawing modes CoreGraphics cannot express
  Gradients.swift                Haiku's gradient LUT + per-pixel evaluation
  SessionRenderer.swift          the op dispatcher: the bulk of the work
  RemoteConnection.swift         Network.framework TCP + handshake
  ReconnectPolicy.swift          backoff and give-up rules, kept testable
  SSHTunnel.swift                ssh -L lifecycle, orphan prevention
  InputEncoder.swift             NSEvent semantics -> RP_* input messages
Sources/HaikuRemote/             menu bar, canvas view, settings, headless modes
Sources/HaikuRemoteTests/        253-check suite (no XCTest; see build.sh)
```

---

## Things that will bite whoever touches this next

Each of these is a real bug I hit or a trap the reference client falls into. All
are documented with file/line references in `PROTOCOL.md`.

1. **Advance the stream by the message's declared length, never by the fields you
   parsed.** `RP_DRAW_STRING` carries a trailing `hasDelta` bool that no client
   reads. Parse-driven advancement desyncs the whole stream there.
2. **`BRect` edges are inclusive** — width is `right - left + 1`. Off-by-one here
   is a one-pixel seam on every fill in the session.
3. **The wire is fully packed with no alignment padding.** Payloads start at
   offset 6, so use unaligned loads; binding memory to a struct will read
   garbage.
4. **Know which messages omit the leading token.** Twelve are session-level; get
   the set wrong and every field after it is 4 bytes off.
5. **Answer `RP_STRING_WIDTH` before doing any rendering work.** `app_server` is
   blocked on a 1-second timeout while you think.
6. **The reference client's `RP_STRING_WIDTH` handler is broken** — it calls
   `where.writeFloat32` where `where` is not in scope, throws, and never replies.
   So in your browser demo every string-width query stalls `app_server` for the
   full 1 s and then falls back to server-side font metrics. This is probably a
   real source of the browser client's sluggishness, and it means **a correct
   client is deliberately not pixel-identical to the browser demo for text.**
   "Validate pixel-for-pixel against the browser" is only a sound oracle for
   non-text content.
7. **Request the system palette before advertising a display mode**, or the first
   `B_CMAP8` bitmap decodes as black. Observed and fixed; see PROTOCOL.md §4.1.
8. **Don't paint the cursor into the canvas.** Nothing ever repaints what it
   covered, so it leaves a trail.
9. The reference client also has family/style swapped in the font record, iterates
   UTF-16 units where the server sends one point per codepoint, has a typo making
   `B_OP_MAX` a silent no-op, and doesn't implement polygons, Béziers, or the
   `260`–`268` gradient-stroke opcodes at all. Anything your demo renders today
   is not using those.
10. **Never let CoreGraphics colour-manage anything.** Build every `CGColor` in
    the canvas' own `DeviceRGB` space. `CGColor(red:green:blue:alpha:)` is Generic
    RGB and silently brightens every value on the way in — see phase 6 above. The
    same applies to `CGGradient`, which must be given `DeviceRGB` explicitly.
11. **Assert exact pixel values in tests, not inequalities.** A whole-session
    colour shift passed a 171-check suite because the render assertions were of
    the form `px.r > 200 && px.g < 60`. Anything looser than equality will hide a
    systematic error.
12. **Only all-`0xff` and all-`0x00` patterns are solid.** "All eight bytes equal"
    is not the same test and quietly flattens column stripes.
13. **`softBlend` writes to the canvas backing store directly.** That is safe
    alongside `makeImage()` — CoreGraphics snapshots copy-on-write — but it does
    assume the canvas stays opaque, which holds because `app_server` composites
    into an opaque framebuffer and every Haiku ASSIGN macro forces alpha to 255.

## Deliberate deviations from the browser client

Where the reference client is simply wrong, this one is correct instead, because
matching a bug is worse than matching behaviour:

- answers `RP_STRING_WIDTH` (see above)
- decodes font family/style correctly
- implements `B_OP_MAX`, polygons, Béziers, round rects, and `B_RGB15`/`_BIG`
  colour spaces
- iterates codepoints in `RP_DRAW_STRING_WITH_OFFSETS`
- computes `RP_READ_BITMAP` row padding so `bytesPerRow >= width * 3` actually
  holds (the reference client's `(w * 3 + 3) & ~7` can round below it)

Where the reference client is merely *approximate* **and** Haiku's own behaviour
is unclear or invisible in practice, this one still matches the browser so the
demo stays a usable comparison: `B_OP_COPY` forces opaque rather than truly
replacing, and rect fills floor the origin and ceil the far edge.

`B_OP_BLEND` and `B_OP_ADD` are no longer in that list — they now do what
`app_server` does, because "additive blending" is not an approximation of a 50%
average, it is a different picture.

Two deviations run the *other* way: this client deliberately reproduces
behaviour in `app_server` that looks like a bug, because fidelity to the server
is the whole point of a remote desktop and "fixing" it would make the client
differ visibly from a local Haiku screen.

- `B_GRADIENT_RADIAL_FOCUS` ignores its focal point, because `Painter` never
  passes the focus to AGG.
- The gradient colour LUT reproduces Haiku's `dist + 1` off-by-one, so entry 0
  of a full black-to-white ramp is 1 rather than 0.

## Known gaps

Everything previously listed here — the missing drawing modes, shape arcs,
diamond/conic gradients, `drawCursor`, stippled strokes, fixed window size and
manual reconnect — is now implemented and verified. What remains:

- **No antialiasing anywhere.** The canvas and the coverage rasteriser both have
  it off, matching the reference client. Real `app_server` antialiases shapes and
  text through AGG, and can do LCD subpixel AA
  (`gSubpixelAntialiasing`), so edges here are harder than on a local screen.
  The blend path already handles partial coverage, so enabling it is mostly a
  question of deciding whether to match Haiku's exact filter.
- **Fonts are resolved by trait, not by name.** `family`/`style` are decoded
  correctly but unused; the client picks Helvetica, or Menlo for
  `B_FIXED_SPACING`, as the reference client does. Matching a real Haiku screen
  needs the server's font list, which the protocol does not send.
- `B_ALPHA_COMPOSITE` is unsupported (only `B_ALPHA_OVERLAY`); it logs and
  falls back.
- `RP_DRAW_BITMAP`'s `options` field (bilinear filtering) is ignored.
- `RP_DRAW_STRING_WITH_OFFSETS` draws one codepoint at a time, which is what the
  offsets ask for but means no kerning or shaping across the run.
- Shape arc radii assume the shape transform is a uniform scale. That holds for
  every message `app_server` sends — it applies a single `viewScale` — but a
  non-uniform scale would distort the arc.
- The software blend path is a straightforward per-pixel loop, roughly a
  millisecond for a full-screen fill. Fine at the rate exotic modes actually
  arrive, but it is not vectorised.
