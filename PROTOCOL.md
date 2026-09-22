# Haiku `app_server` RemoteDesktop Protocol (`RP_*`)

Reverse-engineered from source in the `felipedbene/Haiku-Graviton` checkout at
`/Users/benfelip/Projects/Haiku-Graviton`. Every claim below is traceable to a
file and line; assumptions are called out explicitly as such.

**Authoritative sources**

| Role | Path |
|---|---|
| Message codes, framing, (de)serialisation primitives | `src/servers/app/drawing/interface/remote/RemoteMessage.h` / `.cpp` |
| Connection lifecycle, listen socket, session-level messages | `src/servers/app/drawing/interface/remote/RemoteHWInterface.cpp` |
| Send side of every drawing op (authoritative payload layouts) | `src/servers/app/drawing/interface/remote/RemoteDrawingEngine.cpp` |
| Input event decode (server side) | `src/servers/app/drawing/interface/remote/RemoteEventStream.cpp` |
| Byte-level transport | `src/servers/app/drawing/interface/remote/NetSender.cpp`, `NetReceiver.cpp` |
| Reference client (the known-working browser demo) | `src/tools/html5_remote_desktop/HaikuRemoteDesktop.js` |
| Deployment / launch of the working setup | `graviton/scripts/haiku-remote-desktop`, `graviton/ssh/files/remote-desktop.sh` |

---

## 0. The headline: this is not a framebuffer protocol

This is the single most important fact about `RP_*`, and it invalidates the
"decode the initial full-frame message" / "dirty-rect tiles" / "delta encoding"
model.

`RP_*` is a **remote drawing-command protocol** — architecturally a cousin of
X11 or RDP's orders, not of VNC/RFB. `app_server` does not rasterise anything
and then ship pixels. It ships the *drawing calls themselves* — "stroke this
rect", "fill this region with this colour", "draw this string at this point",
"play back this Bézier path" — and **the client is the renderer**.

The server holds no framebuffer at all:

```cpp
// RemoteHWInterface.cpp:575-593
RenderingBuffer* RemoteHWInterface::FrontBuffer() const { return NULL; }
RenderingBuffer* RemoteHWInterface::BackBuffer()  const { return NULL; }
bool RemoteHWInterface::IsDoubleBuffered()       const { return false; }
```

Consequences that drive the whole client design:

1. **There is no full-frame message.** No message type carries a screen-sized
   image. The nearest thing is `RP_DRAW_BITMAP`, which blits one application's
   bitmap, and `RP_READ_BITMAP`, which reads pixels *back out of the client*.
2. **There is no delta/dirty-rect encoding and no compression.** Bitmaps go
   over the wire raw. Your own deployment notes call this out: *"bitmaps cross
   the remote protocol raw and uncompressed through a 16 KB ring buffer, so a
   wallpaper is the single most expensive thing that can be on screen"*
   (`graviton/ssh/files/remote-desktop.sh:69-73`).
3. **The client's canvas is the authoritative front buffer.** It is persistent
   session state that only the client holds. `RP_INVALIDATE_RECT` /
   `RP_INVALIDATE_REGION` are sent *by the server* and the reference client
   correctly ignores them (`HaikuRemoteDesktop.js:1918-1920`) — there is nothing
   to invalidate, because the canvas already holds the truth.
4. **A client must implement a fair chunk of a 2D vector graphics engine** with
   Haiku's semantics: per-view state objects, 11 drawing (blend) modes, 8×8
   stipple patterns, 5 gradient types, path playback, 8 bitmap colour spaces,
   and — see §7 — **client-side text layout and measurement**.

So a Swift client's core is not a decoder; it is a `CGContext`/Metal-backed
implementation of Haiku's `DrawingEngine` interface driven off the wire.

---

## 1. Transport

### 1.1 Raw TCP, one connection, bidirectional

`app_server` **listens**; the client **connects**. There is no HTTP, no
WebSocket, and no TLS anywhere in the server. The listener is a plain
`BNetEndpoint` (`RemoteHWInterface.cpp:71-90`, `NetReceiver.cpp:63-89`):

```cpp
BNetAddress loopback((uint32)htonl(INADDR_LOOPBACK), fListenPort);
fInitStatus = fListenEndpoint->Bind(loopback);
```

**Loopback-only bind is a local fork change, deliberately.** Upstream Haiku
binds `INADDR_ANY`. The comment at `RemoteHWInterface.cpp:77-86` explains why
this fork does not: the protocol has *no authentication and no encryption*, so
anything that can reach the port owns the session including every keystroke.
**SSH is the authentication.** Keep it that way.

Once accepted, the same socket carries both directions, serviced by two threads:
`NetReceiver` drains the socket into a 16 KB `StreamingRingBuffer`
(`NetReceiver.cpp:96-131`), `NetSender` drains a second 16 KB ring buffer to the
socket (`NetSender.cpp:52-76`). Both are pure byte pumps — **no framing lives in
the transport layer**.

### 1.2 Ports

| Port | Meaning |
|---|---|
| `10901` | `RemoteHWInterface` constructor default (`RemoteHWInterface.cpp:48`) — *not* what your images use |
| **`10900`** | What your Graviton images actually use: `TARGET_SCREEN=10900` (`graviton/ssh/files/remote-desktop.sh:31-32`, `graviton/scripts/haiku-remote-desktop`) |

The port is passed as the interface's target string and parsed with
`sscanf(fTarget, "%" B_SCNu16, &fListenPort)` (`RemoteHWInterface.cpp:66`), i.e.
the `TARGET_SCREEN` env var doubles as the port number. A Desktop is keyed on
`(uid, targetScreen)`, which is how `launch_daemon`'s Tracker and Deskbar reach
the remote Desktop.

### 1.3 websockify is a pure byte bridge, not part of negotiation

The reference client is a browser and browsers cannot open raw TCP sockets, so
it uses `new WebSocket(targetAddress, 'binary')`
(`HaikuRemoteDesktop.js:1768`). websockify exists solely to satisfy that
browser constraint. The server contains zero WebSocket code, so **the WS upgrade
is not part of protocol negotiation** and a raw-socket client is correct.

Two further pieces of evidence that the `RP_*` byte stream is transport-agnostic:

- The reference client does its own stream reassembly across arbitrary chunk
  boundaries via `this.messageRemainder` (`HaikuRemoteDesktop.js:1832-1869`). It
  never relies on WebSocket message boundaries lining up with `RP_*` message
  boundaries. The protocol is a pure byte stream either way.
- **Your own launcher already performs the raw-TCP handshake and asserts a
  reply**, over `ssh -L`, with no websockify in the path
  (`graviton/scripts/haiku-remote-desktop`):

  ```python
  s = socket.create_connection(('localhost', port), timeout=6)
  s.sendall(bytes([1, 0, 6, 0, 0, 0]))   # RP_INIT_CONNECTION, LE, size=6
  got = s.recv(1)
  ```

  That is Phase 1's experiment, already written down and already passing in
  production use. See §10.

---

## 2. Framing

Every message, both directions:

```
offset  size  type    field
------  ----  ------  --------------------------------------------------
  0      2    uint16  code            (RP_* constant)
  2      4    uint32  totalLength     (INCLUDING these 6 header bytes)
  6      n    ...     payload         (n = totalLength - 6)
```

Writer: `RemoteMessage::Start()` reserves the header and `Flush()` back-patches
the length (`RemoteMessage.h:238-263`):

```cpp
void RemoteMessage::Start(uint16 code) { Add(code); uint32 sizeDummy = 0; Add(sizeDummy); }
status_t RemoteMessage::Flush() {
    uint32 length = fWriteIndex;
    memcpy(fBuffer + sizeof(uint16), &length, sizeof(uint32));   // length INCLUDES header
    return fTarget->Write(fBuffer, length);
}
```

Reader: `RemoteMessage::NextMessage()` (`RemoteMessage.cpp:41-78`) reads the two
header fields, **rejects `totalLength < 6`**, and sets the remaining payload
budget to `totalLength - 6`. The reference client mirrors this exactly, including
the `< 6` rejection (`HaikuRemoteDesktop.js:973-987`).

### 2.1 Trailing payload must be skipped by declared length, not by struct walk

`NextMessage()` opens by *discarding* whatever the previous handler left unread:

```cpp
if (fDataLeft > 0)
    fSource->Read(NULL, fDataLeft);   // discard remainder of message
```

This is load-bearing, not defensive coding: **several messages carry trailing
fields that the reference client never reads.** The clearest case is
`RP_DRAW_STRING`, where the server appends a `bool hasDelta` and optionally an
`escapement_delta` array (`RemoteDrawingEngine.cpp:888-890`) that
`HaikuRemoteDesktop.js:1330-1350` simply does not consume.

