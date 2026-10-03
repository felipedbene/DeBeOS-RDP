# Building & packaging the DeBeOS-RDP client natively on Haiku (32-bit x86_gcc2 hybrid)

SOP for producing an installable `.hpkg` of the headless `haiku-remote` client **on a
Haiku machine itself** (e.g. a 32-bit x86_gcc2h laptop). No cross-compile, no VM.

> The whole trick: on an **x86_gcc2 hybrid**, the default compiler is legacy **gcc2**,
> which cannot build C++20. Everything below runs under **`setarch x86`**, which switches
> your shell to the modern **gcc13 secondary** toolchain. If you forget it, you get a
> cascade of "not declared in this scope" errors in `types.hpp` — that's gcc2, not a bug.

## 0. One-time: confirm the toolchain
```sh
uname -m            # x86_gcc2  (hybrid). If x86_64, you don't need setarch at all.
setarch x86 g++ --version    # must say 13.x, NOT 2.95
```

## 1. Install build dependencies (secondary = `_x86` packages)
```sh
pkgman install make pkgconfig gcc_x86 \
                freetype_x86_devel harfbuzz_x86_devel \
                haiku_devel haiku_x86_devel \
                libpng16_x86_devel        # see note
```
- `haiku_devel` + `haiku_x86_devel` are **required** — without them the compile dies with
  `fatal error: stdlib.h: No such file`.
- **PNG package name varies by Haiku revision.** Recent nightlies ship `libpng16`; older
  ones ship `libpng12`. Check with `pkgman search libpng` and install the `_x86_devel`
  that exists (`libpng16_x86_devel` **or** `libpng12_x86_devel`). Remember which — you
  reference it in steps 2 and 4.

## 2. Build (headless `haiku-remote` only — cheapest on a slow box)
```sh
cd DeBeOS-RDP
export PKG_CONFIG_PATH=/boot/system/develop/lib/x86/pkgconfig   # secondary .pc files
# sanity: every module must print flags, not come back empty
setarch x86 pkg-config --cflags --libs libpng freetype2 harfbuzz

setarch x86 make -C CrossPlatform build/haiku-remote \
     PACKAGES="libpng freetype2 harfbuzz" \
     CXXFLAGS="-std=c++20 -O0 -ffp-contract=off --param ggc-min-expand=10 --param ggc-min-heapsize=32768"
```
- Use the PNG **pkg-config module** name that resolved in the sanity line (`libpng` works
  for both libpng16 and libpng12; `libpng12`/`libpng16` also work explicitly).
- `-O0` + the `ggc-*` params keep the compiler's memory tiny — important on a RAM-starved
  machine (a C++20 TU can otherwise need ~1 GB and get OOM-killed; low CPU + no progress =
  swapping). Build one frontend (`build/haiku-remote`), not `all`, to avoid SDL/X11 work.
- Sockets: the Makefile already links `-lnetwork` on Haiku (merged fix). If an *older*
  checkout fails the link with undefined `getaddrinfo/socket/...`, add
  `LDLIBS="$(pkg-config --libs libpng freetype2 harfbuzz) -lnetwork"`.
- The result is `CrossPlatform/build/haiku-remote` (ELF 32-bit x86).

## 3. Stage the install tree + write `.PackageInfo`
```sh
mkdir -p ~/pkg/apps/DeBeOS-RDP
cp CrossPlatform/build/haiku-remote ~/pkg/apps/DeBeOS-RDP/
cat > ~/pkg/.PackageInfo <<'INFO'
name            debeos_rdp
version         1.0.0-1
architecture    x86_gcc2
summary         "DeBeOS-RDP remote desktop client (haiku-remote)"
description     "Headless haiku-remote protocol client / PNG capture."
packager        "you <you@example.com>"
vendor          "DeBeOS"
copyright        "2026 Felipe De Bene"
licenses        "MIT"
provides {
    debeos_rdp = 1.0.0
}
requires {
    haiku
    freetype_x86
    harfbuzz_x86
    libpng16_x86
}
INFO
```
> **THE gotcha — `architecture x86_gcc2`, NOT `x86`.** On a hybrid, *every* package
> (even secondary `_x86` ones) must declare the **primary** arch `x86_gcc2`. The `_x86` is
> only a name/dependency convention. A package marked `architecture x86` is "foreign" to
> the dependency solver and is rejected with the useless message
> `package X is not installable / solution: do not install` (no "nothing provides" line,
> nothing in syslog) — which looks exactly like a missing dependency but is not.
> Verify any doubt against a known-good secondary package: `package dump $(finddir B_SYSTEM_PACKAGES_DIRECTORY)/freetype_x86-*.hpkg | grep architecture` → `x86_gcc2`.
>
> Match the `requires` PNG line to what you built against (`libpng16_x86` **or**
> `libpng12_x86`).

