# Swift/AppKit prototype — FROZEN, 2026-09-22

This was the first DeBeOS-RDP client: a native macOS menu-bar app rendering `RP_*`
drawing commands onto CoreGraphics. It worked, and it is what proved the protocol
could be spoken by something outside the Haiku tree at all.

**It is no longer developed.** The portable C++ client in `CrossPlatform/` is the
go-forward client. Nothing here should be extended, and no fix belongs here.

## Why it was retired rather than maintained

Keeping two independent renderers of the same protocol means every wire change has
to be implemented twice, and the two drift. When they drift, neither can be used as
a reference for the other — which is the property that made them useful in the first
place. So the choice was one or the other, and the C++ client was ahead on the
measurements that matter:

| | Swift (this) | C++ `CrossPlatform/` |
|---|---:|---:|
| lines of code | 6,834 | **8,418** |
| distinct `RP_` opcodes referenced | 88 | **99** |
| server's distinct `RP_` opcodes | | **99** |

It is also portable, which the AppKit/CoreGraphics client structurally is not — and
the fleet this has to work against is Linux and Haiku, not macOS.

## What is still here and still works

Everything, unchanged, including `build-macos.sh` (formerly the repo's `build.sh`).
On a Mac with a full Xcode install:

```sh
cd archive/swift-prototype
./build-macos.sh test     # the protocol test suite
./build-macos.sh app      # build HaikuRemote.app
```

`Package.swift` moved here too, so the repo root is no longer a SwiftPM package.
That is deliberate: it stops tooling from treating the prototype as the project.

## What it is still good for

Reading. It is an independent second reading of the same specification, and where
it disagrees with the C++ client one of them is wrong — which has diagnostic value
even though neither is authoritative. `PROTOCOL.md` at the repo root remains the
written spec, and the server in `src/servers/app/drawing/interface/remote/` of the
DeBeOS tree remains the actual authority.

One caveat if you do read it for that purpose: it was written against an earlier
state of the protocol. It predates the `RP_SESSION_COOKIE` first-frame requirement
and zstd wire compression, so its absence of either is not evidence about the
current wire.