**A Swift decoder must therefore advance the stream cursor to
`messageStart + totalLength` after each handler returns**, regardless of how many
bytes that handler actually consumed. Do not derive the next message's offset by
summing the fields you parsed.

### 2.2 Flush granularity, and why messages can be batched or split

`RemoteMessage`'s destructor flushes if anything is pending
(`RemoteMessage.h:229-235`), and most `RemoteDrawingEngine` methods construct a
stack-local `RemoteMessage` and never call `Flush()` explicitly — the destructor
does it at end of scope. Flushes append to a 16 KB ring buffer that `NetSender`
drains in ≤4 KB chunks. So on the wire:

- **A single `read()` may contain many complete messages, or a partial one, or
  both.** The client must buffer and re-drive the parse loop, exactly as
  `messageRemainder` does.
- **A single message may exceed 4 KB** (bitmaps routinely do) and arrive across
  many reads.

---

## 3. Endianness, packing, and type widths

### 3.1 Little-endian, and the reason is "no conversion at all"

There is **no byte-swapping anywhere in the protocol**. `RemoteMessage::Add` is a
raw `memcpy` of the host representation (`RemoteMessage.h:266-276`):

```cpp
template<typename T> void RemoteMessage::Add(const T& value) {
    memcpy(fBuffer + fWriteIndex, &value, sizeof(T));
    fWriteIndex += sizeof(T);
}
```

The wire format is therefore *host* byte order. Haiku on Graviton (ARM64) is
little-endian and macOS on Apple silicon is little-endian, so both ends agree.
The reference client hardcodes this: `new StreamingDataView(this.buffer, true)`
— `littleEndian = true` (`HaikuRemoteDesktop.js:962`).

> **Note:** this makes the protocol non-portable across endianness by
> construction. It is fine for our ARM64→ARM64 case; just do not read
> little-endian as a documented guarantee.

### 3.2 Fully packed — expect unaligned loads

`_MakeSpace`/`memcpy` write at whatever `fWriteIndex` happens to be
(`RemoteMessage.h:371-385`). **There is no alignment padding anywhere.** The
6-byte header alone guarantees misalignment: every payload begins at offset 6,
and the header's own `uint32` length sits at offset 2. The reference client
confirms it with `setUint32(2, ...)` (`HaikuRemoteDesktop.js:370-373`).

In Swift, read scalars with explicit unaligned loads
(`withUnsafeBytes { $0.loadUnaligned(fromByteOffset:as:) }`), never by binding
memory to a struct pointer.

### 3.3 Type widths on the wire

| Wire type | Bytes | Notes |
|---|---|---|
| `uint8` / `int8` | 1 | |
| `bool` | **1** | `sizeof(bool) == 1` on Haiku/gcc ARM64 |
| `uint16` / `int16` | 2 | |
| `uint32` / `int32` | 4 | |
| `float` | 4 | IEEE-754 binary32 |
| `double` | 8 | IEEE-754 binary64 — used **only** by `BAffineTransform` |
| C/C++ `enum` | **4** | default `int` underlying type: `color_space`, `drawing_mode`, `source_alpha`, `alpha_function`, `cap_mode`, `join_mode`, `BGradient::Type` |

Composite types (all packed, no padding):

| Type | Bytes | Layout |
|---|---|---|
| `BPoint` | 8 | `float x, y` |
| `BRect` | 16 | `float left, top, right, bottom` |
| `rgb_color` | 4 | `uint8 red, green, blue, alpha` |
| `pattern` | 8 | `uint8 data[8]` — an 8×8 1-bpp stipple |
| `escapement_delta` | 8 | `float nonspace, space` (`headers/os/interface/Font.h:145`) |

**`BRect` is inclusive on both edges.** Width is `right - left + 1`, not
`right - left` (`HaikuRemoteDesktop.js:435-444`). Getting this wrong yields
off-by-one seams on every fill in the session.

Composite serialisation helpers:

| Helper | Layout |
|---|---|
| `AddString` (`RemoteMessage.h:279-289`) | `uint32 length`, then `length` raw bytes — **not NUL-terminated**, UTF-8 |
| `AddRegion` (`RemoteMessage.h:292-300`) | `int32 rectCount`, then `rectCount` × `BRect` |
| `AddList<T>` (`RemoteMessage.h:303-309`) | `count` × `T`, **no count prefix** — the count is either a separate explicit field or a fixed arity |
| `AddTransform` (`RemoteMessage.cpp:279-294`) | `bool isIdentity`; if false, 6 × `double` in order `sx, shy, shx, sy, tx, ty` |
| `AddBitmap` | see §6 |
| `AddFont` | see §5, and note the reference-client bug there |
| `AddGradient` | see §6 |

---

## 4. Connection lifecycle

### 4.1 Handshake

The handshake is short and **client-initiated**. The client connects, then:

```
client → server   RP_INIT_CONNECTION (1)          empty payload — 6 bytes total: 01 00 06 00 00 00
server → client   RP_INIT_CONNECTION (1)          empty payload (ack)
server → client   RP_SET_CURSOR (200)             hotspot + cursor bitmap
server → client   RP_SET_CURSOR_VISIBLE (201)     bool
server → client   RP_MOVE_CURSOR_TO (202)         float x, float y
client → server   RP_UPDATE_DISPLAY_MODE (2)      int32 width, int32 height
client → server   RP_GET_SYSTEM_PALETTE (4)       empty payload
server → client   RP_GET_SYSTEM_PALETTE_RESULT (5) uint32 count, then count × rgb_color
                  ... session proceeds; server begins streaming drawing ops ...
```

Server side (`RemoteHWInterface.cpp:266-289`); client side
(`HaikuRemoteDesktop.js:2048-2052` for the open, `1874-1894` for the reply).

**`RP_UPDATE_DISPLAY_MODE` is what actually starts the session.** It is the
message that sets `fIsConnected = true`, installs the client's dimensions as the
display mode, and calls `_NotifyScreenChanged()`
(`RemoteHWInterface.cpp:291-307`):

> **Ordering hazard, found the hard way.** The reference client sends
> `RP_UPDATE_DISPLAY_MODE` *before* `RP_GET_SYSTEM_PALETTE`
> (`HaikuRemoteDesktop.js:1877-1883`). Since the mode change is what makes the
> server start drawing, any `B_CMAP8` bitmap in the first paint can arrive
> **before** `RP_GET_SYSTEM_PALETTE_RESULT` does — and without the palette it
> can only be decoded as black. Our Swift client requests the palette first,
> which narrows the window but cannot close it: the protocol offers no ordering
> guarantee, so the decoder also logs a warning if a paletted bitmap wins the
> race. This was observed as a black rectangle where a palette ramp belonged,
> and fixed by the reordering.

```cpp
case RP_UPDATE_DISPLAY_MODE: {
    int32 width, height;
    message.Read(width);
    result = message.Read(height);
    fIsConnected = true;
    fClientMode.virtual_width  = width;
    fClientMode.virtual_height = height;
    _FillDisplayModeTiming(fClientMode);
    _NotifyScreenChanged();
}
```

The client chooses the resolution. Until this arrives the server sits on a
640×480 fallback mode (`RemoteHWInterface.cpp:58-64`) and nothing repaints.

**This is also the only resync mechanism** — see §9.2.

### 4.2 Teardown

`RP_CLOSE_CONNECTION` (3) is sent server→client on shutdown, by
`RemoteHWInterface::_Disconnect()` (`RemoteHWInterface.cpp:706-717`), which
sends it and then closes the listen endpoint. It carries no payload and expects
no reply.

The native in-tree client **does** handle it: `RemoteView.cpp:522-526` answers
it with `be_app->PostMessage(B_QUIT_REQUESTED)`. (An earlier revision of this
document said the reference client had no handler and relied on the socket
closing; that was wrong, and so was the `:374-386` citation.)

**An orderly close is a successful end of session, not an error.** Two
consequences a client must get right:

1. The EOF that follows `RP_CLOSE_CONNECTION` is not a transport failure. A
   client that folds "peer closed the stream" into its error path discards work
   it has already completed — in the capture client that meant losing the entire
   PNG after every pixel had arrived and decoded.
2. `RP_CLOSE_CONNECTION` arrives *before* the EOF, so it is the client's only
   in-band warning that the stream is ending. A client with a capture deadline
   should stop on it rather than waiting out the deadline against a socket that
   will never speak again.

Because the server closes on its own shutdown, a session ending before the
client's own deadline is the **normal** case, not the exception.

There is **no client→server close message** and no keepalive/ping in either
direction. Liveness detection is TCP's problem — relevant for a client running
over hotel wifi behind a NAT with an idle timeout.

### 4.3 Reconnect behaviour