## 4. Create the hpkg
```sh
cd ~/pkg
package create -b ~/debeos_rdp-1.0.0-1-x86_gcc2.hpkg
package add  ~/debeos_rdp-1.0.0-1-x86_gcc2.hpkg -C ~/pkg .   # if your `package` needs an explicit add; otherwise `package create` from the staged dir suffices — see `package --help`
package list -i ~/debeos_rdp-1.0.0-1-x86_gcc2.hpkg           # verify architecture + requires
```

## 5. Install & run
```sh
pkgman install ~/debeos_rdp-1.0.0-1-x86_gcc2.hpkg
setarch x86 /boot/system/apps/DeBeOS-RDP/haiku-remote --help
```
- Dropping the hpkg into `/system/packages` also activates it (on next reboot).
- The installed binary is a secondary-arch x86 binary, so **run it under `setarch x86`**.

## Troubleshooting quick table
| Symptom | Cause | Fix |
|---|---|---|
| `'uint32_t' not declared`, templates invalid in `types.hpp` | compiling with gcc2 | prefix with `setarch x86` |
| `ft2build.h: No such file` | combined `pkg-config --cflags` returns empty because ONE module is missing | install the missing `_x86_devel`; probe each module singly |
| `stdlib.h: No such file` | system headers absent | `pkgman install haiku_devel haiku_x86_devel` |
| undefined `getaddrinfo`/`socket` at link | sockets live in libnetwork on Haiku | ensure `-lnetwork` (current Makefile does this) |
| build crawls then dies, low CPU | RAM exhausted, swapping | `-O0 --param ggc-min-expand=10`; close apps; build one frontend |
| `package ... is not installable` (no detail) | `.PackageInfo` `architecture x86` | change to `architecture x86_gcc2` |

## Interactive GUI frontend (`haiku-remote-gui`)

The headless `haiku-remote` only connects and captures (`--png`). For a real interactive
session — a window on your desktop with live mouse/keyboard — build the SDL2 frontend.

1. Add the SDL2 dev package (confirm the exact name on your revision first):
   ```sh
   pkgman search sdl2            # find the _x86_devel row
   pkgman install sdl2_x86_devel
   ```
2. Build the GUI target (SDL2 is auto-detected once its `.pc` is on the path):
   ```sh
   cd DeBeOS-RDP
   export PKG_CONFIG_PATH=/boot/system/develop/lib/x86/pkgconfig
   setarch x86 make -C CrossPlatform build/haiku-remote-gui \
       PACKAGES="libpng freetype2 harfbuzz" \
       CXXFLAGS="-std=c++20 -O0 -ffp-contract=off --param ggc-min-expand=10 --param ggc-min-heapsize=32768"
   ```
   If the build prints `haiku-remote-gui SKIPPED (SDL2 not found)`, the devel package name
   was different — recheck `pkgman search sdl2` and install the right `_x86_devel`.

## Connecting to a remote instance

A Haiku `app_server` serves its remote protocol on **`localhost:10900`** (loopback only) and
authenticates with a **session cookie** at
`/boot/system/settings/remote_desktop/session_cookie.10900`. The TLS `remote_broker`
(port 10902) is the intended external front door, but it needs a client built WITH OpenSSL.

With a plain (non-TLS) client, reach `app_server` over an **SSH tunnel** instead — no broker,
no OpenSSL needed:
```sh
# on the client machine: forward local 10900 -> the server's loopback 10900
ssh -N -L 10900:localhost:10900 <user>@<server-host>
```
Then, in another Terminal, point the client at the tunnel with the server's cookie:
```sh
# headless capture:
setarch x86 CrossPlatform/build/haiku-remote     --host 127.0.0.1 --port 10900 --cookie <COOKIE> --png ~/shot.png
# interactive window:
setarch x86 CrossPlatform/build/haiku-remote-gui --host 127.0.0.1 --port 10900 --cookie <COOKIE>
```
`<COOKIE>` is the contents of the server's `session_cookie.10900`. The installed binaries are
secondary-arch x86, so run them under `setarch x86`.