`NetReceiver::_Listen()` loops on `Accept(5000)` forever
(`NetReceiver.cpp:71-89`), and `_NewConnection()` tears down the old `NetSender`
and empties the send buffer on each new connection
(`RemoteHWInterface.cpp:353-371`). So reconnecting is supported and does not
require restarting `app_server` — but see §9.2 for what the client must do to
get its pixels back.

---

## 5. Message catalogue

`RP_*` values are the enum at `RemoteMessage.h:38-139`; the reference client
mirrors them at `HaikuRemoteDesktop.js:3-92`.

### 5.1 Session-level vs. token-addressed — the dispatch rule

This is the second-most important structural fact after §0. **Most drawing
messages begin with an `int32 token` identifying which view state they apply
to, but a specific set of messages do not.** Get the set wrong and every
subsequent field in the message is misaligned by 4 bytes.

The reference client's dispatcher (`HaikuRemoteDesktop.js:1872-1990`) encodes the
rule: an explicit `switch` handles the session-level codes, and the `default:`
branch reads an `int32 token` and forwards to that state object.

**Session-level (NO leading token):**

| Code | # | Payload |
|---|---|---|
| `RP_INIT_CONNECTION` | 1 | *(empty)* |
| `RP_CLOSE_CONNECTION` | 3 | *(empty)* |
| `RP_GET_SYSTEM_PALETTE_RESULT` | 5 | `uint32 count`, `count ×` `rgb_color` |
| `RP_CREATE_STATE` | 20 | `int32 token` — the token *is* the payload, not a prefix |
| `RP_DELETE_STATE` | 21 | `int32 token` — ditto |
| `RP_INVALIDATE_RECT` | 24 | `BRect` — ignored by clients (§0) |
| `RP_INVALIDATE_REGION` | 25 | region — ignored by clients (§0) |
| `RP_COPY_RECT_NO_CLIPPING` | 61 | `int32 xOffset`, `int32 yOffset`, `BRect` |
| `RP_FILL_REGION_COLOR_NO_CLIPPING` | 161 | region, `rgb_color` |
| `RP_SET_CURSOR` | 200 | `BPoint hotspot`, bitmap (non-minimal) |
| `RP_SET_CURSOR_VISIBLE` | 201 | `bool` |
| `RP_MOVE_CURSOR_TO` | 202 | `float x`, `float y` |

The two `*_NO_CLIPPING` cases are token-free because they bypass per-view
clipping entirely — `RemoteDrawingEngine::CopyRect` and the `rgb_color` overload
of `FillRegion` never add `fToken` (`RemoteDrawingEngine.cpp:291-300`,
`603-610`). The reference client handles both at session scope, resetting the
transform and compositing mode first (`HaikuRemoteDesktop.js:1951-1978`).

**Everything else is token-addressed:** `int32 token` first, then the payload
below.

### 5.2 State lifecycle and state-setting messages

`RemoteDrawingEngine`'s constructor emits `RP_CREATE_STATE` with a fresh token
from `gTokenSpace`; its destructor emits `RP_DELETE_STATE`
(`RemoteDrawingEngine.cpp:27-56`). One state per `DrawingEngine`, i.e. roughly
one per view hierarchy. Clients must lazily create an unknown state rather than
dropping the message — the reference client does exactly that
(`HaikuRemoteDesktop.js:1980-1988`), because ordering across the ring buffer is
not guaranteed to put `RP_CREATE_STATE` first.

State fields default to: `lowColor` opaque white, `highColor` opaque black,
`penSize` 1.0, butt cap, miter join, miter limit 10, solid pattern (all `0xff`),
12 px font, identity transform (`HaikuRemoteDesktop.js:1024-1041`).

| Code | # | Payload after `int32 token` |
|---|---|---|
| `RP_ENABLE_SYNC_DRAWING` | 22 | *(none)* — unimplemented in reference client |
| `RP_DISABLE_SYNC_DRAWING` | 23 | *(none)* — ditto |
| `RP_SET_OFFSETS` | 40 | `int32 xOffset`, `int32 yOffset` |
| `RP_SET_HIGH_COLOR` | 41 | `rgb_color` |
| `RP_SET_LOW_COLOR` | 42 | `rgb_color` |
| `RP_SET_PEN_SIZE` | 43 | `float` |
| `RP_SET_STROKE_MODE` | 44 | `enum cap_mode` (4), `enum join_mode` (4), `float miterLimit` |
| `RP_SET_BLENDING_MODE` | 45 | `enum source_alpha` (4), `enum alpha_function` (4) |
| `RP_SET_PATTERN` | 46 | `pattern` (8 bytes) |
| `RP_SET_DRAWING_MODE` | 47 | `enum drawing_mode` (4) |
| `RP_SET_FONT` | 48 | font record — see below |
| `RP_SET_TRANSFORM` | 49 | `bool isIdentity`; if false 6 × `double` |
| `RP_CONSTRAIN_CLIPPING_REGION` | 60 | region (`int32 rectCount`, `count ×` `BRect`) |

Note the sender elides no-op state changes — each setter early-returns if the
value is unchanged (e.g. `RemoteDrawingEngine.cpp:126-137`). State is therefore
*sticky per token* and must be persisted client-side across messages.

**Font record** (`RemoteMessage::AddFont`, `RemoteMessage.cpp:114-127`), **29
bytes** (1+1+4+1+4+4+4+4+2+4):

```
uint8  direction
uint8  encoding
uint32 flags
uint8  spacing          (B_FIXED_SPACING == 3 selects a monospace face)
float  shear
float  rotation
float  falseBoldWidth
float  size
uint16 face             (B_ITALIC_FACE 0x0001, B_BOLD_FACE 0x0020)
uint32 familyAndStyle   (family << 16 | style)
```

> **Reference-client bug — do not copy it.** The client reads the tail as three
> `uint16`s: `face`, `family`, `style` (`HaikuRemoteDesktop.js:559-573`). The
> server sends `uint16 face` then **`uint32 familyAndStyle`**. Both total 6
> bytes so the stream stays in sync, but little-endian ordering means the
> client's `family` is actually the *style* ID and its `style` is the *family*
> ID — swapped. Harmless in practice only because the client ignores both fields
> and picks a face from `spacing`/`face` alone
> (`HaikuRemoteDesktop.js:1073-1076`).

### 5.3 Drawing operations

All token-addressed. `bounds`/`clipBounds` fields exist because the sender
already culls against the clipping region before transmitting (e.g.
`RemoteDrawingEngine.cpp:402-403`) — a clipped-out op is never sent at all.

**Rects, ellipses, arcs**

| Code | # | Payload after token |
|---|---|---|
| `RP_INVERT_RECT` | 62 | `BRect` |
| `RP_STROKE_RECT` | 84 | `BRect` |
| `RP_FILL_RECT` | 104 | `BRect` |
| `RP_STROKE_ELLIPSE` | 82 | `BRect` |
| `RP_FILL_ELLIPSE` | 102 | `BRect` |
| `RP_STROKE_ROUND_RECT` | 85 | `BRect`, `float xRadius`, `float yRadius` |
| `RP_FILL_ROUND_RECT` | 105 | `BRect`, `float xRadius`, `float yRadius` |
| `RP_STROKE_ARC` | 80 | `BRect`, `float angle`, `float span` (degrees) |
| `RP_FILL_ARC` | 100 | `BRect`, `float angle`, `float span` (degrees) |

Angles are **degrees**, converted client-side (`* Math.PI / 180`,
`HaikuRemoteDesktop.js:1396-1401`). The reference client renders round rects as
plain rects — it warns `'round rects not implemented, falling back to rect'`
(`HaikuRemoteDesktop.js:1491`). A native client can do better, at the cost of
diverging from the browser demo pixel-for-pixel.

**Lines, points, polygons, shapes, triangles**

| Code | # | Payload after token |
|---|---|---|
| `RP_STROKE_LINE` | 88 | 2 × `BPoint` |
| `RP_STROKE_LINE_ARRAY` | 89 | `int32 numLines`, then `numLines` × { `BPoint start`, `BPoint end`, `rgb_color` } |
| `RP_STROKE_POINT_COLOR` | 140 | `BPoint`, `rgb_color` |
| `RP_STROKE_LINE_1PX_COLOR` | 141 | 2 × `BPoint`, `rgb_color` |
| `RP_STROKE_RECT_1PX_COLOR` | 142 | `BRect`, `rgb_color` |
| `RP_FILL_RECT_COLOR` | 160 | `BRect`, `rgb_color` |
| `RP_STROKE_BEZIER` | 81 | 4 × `BPoint` |
| `RP_FILL_BEZIER` | 101 | 4 × `BPoint` |
| `RP_STROKE_POLYGON` | 83 | `BRect bounds`, `bool closed`, `int32 numPoints`, `numPoints ×` `BPoint` |
| `RP_FILL_POLYGON` | 103 | `BRect bounds`, `bool closed`, `int32 numPoints`, `numPoints ×` `BPoint` |
| `RP_STROKE_TRIANGLE` | 87 | 3 × `BPoint`, `BRect bounds` |
| `RP_FILL_TRIANGLE` | 107 | 3 × `BPoint`, `BRect bounds` |
| `RP_STROKE_SHAPE` | 86 | shape record, `BPoint viewToScreenOffset`, `float viewScale` |
| `RP_FILL_SHAPE` | 106 | shape record, `BPoint viewToScreenOffset`, `float viewScale` |
| `RP_FILL_REGION` | 108 | region |

Note the **`*_1PX_COLOR` / `*_RECT_COLOR` variants carry their own colour and
ignore the state's pattern/high colour** — they are the fast path Haiku uses for
plain 1-pixel UI chrome, and they are extremely common in a Tracker/Deskbar
session.

Note also `RP_STROKE_TRIANGLE` puts the 3 points **before** `bounds`
(`RemoteDrawingEngine.cpp:805-809`), the opposite order from polygons.

**Shape record** (`RemoteDrawingEngine.cpp:758-767`):

```
BRect  bounds
int32  opCount
uint32 ops[opCount]
int32  pointCount
BPoint points[pointCount]
```

Each op word packs a flag in the high byte and a count in the low 24 bits;
`op = ops[i] & 0xff000000`, `count = ops[i] & 0x00ffffff`. Flags
(`HaikuRemoteDesktop.js:154-161`, playback at `907-947`):

| Flag | Value | Consumes |
|---|---|---|
| `B_SHAPE_OP_MOVE_TO` | `0x80000000` | 1 point |
| `B_SHAPE_OP_CLOSE` | `0x40000000` | 0 points |
| `B_SHAPE_OP_BEZIER_TO` | `0x20000000` | `count` points, in triples (ctrl1, ctrl2, end) |
| `B_SHAPE_OP_LINE_TO` | `0x10000000` | `count` points |
| `B_SHAPE_OP_SMALL_ARC_TO_CCW` | `0x08000000` | `count` points — **unimplemented in reference client** |
| `B_SHAPE_OP_SMALL_ARC_TO_CW` | `0x04000000` | ditto |
| `B_SHAPE_OP_LARGE_ARC_TO_CCW` | `0x02000000` | ditto |
| `B_SHAPE_OP_LARGE_ARC_TO_CW` | `0x01000000` | ditto |

Flags are a bitmask and more than one can be set on a single op word; the
reference client tests each independently in a fixed order (move, line, bezier,
arc, close) and that order is part of the contract.

**The arc ops are SVG elliptical arcs, three points each.** Unimplemented in the
reference client, but fully specified by the server side. `BShape::ArcTo` packs
them as `op | 3` plus three points (`Shape.cpp:479-500`,
`ServerPicture.cpp:141-168`), and `Painter` hands them straight to AGG
(`Painter.cpp:1828-1845`):

```
points[0] = (rx, ry)          radii, scaled by viewScale
points[1] = (angle, unused)   x-axis rotation
points[2] = end point
largeArc  = op & (LARGE_ARC_TO_CW | LARGE_ARC_TO_CCW)
sweep     = op & (SMALL_ARC_TO_CW | LARGE_ARC_TO_CW)   // note: the CW flags
```

Two traps here:

- **`angle` is in radians, not degrees.** It passes unconverted from
  `BShape::ArcTo` through `ServerPicture` into `agg::path_storage::arc_to`, which
  documents radians. Nothing in the chain scales it, despite `BShape`'s API
  reading like degrees.
- **`sweep` is the *clockwise* flag**, which is the opposite polarity to the
  `counterClockWise` argument callers pass to `BShape::ArcTo`; `Painter`
  re-derives it from the CW bits.

`agg::arc_to` implements SVG's endpoint parameterisation (SVG 1.1 appendix
F.6.5), so a client with no elliptical-arc primitive has to convert to cubic
Béziers: at most a quarter turn per segment, with control points at
`±(4/3)·tan(Δθ/4)` along the parametric derivative. The radii are lengths, so a
shape transform applies its *scale* to them but not its translation — which is
exact only because Haiku uses a single uniform `viewScale`.

**Gradient variants**

Every gradient op is its base op's payload with a gradient record appended.

| Code | # | Base payload + gradient |
|---|---|---|
| `RP_FILL_ARC_GRADIENT` | 120 | `BRect`, `float angle`, `float span`, gradient |
| `RP_FILL_BEZIER_GRADIENT` | 121 | 4 × `BPoint`, gradient |
| `RP_FILL_ELLIPSE_GRADIENT` | 122 | `BRect`, gradient |
| `RP_FILL_POLYGON_GRADIENT` | 123 | `BRect`, `bool closed`, `int32 n`, `n ×` `BPoint`, gradient |
| `RP_FILL_RECT_GRADIENT` | 124 | `BRect`, gradient |
| `RP_FILL_ROUND_RECT_GRADIENT` | 125 | `BRect`, `float xRadius`, `float yRadius`, gradient |
| `RP_FILL_SHAPE_GRADIENT` | 126 | shape record, `BPoint offset`, `float scale`, gradient |
| `RP_FILL_TRIANGLE_GRADIENT` | 127 | 3 × `BPoint`, `BRect bounds`, gradient |
| `RP_FILL_REGION_GRADIENT` | 128 | region, gradient |
| `RP_STROKE_ARC_GRADIENT` | 260 | as 120 |
| `RP_STROKE_BEZIER_GRADIENT` | 261 | as 121 |
| `RP_STROKE_ELLIPSE_GRADIENT` | 262 | as 122 |
| `RP_STROKE_POLYGON_GRADIENT` | 263 | as 123 |
| `RP_STROKE_RECT_GRADIENT` | 264 | as 124 |
| `RP_STROKE_ROUND_RECT_GRADIENT` | 265 | as 125 |
| `RP_STROKE_SHAPE_GRADIENT` | 266 | as 126 |
| `RP_STROKE_TRIANGLE_GRADIENT` | 267 | as 127 |
| `RP_STROKE_LINE_GRADIENT` | 268 | 2 × `BPoint`, gradient |

> **The `260`–`268` block does not exist in the reference client at all.** Its
> constant table stops at `RP_MODIFIERS_CHANGED = 244`
> (`HaikuRemoteDesktop.js:92`). The JS client predates that server-side
> addition, so those nine codes fall through to
> `console.warn('unhandled message: ...')`. Anything the browser demo renders
> correctly today is, by definition, not using them.

**Bitmaps** — see §6. **Text** — see §7.

### 5.4 Client→server messages

Only these nine codes ever travel client→server. Everything else is
server→client.

| Code | # | Payload |
|---|---|---|
| `RP_INIT_CONNECTION` | 1 | *(empty)* |
| `RP_UPDATE_DISPLAY_MODE` | 2 | `int32 width`, `int32 height` |
| `RP_GET_SYSTEM_PALETTE` | 4 | *(empty)* |
| `RP_DRAW_STRING_RESULT` | 182 | `int32 token`, `BPoint penAfter` |
| `RP_STRING_WIDTH_RESULT` | 184 | `int32 token`, `float width` |
| `RP_READ_BITMAP_RESULT` | 186 | `int32 token`, bitmap (non-minimal) |
| `RP_MOUSE_*` | 220–223 | §8 |
| `RP_KEY_*` / `RP_MODIFIERS_CHANGED` | 240–244 | §8 |

`RP_UNMAPPED_KEY_DOWN` (242) and `RP_UNMAPPED_KEY_UP` (243) are defined on both
sides but **never sent by the reference client and never decoded by the server**
— `RemoteEventStream::EventReceived` has no case for them
(`RemoteEventStream.cpp:98-120`), so they fall out as `what == 0` and are
rejected. Treat them as reserved.

---

## 6. Bitmaps, colour spaces, and gradients

### 6.1 Bitmap record

Two forms, selected by the `minimal` flag (`RemoteMessage.cpp:90-111`):

**Non-minimal** (used by `RP_DRAW_BITMAP`, `RP_SET_CURSOR`, `RP_READ_BITMAP_RESULT`):

```
int32  width
int32  height
int32  bytesPerRow
uint32 colorSpace        (color_space enum)
uint32 flags
uint32 bitsLength
uint8  bits[bitsLength]
```

**Minimal** (used per-rect by `RP_DRAW_BITMAP_RECTS`): the same, **minus
`colorSpace` and `flags`**, which are sent once in the enclosing message
instead.

`bytesPerRow` can exceed `width * bytesPerPixel` — rows are padded and the
padding must be skipped per row. The reference client honours this by reading a
`bytesPerRow`-sized line buffer at a time for the sub-32-bpp spaces
(`HaikuRemoteDesktop.js:696-743`).

### 6.2 The two bitmap-drawing ops

`RP_DRAW_BITMAP` (63) — one blit, arbitrary scale:

```
int32  token
BRect  bitmapRect        (source rect within the bitmap)
BRect  viewRect          (destination rect on screen)
uint32 options
bitmap (non-minimal)
```

`RP_DRAW_BITMAP_RECTS` (64) — the clipped/scaled path
(`RemoteDrawingEngine.cpp:356-381`):

```
int32  token
uint32 options
uint32 colorSpace        (once, for all bitmaps below)
uint32 flags             (once)
int32  rectCount
rectCount × { BRect destRect, bitmap (MINIMAL) }
```

The server picks `RECTS` whenever the clipped destination region is not a single
rect equal to `viewRect`, or whenever the blit would downscale — in which case it
**pre-scales locally and ships the smaller bitmap** to save bandwidth
(`RemoteDrawingEngine.cpp:1106-1224`). Each sub-bitmap is already the right size
for its destination rect, so the client blits 1:1.

`options` is `B_FILTER_BITMAP_BILINEAR` etc.; the reference client ignores it and
warns (`HaikuRemoteDesktop.js:1295-1299`).

### 6.3 Colour spaces, and the good news for macOS

Values at `HaikuRemoteDesktop.js:110-125`; decoders at `640-775`.

| Space | Value | Memory layout | macOS notes |
|---|---|---|---|
| `B_RGB32` | `0x0008` | **BGRX**, 4 bpp | direct match — see below |
| `B_RGBA32` | `0x2008` | **BGRA**, 4 bpp | direct match |
| `B_RGB24` | `0x0003` | BGR, 3 bpp | needs expansion to 4 bpp |
| `B_RGB16` | `0x0005` | BGR 5:6:5 | needs expansion |
| `B_RGB15` / `B_RGBA15` | `0x0010` / `0x2010` | BGR(A) 5:5:5:1 | **not implemented in reference client** |
| `B_CMAP8` | `0x0004` | 8-bit index | needs the system palette (§4.1) |
| `B_GRAY8` | `0x0002` | 8-bit grey | expand |
| `B_GRAY1` | `0x0001` | 1 bpp, **MSB-first within each byte, a set bit is black** | expand; note bit order *and* polarity |
| `*_BIG` variants | `0x1xxx` / `0x3xxx` | big-endian equivalents | **not implemented in reference client** |

**Haiku's `B_RGB32`/`B_RGBA32` are byte-order BGRA, which is exactly
CoreGraphics' `.byteOrder32Little` + `ARGB` alpha layout — i.e. native macOS
bitmap format.** The reference client has to swap R and B by hand
(`HaikuRemoteDesktop.js:667-694`) only because canvas `ImageData` is RGBA. A
Swift client can wrap the received bytes in a `CGDataProvider` and build a
`CGImage` with `bitmapInfo = .byteOrder32Little | .premultipliedFirst` (or
`.noneSkipFirst` for `B_RGB32`) and do **zero pixel conversion**. This is the
single biggest performance win available over the browser path.

**`B_GRAY1` is MSB-first, and a set bit means _black_** — the *same* bit
convention as `pattern` (§6.5), not the opposite one. Haiku's own reader is
`ReadGray1` in `src/kits/interface/ColorConversion.cpp:556-567`:

```c
int32 shift = 7 - (index % 8);
// In B_GRAY1, a set bit means black (highcolor), a clear bit means white
// (low/view color). So we map them to 00 and 0xFF, respectively.
uint32 result = ((**source >> shift) & 0x01) ? 0x00 : 0xFF;
```

> An earlier revision of this document said LSB-first with a set bit white,
> derived from `HaikuRemoteDesktop.js:760`. That is the JS client being wrong in
> two ways at once — mirrored within every byte *and* inverted — and this
> document should not have taken it as the oracle. `CrossPlatform/` now follows
> `ColorConversion.cpp`; **`Sources/HaikuRemoteCore/Bitmaps.swift` still
> implements the old reading**, and its test asserts it, so the Swift decoder
> needs the same correction.

**Transparent magic.** `B_TRANSPARENT_MAGIC_RGBA32 = 0xff777477`. In `B_RGB32`,
a pixel exactly equal to that value means "transparent" and its alpha must be
cleared — but only when `unsetAlpha` is false
(`HaikuRemoteDesktop.js:691-692`). Skipping this leaves opaque magenta-grey
blocks where Haiku expects see-through.

### 6.4 Gradient record

`RemoteMessage::AddGradient` (`RemoteMessage.cpp:195-276`):

```
uint32 type              (BGradient::Type)
  LINEAR       (0): BPoint start, BPoint end
  RADIAL       (1): BPoint center, float radius
  RADIAL_FOCUS (2): BPoint center, BPoint focal, float radius
  DIAMOND      (3): BPoint center
  CONIC        (4): BPoint center, float angle
  NONE         (5): (nothing)
int32 stopCount
stopCount × { rgb_color color, float offset }
```

**`offset` is 0–255, not 0–1.** The reference client divides by 255
(`HaikuRemoteDesktop.js:867`). Only `LINEAR` and `RADIAL` are implemented there;
the rest warn and fall back to solid black (`858-862`).

#### How app_server actually rasterises them

`Painter::_FillPathGradient` (`Painter.cpp:2075-2140`) pairs each type with an
AGG distance function and a transform, then indexes a 256-entry colour LUT built
by `Painter::_MakeGradient` (`Painter.cpp:2175`). Writing `(dx, dy)` for the
offset from the gradient's centre, the normalised parameter is:

| Type | Parameter | Extent |
|---|---|---|
| `LINEAR` | projection of `(dx, dy)` onto the axis, over the axis length | `start`→`end` |
| `RADIAL` | `sqrt(dx² + dy²) / radius` | `radius` |
| `RADIAL_FOCUS` | same as `RADIAL` — see below | `radius` |
| `DIAMOND` | `max(|dx|, |dy|) / 100` — Chebyshev distance | **fixed 100** |
| `CONIC` | `|atan2(dy, dx)| / π` | **fixed 100, unused** |

AGG **clamps** the parameter to `[0, 1]` rather than repeating
(`span_gradient::generate`), so both ends hold the terminal stop colour.

Three things here are counter-intuitive enough to be worth stating outright:

- **`RADIAL_FOCUS` ignores its focal point.** `Painter` default-constructs
  `agg::gradient_radial_focus`, whose focus is `(0, 0)`, and never calls `init()`
  with the gradient's focus (`Painter.cpp:2101-2110`). With the focus at the
  centre that function reduces to plain radial distance, so on a real Haiku
  desktop `B_GRADIENT_RADIAL_FOCUS` is **indistinguishable from
  `B_GRADIENT_RADIAL`**. A client that honours the focus will not match the
  screen it is mirroring.

- **`DIAMOND` and `CONIC` have a fixed extent.**
  `_CalcRadialGradientTransform` is called with the default `gradient_d2 = 100`
  (`Painter.h:325`), so they always span 100 view units from the centre no matter
  how large the shape is. Scaling them to the shape's bounding box is wrong.

- **`CONIC` ignores its `angle` field.** Only the centre reaches the transform,
  so the sweep always starts along +x, and because the function takes
  `|atan2(...)|` the result is mirrored about the x axis rather than sweeping a
  full turn.

The LUT interpolation has its own off-by-one worth reproducing rather than
"fixing": `_MakeGradient` divides by `dist + 1`, so entry 0 of a full-range
black-to-white ramp comes out as 1, not 0 (`Painter.cpp:2213-2220`).

### 6.5 Patterns and drawing modes

`pattern` is 8 bytes = an 8×8 1-bpp stipple. Bit set → `highColor`, clear →
`lowColor`. Bit order is **MSB-first**: `data[i / 8] & (1 << (7 - i % 8))`
(`HaikuRemoteDesktop.js:812-824`).

The reference client treats "all 8 bytes equal" as solid and takes a
single-colour fast path (`805-809`, `1058-1069`). **That test is wrong.** All
eight bytes being equal only means all eight *rows* are equal, which is true of
every vertical-stripe pattern: eight bytes of `0xf0` is four columns on, four
off. `app_server` tests the whole 64-bit pattern against two constants —

```cpp
inline bool IsSolidHigh() const { return fPattern == B_SOLID_HIGH; }  // all 0xff
inline bool IsSolidLow()  const { return fPattern == B_SOLID_LOW; }   // all 0x00
inline bool IsSolid()     const { return IsSolidHigh() || IsSolidLow(); }
```

— `PatternHandler.h:120-125`. Anything else is a stipple. Getting this wrong is
invisible in ordinary UI (which is almost all `B_SOLID_HIGH`) and then silently
flattens every column-striped fill to one colour.

**The stipple is anchored to the view origin, not to the shape.**
`PatternHandler::IsHighColor` subtracts the view offsets before indexing, and
`SetOffsets` masks them to `& 7` (`PatternHandler.h:162-170`,
`PatternHandler.cpp:222-227`):

```cpp
x -= fXOffset;  y -= fYOffset;
int32 value = ptr[y & 7] & (1 << (7 - (x & 7)));
```

So the pattern phase is a property of the *screen grid*. Tiling an 8×8 image from
each shape's bounding-box corner — the obvious implementation — puts the phase in
a different place for every shape, and two abutting fills will not line up.
`RP_SET_OFFSETS` therefore has to feed the pattern, not just the transform.

`drawing_mode` (`HaikuRemoteDesktop.js:96-106`) and how the reference client maps
each to a canvas composite op (`1202-1261`):

| Haiku mode | # | Reference client mapping | Fidelity |
|---|---|---|---|
| `B_OP_COPY` | 0 | `source-over`, alpha forced to 255 | approximation |
| `B_OP_OVER` | 1 | `source-over` | good |
| `B_OP_ERASE` | 2 | **commented out / unimplemented** | missing |
| `B_OP_INVERT` | 3 | `difference` + white fill | approximation |
| `B_OP_ADD` | 4 | `lighter` | approximation |
| `B_OP_SUBTRACT` | 5 | **commented out / unimplemented** | missing |
| `B_OP_BLEND` | 6 | `lighter` | wrong — `B_OP_BLEND` is a 50% average, not additive |
| `B_OP_MIN` | 7 | `darken` | approximation |
| `B_OP_MAX` | 8 | `'ligthen'` — **typo, not a valid canvas op** | broken, silently a no-op change |
| `B_OP_SELECT` | 9 | unhandled → warns, `source-over` | missing |
| `B_OP_ALPHA` | 10 | `source-over` + `globalAlpha` when `B_CONSTANT_ALPHA` | good |

`B_OP_COPY` additionally sets `unsetAlpha = true`, which forces every colour
read in that state to opaque — that is what `toColor(unsetAlpha)` /
`toUint32(unsetAlpha)` do throughout. `B_OP_ALPHA` with `B_CONSTANT_ALPHA`
(`source_alpha == 1`) means "use `highColor.alpha` as a global multiplier"
rather than per-pixel alpha (`HaikuRemoteDesktop.js:1052-1055`, `1189-1200`).

The reference client only supports `B_ALPHA_OVERLAY`; `B_ALPHA_COMPOSITE` warns
(`1195-1197`).

#### 6.5.1 What the modes actually do

The table above is what the *reference client* does. Here is what `app_server`
does, from `src/servers/app/drawing/Painter/drawing_modes/`. Only three of the
eleven are Porter-Duff operators; the rest read the destination and rewrite it
conditionally, and three of them touch only pixels where the stipple selects the
high colour. **No `CGBlendMode` or canvas composite op can express those**, which
is why mapping them onto approximate blend modes goes wrong in visible ways.

Writing `d` as the destination pixel and `s` as the source colour at that pixel
(`PatternHandler::ColorAt`), with `ASSIGN_*` used at full coverage and `BLEND_*`
at partial:

| Mode | # | Actual semantics | Source |
|---|---|---|---|
| `B_OP_COPY` | 0 | `d = s`, source alpha written through | `DrawingModeCopy.h:21` |
| `B_OP_OVER` | 1 | `d = s`, alpha forced 255 | `DrawingModeOver.h:21` |
| `B_OP_ERASE` | 2 | **where stipple is high**: `d = lowColor` | `DrawingModeErase.h:21,31` |
| `B_OP_INVERT` | 3 | **where stipple is high**: `d = 255 - d` | `DrawingModeInvert.h:24,39` |
| `B_OP_ADD` | 4 | `d = min(255, d + s)` per channel | `DrawingModeAdd.h:27` |
| `B_OP_SUBTRACT` | 5 | `d = max(0, d - s)` per channel | `DrawingModeSubtract.h:29` |
| `B_OP_BLEND` | 6 | `d = (d + s) >> 1` per channel — a true 50% average | `DrawingModeBlend.h:26` |
| `B_OP_MIN` | 7 | if `brightness(s) < brightness(d)` then `d = s`, **whole pixel** | `DrawingModeMin.h:21` |
| `B_OP_MAX` | 8 | if `brightness(s) > brightness(d)` then `d = s`, **whole pixel** | `DrawingModeMax.h:27` |
| `B_OP_SELECT` | 9 | **where stipple is high**: `d == high → low`, `d == low → high`, else unchanged | `DrawingModeSelect.h:31` |
| `B_OP_ALPHA` | 10 | source-over using source alpha, or `highColor.alpha` as a global multiplier under `B_CONSTANT_ALPHA` | `DrawingModeAlpha*.h` |

Two of these deserve emphasis because the natural mapping is wrong rather than
merely imprecise:

- **`B_OP_MIN`/`B_OP_MAX` are not per-channel.** They compare a single
  brightness value and then replace *the entire pixel* or none of it. Canvas
  `darken`/`lighten` and `CGBlendMode.darken`/`.lighten` are per-channel, so they
  can produce a colour present in neither operand. With `d = (255,0,0)` and
  `s = (0,128,0)`, `B_OP_MIN` yields `(0,128,0)` — brightness 75 against 76 —
  while per-channel darken yields `(0,0,0)`.

- **`B_OP_SUBTRACT` is not `difference`.** It clamps at zero and is directional;
  `difference` gives `|d - s|`.

The brightness function is an integer approximation of luma
(`DrawingMode.h:190`):

```cpp
brightness = (308 * red + 600 * green + 116 * blue) / 1024
```

and `BLEND(d, r, g, b, a)` is `d + (src - d) * a / 256` per channel with the
destination alpha forced to 255 (`DrawingMode.h:28`), because `app_server`
composites into an opaque framebuffer. That last point matters for a client that
keeps a premultiplied canvas: premultiplied equals straight whenever alpha is
255, so a byte-level transcription of these macros is exact as long as the canvas
never becomes transparent.

Finally, note that `RP_INVERT_RECT` (62) is **not** `B_OP_INVERT`.
`Painter::_InvertRect32` inverts every channel of every pixel in the rect,
ignoring both the drawing mode and the pattern, whereas `B_OP_INVERT` is gated on
the stipple.

The `*_COLOR` opcodes (`RP_FILL_RECT_COLOR`, `RP_STROKE_RECT_1PX_COLOR`,
`RP_STROKE_POINT_COLOR`, `RP_STROKE_LINE_1PX_COLOR`,
`RP_FILL_REGION_COLOR_NO_CLIPPING`) carry no mode or pattern and must **not** be
run through the drawing mode at all: `Painter::FillRect(BRect, rgb_color)` is a
raw pixel assignment loop that bypasses both (`Painter.cpp`, and
`DrawingEngine::FillRect(BRect, rgb_color)` above it).

---

## 7. Text: the client owns font metrics, and the server blocks on it

This is the third structural surprise, and the one with the worst latency
implications.

`app_server` does **not** rasterise text and does not do its own layout for
remote views. It asks the client to draw the string *and to tell it where the
pen ended up*, then **blocks waiting for the answer**.

`RP_DRAW_STRING` (180), server→client:

```
int32  token
BPoint where
uint32 length
uint8  utf8[length]
bool   hasDelta
escapement_delta delta[length]     // only if hasDelta
```

`RP_DRAW_STRING_WITH_OFFSETS` (181), server→client:

```
int32  token
uint32 length
uint8  utf8[length]
BPoint offsets[UTF8CountChars(string, length)]     // one per CODEPOINT
```

**"One per codepoint" is the intent, not the rule on the wire.** The count is
literally `UTF8CountChars()` (`headers/private/interface/utf8_functions.h`),
which counts **bytes that are not UTF-8 continuation bytes** and **stops at the
first NUL** — it never validates. `app_server` does not validate either:
`AS_DRAW_STRING_WITH_OFFSETS` only checks that the application supplied *at
least* that many offsets, so whatever bytes were passed to `BView::DrawString`
reach the wire verbatim. That count disagrees with a decoded walk of the string
on anything malformed:

| `string` bytes | `UTF8CountChars` → points sent | Decoded sequences |
|---|---|---|
| `41 42` (`"AB"`) | 2 | 2 |
| `c3 a9` (`"é"`) | 1 | 1 |
| `c3 a9 a9` (extra continuation) | 1 | 2 |
| `80 41` (orphan continuation) | 1 | 2 |
| `a9 20 32 30 32 36` (Latin-1 `"© 2026"`) | 5 | 6 |
| `41 00 42` (NUL inside `length`) | 1 | 3 |

A client that walks decoded sequences reads *past* the payload on Latin-1 text
containing `©`, `«`, `»`, `°`, `±`, `µ`, `¶`, `·`, `¼`–`¾` (all U+0080–U+00BF,
i.e. bare continuation bytes), or on any string whose `length` spans a NUL — and
then the reply below is never sent. Count glyph starts the way the sender does:
one point per non-continuation byte, absorbing the continuation bytes that
follow it, stopping at a NUL.

`RP_STRING_WIDTH` (183), server→client: `int32 token`, `uint32 length`,
`uint8 utf8[length]`.

Replies, client→server: `RP_DRAW_STRING_RESULT` (182) = `int32 token` +
`BPoint penAfter`; `RP_STRING_WIDTH_RESULT` (184) = `int32 token` +
`float width`.

### 7.1 The blocking round-trips and their timeouts

`RemoteDrawingEngine::DrawString` flushes and then sleeps on a semaphore
(`RemoteDrawingEngine.cpp:878-908`):

```cpp
do {
    result = acquire_sem_etc(fResultNotify, 1, B_RELATIVE_TIMEOUT, 1 * 1000 * 1000);
} while (result == B_INTERRUPTED);
if (result != B_OK) return point;     // timeout: pen advance is silently WRONG
return fDrawStringResult;
```

| Operation | Timeout | On timeout |
|---|---|---|
| `RP_DRAW_STRING` | **1 s** | returns the *original* pen point → text advance wrong → layout corruption |
| `RP_DRAW_STRING_WITH_OFFSETS` | **1 s** | returns `offsets[0]` → same |
| `RP_STRING_WIDTH` | **1 s** | falls back to server-side `ServerFont::StringWidth` (`RemoteDrawingEngine.cpp:974-976`) |
| `RP_READ_BITMAP` | **10 s** | returns the error; caller sees `B_UNSUPPORTED` |

**This is the protocol's hard low-RTT assumption, and it is per-string, not per
frame.** Every string Haiku draws costs a full network round-trip on
`app_server`'s drawing thread. On localhost that is microseconds. Over hotel
wifi at 150 ms RTT, a window with 40 labels costs ~6 s of *serialised* blocking.
Above ~1 s RTT, text stops working correctly rather than merely slowly.

Measured against `tools/rp_mock_server.py --latency-ms N`, with the Swift client
answering correctly (one-way delay applied to the server's sends, so real
symmetric RTT would be roughly double):

| Simulated one-way delay | Observed `RP_STRING_WIDTH` round trip |
|---|---|
| 0 ms (localhost) | 1.6–13.4 ms |
| 150 ms | 161 ms |
| 400 ms | 411 ms |

The client's own contribution is ~1–11 ms (measure + reply); everything else is
the network. Extrapolating, a symmetric RTT above roughly 900 ms breaks text
rather than merely slowing it, because `DrawString` then times out and returns a
wrong pen position. Hotel wifi at 150 ms RTT is fine; a congested in-flight link
at 600–800 ms is marginal.

Mitigations worth considering in Phase 4 (all client-side, no server change):

- Answer `RP_STRING_WIDTH` and `RP_DRAW_STRING_RESULT` **immediately and
  unconditionally**, before doing any rendering work — the reply only needs
  metrics, not a finished raster. Measure, reply, then draw.
- Keep the reply on a dedicated high-priority path so a large queued
  `RP_DRAW_BITMAP` cannot delay it behind head-of-line blocking in the same
  socket. Note this is a genuine limitation: with one TCP connection, a 2 MB
  bitmap in flight *will* delay the reply, and there is no protocol-level
  interleaving to prevent it.
- Cache measurements aggressively, keyed on (font record, string). UI labels
  repeat constantly.

### 7.2 A reference-client bug that changes what "matching the demo" means

`RP_STRING_WIDTH`'s handler is broken (`HaikuRemoteDesktop.js:1378-1389`):

```js
reply.start(RP_STRING_WIDTH_RESULT);
reply.dataView.writeInt32(this.token);
where.writeFloat32(textMetric.width);     // <-- `where` is undefined here
reply.flush();
```

`where` is not in scope in that branch — it should be `reply.dataView`. The
handler throws, is swallowed by the caller's `try/catch`
(`HaikuRemoteDesktop.js:1858-1863`), and **no reply is ever sent**. So in your
working browser demo, every `RP_STRING_WIDTH` blocks `app_server` for the full
1 s timeout and then falls back to *server-side* font metrics.

This has two consequences that matter for the phase plan:

1. It is very likely a real and significant source of the browser client's
   sluggishness — a 1 s stall per distinct string-width query.
2. **A correct Swift client will not be pixel-identical to the browser demo, by
   design.** If we answer `RP_STRING_WIDTH` properly with Core Text metrics,
   `app_server` lays out using *our* numbers instead of its own fallback, so
   text positions will legitimately differ. "Validate pixel-for-pixel against
   the browser demo" (Phase 2) is therefore only a valid oracle for
   *non-text* content, and only if we reproduce the reference client's
   approximations rather than improving on them.

### 7.3 Other known reference-client text issues

- `RP_DRAW_STRING_WITH_OFFSETS` iterates `string.length` (UTF-16 code units)
  while the server sends one `BPoint` per **codepoint** via `UTF8CountChars`
  (`RemoteDrawingEngine.cpp:921` vs `HaikuRemoteDesktop.js:1362-1365`). These
  diverge for any non-BMP character.
- Font selection is `Helvetica`, or `monospace` when
  `spacing == B_FIXED_SPACING`, with bold/italic from `face`
  (`HaikuRemoteDesktop.js:1073-1076`). `family`/`style` are ignored entirely,
  so the demo never uses Haiku's actual fonts (DejaVu / Noto).

### 7.4 `RP_READ_BITMAP` — the server reads pixels *from* the client

`RP_READ_BITMAP` (185), server→client: `int32 token`, `BRect bounds`,
`bool drawCursor`. The client must reply `RP_READ_BITMAP_RESULT` (186) with
`int32 token` + a non-minimal bitmap of that screen region.

The reference client answers in `B_RGB24` with
`bytesPerRow = (width * 3 + 3) & ~7` (`HaikuRemoteDesktop.js:1714-1755`). Note
that expression is *not* a straightforward 4-byte alignment — `& ~7` clears the
low three bits after adding 3, which can round *down* below `width * 3`. It
appears to be intended as an 8-byte alignment and is at best fragile; a native
client should compute a padding that genuinely satisfies
`bytesPerRow >= width * 3`. `drawCursor` is unimplemented there.

This is how `BScreen::ReadBitmap`, screenshots, and window-drag transparency work
in a remote session. It is also a 10 s blocking round-trip carrying a full
uncompressed region back upstream.

**`bounds` is whatever the application asked for: unvalidated, possibly outside
the screen, possibly empty.** `BPrivateScreen::ReadBitmap` forwards the caller's
`BRect` untouched and `ServerApp`'s `AS_READ_BITMAP` does not clip it before
handing it to the drawing engine, so a client cannot assume the rectangle
intersects its surface at all. Two consequences, both of them the client's
problem:

- **Answer every request, degenerate ones included.** `ServerApp` holds the
  desktop drawing engine's *exclusive* lock across the whole 10 s wait, so a
  skipped reply is not a slow screenshot — it is ten seconds in which nothing on
  the desktop repaints. A request that cannot be honoured should still be
  answered with a parseable bitmap (one pixel is enough); the caller then gets a
  failed readback promptly instead of a frozen session. Note that a `0x0` reply
  is *not* parseable: `RemoteMessage::ReadBitmap` constructs
  `BBitmap(BRect(0, 0, width - 1, height - 1))`, whose `InitCheck` fails, and the
  result callback then returns without releasing the semaphore — so the server
  waits out the full timeout anyway.
- **Reply with the geometry that was requested, not the part that was visible.**
  The server imports the reply with `ServerBitmap::ImportBits(bits, length,
  bytesPerRow, colorSpace)` into a bitmap it already sized from its own `bounds`,
  and that overload converts *the destination's* width and height while reading
  at *the client's* stride. So a reply clipped down to the client's surface is
  not read as a partial image — rows are read across the source's row padding and
  the picture skews. Send the requested rectangle and fill the part that does not
  overlap the surface.

---

## 8. Input events

All client→server. Server decode in `RemoteEventStream::EventReceived`
(`RemoteEventStream.cpp:93-221`); reference client encode at
`HaikuRemoteDesktop.js:2067-2220`.

The server maintains sticky `fMousePosition`, `fMouseButtons`, and `fModifiers`
across events (`RemoteEventStream.cpp:26-28`) and synthesises Haiku `BMessage`s
from them, filling `when` with the server's own `system_time()` — **client
timestamps are never transmitted**.

| Code | # | Payload |
|---|---|---|
| `RP_MOUSE_MOVED` | 220 | `float x`, `float y` |
| `RP_MOUSE_DOWN` | 221 | `float x`, `float y`, `int32 buttons`, `int32 clicks` |
| `RP_MOUSE_UP` | 222 | `float x`, `float y`, `int32 buttons` |
| `RP_MOUSE_WHEEL_CHANGED` | 223 | `float xDelta`, `float yDelta` |
| `RP_KEY_DOWN` | 240 | `int32 numBytes`, `uint8 bytes[numBytes]`, `int32 rawChar`, `int32 key` |
| `RP_KEY_UP` | 241 | same as 240 |
| `RP_MODIFIERS_CHANGED` | 244 | `uint32 modifiers` |

Details that matter:

- **`RP_MOUSE_MOVED` carries no buttons field.** The server explicitly skips the
  read for it (`RemoteEventStream.cpp:136-138`) and reuses the last known button
  state. Dragging therefore depends on a prior `RP_MOUSE_DOWN` having landed.
- **Button bit values coincide with the DOM's.** Haiku
  `B_PRIMARY_MOUSE_BUTTON = 1`, `B_SECONDARY = 2`, `B_TERTIARY = 4`; DOM
  `MouseEvent.buttons` uses left = 1, right = 2, middle = 4. The reference client
  passes `event.buttons` through unchanged (`HaikuRemoteDesktop.js:2083`). On
  macOS, `NSEvent` gives left/right/other separately, so build the mask
  explicitly.
- **`clicks` is only present on `RP_MOUSE_DOWN`**, and the server reads it
  defensively (`if (message.Read(clicks) == B_OK)`), so omitting it is tolerated.
  The reference client sends `event.detail`; on macOS use `NSEvent.clickCount`.
- **Coordinates are absolute, in client-canvas pixels**, floats. The reference
  client sends `event.offsetX/offsetY`.
- **`numBytes` is capped at 1000** server-side and larger values abort the event
  (`RemoteEventStream.cpp:172-173`).
- **`rawChar` is always sent as 0 by the reference client**
  (`HaikuRemoteDesktop.js:2189-2192`), with `key` set to the DOM `keyCode`. Since
  Haiku's keymap handling wants a real `raw_char` and a real Haiku key code,
  this is the root of the browser demo's imperfect keyboard mapping. A native
  client has the information to do better — `key` should be a Haiku key code,
  not a platform scancode, if full keymap fidelity is wanted.
- **The `bytes` field carries the *already-composed* UTF-8 text.** The reference
  client writes `event.key` when it is a single character, else a 1-byte
  `keyCode` (`2181-2187`). The server re-publishes it as both a `byte` array and
  a `bytes` string in the `BMessage`.
- **Modifiers are stateful and client-owned.** The client tracks the full mask
  itself and sends the complete new value on any change; the server just stores
  it and stamps it onto later key/mouse events. Lock keys (caps/scroll/num)
  toggle rather than set (`HaikuRemoteDesktop.js:2163-2179`). Modifier constants
  are at `HaikuRemoteDesktop.js:189-204`.
- **macOS key mapping note:** the reference client maps `AltLeft`/`AltRight` to
  `B_LEFT_COMMAND_KEY`/`B_RIGHT_COMMAND_KEY` (`2131-2141`) because Haiku's
  `COMMAND` sits where Alt is on a PC keyboard. On a Mac keyboard the physical
  positions differ again; expect to need an explicit decision about whether
  macOS ⌘ or ⌥ becomes Haiku `B_COMMAND_KEY`.

There is also `RP_MODIFIERS_CHANGED` short-circuiting: when a keypress is purely
a modifier change, the reference client sends *only* the modifiers message and
returns, never a `RP_KEY_DOWN` (`HaikuRemoteDesktop.js:2163-2179`).

---

## 9. Implementation notes for a native client

### 9.1 Receive loop

1. Append every received byte to a rolling buffer.
2. While ≥6 bytes buffered: peek `code` (`uint16` LE @0) and `totalLength`
   (`uint32` LE @2, unaligned). Reject `totalLength < 6`.
3. If fewer than `totalLength` bytes buffered, stop and wait for more.
4. Dispatch on `code` with a cursor into the payload.
5. **Unconditionally advance the cursor to `messageStart + totalLength`** (§2.1).
6. Compact the buffer and repeat.

Do not assume one `read()` == one message in either direction (§2.2). Bitmaps
regularly exceed the 4 KB chunk size and the 16 KB ring buffer, so large
messages *will* arrive fragmented.

### 9.2 Reconnect and resync

There is no "refresh" or "request full repaint" message, because the server holds
no pixels (§0). But `RP_UPDATE_DISPLAY_MODE` calls `_NotifyScreenChanged()`
(`RemoteHWInterface.cpp:305`), which makes the Desktop reconstruct and repaint
everything. **So the resync procedure is: reconnect, send `RP_INIT_CONNECTION`,
then send `RP_UPDATE_DISPLAY_MODE` — the mode change is what forces a full
redraw.**

Two caveats:

- The client must **clear its canvas and discard all cached token states** on
  reconnect. Stale state objects from the previous session will be re-created by
  the server with fresh tokens, and stale clipping regions would corrupt output.
- Sending the *same* width/height still triggers `_NotifyScreenChanged()`, so no
  resolution juggling is needed.

For a travel tool this is the critical robustness path: a dropped TCP connection
is fully recoverable without restarting `app_server` (§4.3), which is a
meaningfully better story than the browser client's "canvas disappears on
`onClose`" behaviour (`HaikuRemoteDesktop.js:2007-2012`).

### 9.3 Performance shape

The bandwidth profile is unusual and worth internalising: **vector ops are tiny
and bitmaps are enormous.** A `RP_FILL_RECT_COLOR` is 30 bytes. A full-screen
1920×1080 `B_RGB32` bitmap is 8.3 MB uncompressed, with no compression available
anywhere in the protocol. Hence your deployment's decision to force a flat
desktop colour (`graviton/ssh/files/remote-desktop.sh:67-80`).

For a native client this means: don't optimise the vector path, and do everything
possible to avoid bitmap traffic. The zero-copy BGRA path in §6.3 is the main
lever available on the client side.

### 9.4 Rendering backend choice

The op set maps almost one-to-one onto CoreGraphics: `CGContext` has
rects, ellipses, arcs, Béziers, paths, clipping regions, affine transforms, and
gradients. `MTKView` buys nothing here — there is no per-frame pixel pipeline to
accelerate, just an accumulating 2D canvas. **A `CGBitmapContext` in BGRA8888
that the view blits, with drawing applied incrementally, is both the closest
match to the protocol's semantics and the closest match to what the reference
client does.**

The genuinely hard parts are not the geometry:

- Haiku's stipple `pattern` (no direct CoreGraphics equivalent; needs a tiled
  `CGPattern` or manual composition)
- the drawing modes CoreGraphics lacks (`B_OP_INVERT`, `B_OP_SELECT`,
  `B_OP_MIN`/`MAX`, `B_OP_BLEND`) — note the reference client fudges or skips all
  of these too (§6.5), so matching it is easier than matching Haiku
- text metric fidelity (§7.2)

---

## 10. Phase 1 status: the transport hypothesis is confirmed

Confirmed by source inspection (§1.1, §1.3) and, independently, by an existing
passing test in your own tooling: `graviton/scripts/haiku-remote-desktop`
already opens a raw TCP socket through `ssh -L`, sends
`RP_INIT_CONNECTION` as the six bytes `01 00 06 00 00 00`, and requires a reply
before it will start websockify. That check is the Phase 1 experiment, and it is
a precondition of the working browser demo — so it has been passing every time
you've used the tool.

**websockify is not part of protocol negotiation and can be removed from the
path.** A raw-socket Swift client using `Network.framework` is the correct
approach; no embedded WebSocket implementation is needed.

Remaining item for a live Phase 1 run: point the same handshake at a running
instance and dump the first few hundred bytes to confirm the message stream
decodes as documented above (expect `RP_INIT_CONNECTION`, `RP_SET_CURSOR`,
`RP_SET_CURSOR_VISIBLE`, `RP_MOVE_CURSOR_TO` before any drawing traffic, and
nothing at all beyond the cursor messages until the client sends
`RP_UPDATE_DISPLAY_MODE`). Use `tools/rp_probe.py --port <local>`.

That live run has **not** been done, because this workstation's network blocks
outbound TCP to EC2 public addresses — see README "Live validation status". Every
byte layout above is nonetheless exercised end to end against
`tools/rp_mock_server.py`, which was written from these same sources
independently of the Swift decoder, so the two agreeing is a real check rather
than a tautology. What remains unverified is only whether *real* `app_server`
traffic contains ops or field orders this document gets wrong.
