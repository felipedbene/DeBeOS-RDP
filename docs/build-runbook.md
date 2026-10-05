# Build runbook: Linux x86_64, Haiku x86_64, Haiku x86 (32-bit hybrid)

Copy-pasteable steps to build, test and run the C++ client on three kinds of host.
**Every command and every output quoted here was run and recorded on the host its
section names** (see *Verified on* in each section). Where something was not
verified, the text says so.

For what the client is and how its frontends behave, read [`client.md`](client.md).
For producing an installable `.hpkg` on a 32-bit Haiku, read
[`packaging-on-haiku.md`](packaging-on-haiku.md).

## At a glance

| | Linux x86_64 | Haiku x86_64 | Haiku x86 (x86_gcc2 hybrid) |
|---|---|---|---|
| Compiler | distro `g++` | `g++` (gcc 13) | `setarch x86 g++` (gcc 13); plain `g++` is gcc 2.95 or absent |
| `./build.sh` | works as is | works as is | works as is: wraps `make` in `setarch x86` and sets `PKG_CONFIG_PATH` itself |
| plain `make` | works | works | `setarch x86 make`, with `PKG_CONFIG_PATH` exported |
| CMake | works | works | `setarch x86 cmake`, with `PKG_CONFIG_PATH` exported |
| `haiku-remote` (headless) | built | built | built |
| `haiku-remote-gui` (SDL2) | built when SDL2 dev files exist (Ubuntu: yes; Amazon Linux 2023: only with CMake `-DHAIKU_REMOTE_FETCH_SDL2=ON`) | built | built |
| `haiku-remote-x11` | built | SKIPPED (no X11 on Haiku) | SKIPPED |
| `make test` | all 8 suites pass | 7 pass; trust-tests fails on the 120 s watchdog (stock-Haiku kernel bug, see below) | same as x86_64 |

All three hosts share one source tree and one build. The differences are the
package names, and the hybrid's `setarch`.

---

## 1. Linux x86_64

**Verified on (2026-10-05):**
- Amazon Linux 2023 workstation: g++ 11.5.0, GNU Make 4.3, CMake 3.22.2,
  libpng 1.6.37, FreeType 2.13.2, HarfBuzz 7.0.0, OpenSSL 3.5.8, libX11 1.8.10.
- A pristine Amazon Linux 2023 x86_64 host (al2023-ami-2023.12.20260930.0), for the
  package list below.
- Ubuntu 24.04 in a container: g++ 13.3.0. Builds all three frontends.
  `--help` was run there; no window was opened (no display in the container).

### 1.1 Prerequisites

Amazon Linux 2023 / Fedora family:

```sh
sudo dnf install -y gcc-c++ make pkgconf-pkg-config git cmake \
    libpng-devel freetype-devel harfbuzz-devel openssl-devel libX11-devel \
    google-noto-sans-fonts google-noto-sans-mono-fonts
```

Ubuntu 24.04 / Debian family:

```sh
sudo apt-get install -y g++ make pkg-config git cmake python3 openssh-client \
    libpng-dev libfreetype-dev libharfbuzz-dev libssl-dev libx11-dev libsdl2-dev \
    fonts-noto-core fonts-noto-mono
```

Notes:
- **Fonts are a real prerequisite.** The client rasterises text from font files on
  the host, and the test suite measures them. On a pristine Amazon Linux 2023 without
  the two Noto packages, `haiku-remote-tests` failed 7 checks, for example
  `FAIL: bold does not measure the same as regular` and
  `FAIL: a real face was measured, not the no-face estimate, for the fixed-pitch face`.
  With them installed it passed.
- **Amazon Linux 2023 has no SDL2 package**, so `make` builds 2 of 3 frontends there.
  To get `haiku-remote-gui`, use CMake with `-DHAIKU_REMOTE_FETCH_SDL2=ON`
  (section 1.4), which also needs `sudo dnf install -y libXext-devel`.
- `openssh-client` is not needed to build. Without an `ssh` binary on the PATH,
  `haiku-remote-tunnel-tests` still passed, but took 60 s instead of under 1 s.
- OpenSSL is optional. Without it the client builds with raw TCP only, and `ws://` /
  `wss://` are rejected (see `client.md`).

### 1.2 Get the source

```sh
git clone https://github.com/felipedbene/DeBeOS-RDP
cd DeBeOS-RDP
```

### 1.3 Build and test with `build.sh` / `make`

```sh
./build.sh app      # the client binaries          (= make all)
./build.sh test     # build, then run all 8 suites (= make test)
```

Expected output of `./build.sh app` on Amazon Linux 2023 (24.5 s; `build.sh` runs a
serial make, so add `MAKEFLAGS=-j$(nproc)` if you want it faster):

```
haiku-remote-gui: SKIPPED syntax-check (SDL2 headers absent, cannot even syntax-check src/sdl_main.cpp)
=== client frontends ===
haiku-remote:     built (headless / PNG capture)
haiku-remote-gui: SKIPPED (SDL2 development files not found; install SDL2 to build the SDL frontend)
haiku-remote-x11: built (X11 frontend)
built 2 of 3 frontends (SKIPPED:haiku-remote-gui)
```

On Ubuntu 24.04 with `libsdl2-dev` the same step ends `built 3 of 3 frontends`.

Expected tail of `./build.sh test` (exit 0):

```
PASS - 485 checks
PASS - 76 render checks
PASS - 89 profile checks
PASS - 74 library checks
PASS - 56 tunnel/transport checks
PASS - 108 connect-flow checks
PASS - 22 cross-platform checks
PASS - 168 broker-trust checks
```

The first count depends on the host's fonts: 485 on the workstation; 478 plus
`(7 SKIPPED, see SKIP: lines)` on Ubuntu with `fonts-noto-core`, which has no
condensed cuts. A `SKIP:` line means "this host has no such font file". It is not a
failure.

Plain `make` works the same way: `make -j$(nproc)`, then `make test`.

### 1.4 Build with CMake

```sh
cmake -S . -B out
cmake --build out -j$(nproc)
ctest --test-dir out
```

Expected: `100% tests passed, 0 tests failed out of 8`. Without SDL2 the configure
step warns `haiku-remote-gui: SKIPPED (SDL2 not found; ...)`.

To build the SDL frontend where the distro has no SDL2 (Amazon Linux 2023), let CMake
fetch it:

```sh
sudo dnf install -y libXext-devel      # SDL2's X11 backend needs it
cmake -S . -B out-sdl -DHAIKU_REMOTE_FETCH_SDL2=ON
cmake --build out-sdl -j$(nproc)
```

Verified on pristine Amazon Linux 2023: builds `haiku-remote`, `haiku-remote-gui` and
`haiku-remote-x11`, and all 8 tests pass. Without `libXext-devel`, configure stops with
`*** ERROR: Missing Xext.h, maybe you need to install the libxext-dev package?`.

### 1.5 Run

```sh
./build/haiku-remote --help         # exit 0; prints usage and the exit-status table
```

`--help` returned 0 for every built frontend on every Linux host above.

Smoke test against the protocol mock, no server needed. The mock gates on a session
cookie like `app_server` does, so point the client at the cookie file it publishes:

```sh
python3 -u tools/rp_mock_server.py --port 10900 --torture --once \
    --cookie-file /tmp/mock-cookie &
MOCK=$!
sleep 1
./build/haiku-remote --port 10900 --cookie-file /tmp/mock-cookie \
    --width 1024 --height 700 --seconds 2 --output /tmp/torture.png
wait $MOCK; echo "mock exit $?"     # 0 = every gating raster check held
```

Expected:

```
hello ack: version 1, capabilities 0x5, session ... generation 1
wrote /tmp/torture.png after 348 messages (server hung up)
mock exit 0
```

The mock's own log ends `RASTER CHECKS: PASS`.

Interactive X11 smoke (on a desktop session): start the mock without `--torture
--once`, then `./build/haiku-remote-x11 --host 127.0.0.1 --port 10900 --cookie-file
/tmp/mock-cookie`. Verified on the workstation's X display: an 800x600 "Haiku Remote"
window was mapped and the client printed `hello ack`.

---

## 2. Haiku x86_64

**Verified on (2026-10-05):** Haiku nightly **hrev60207** x86_64
(`haiku-master-hrev60207-x86_64-anyboot`), installed to disk, in a 4-vCPU/8 GiB
QEMU/KVM guest. gcc 13.3.0 (2023_08_10), GNU Make 4.4.1, pkg-config 0.29.2, CMake
4.4.3, git 2.54.0; libpng 1.6.53, FreeType 2.14.3, HarfBuzz 14.5.1, OpenSSL 3.5.9,
SDL2 2.32.10.

### 2.1 Prerequisites

```sh
pkgman install -y gcc binutils make pkgconfig cmake haiku_devel \
    freetype_devel harfbuzz_devel libpng16_devel openssl3_devel libsdl2_devel
```

- No compiler ships with the stock image (`g++: command not found`), and `make`,
  `pkg-config` and `cmake` are missing too. `git` is pre-installed.
- `libsdl2_devel` is only for `haiku-remote-gui`; `openssl3_devel` only for
  `ws://`/`wss://`; `cmake` only for the CMake path.
- **Disk space:** this installs about 30 packages. The live "Try Haiku" session of
  the anyboot image has about 130 MiB free on `/boot`, which is not enough for
  toolchain plus build. Install Haiku to a real disk first.
- `python3` is not on the PATH. `python3.10` comes in as a dependency of the list
  above and is enough to run `tools/rp_mock_server.py`.

### 2.2 Get the source

```sh
git clone --template= https://github.com/felipedbene/DeBeOS-RDP /boot/home/DeBeOS-RDP
cd /boot/home/DeBeOS-RDP
```

Clone into `/boot/home`, not onto a read-only or packagefs path. `--template=` skips
git's hook templates. On hrev60207 x86_64 a plain `git clone` also worked; the flag is
kept because it is harmless.

### 2.3 Build and test with `build.sh`

```sh
./build.sh app
./build.sh test
```

On Haiku, `build.sh` defaults `CXXFLAGS` to
`-std=c++20 -O0 --param ggc-min-expand=10 --param ggc-min-heapsize=32768`, which keeps
the compiler small on low-RAM machines. It does **not** use `setarch` on x86_64:
`getarch -p` reports `x86_64`, and `setarch x86` would fail here with
`Error: Unsupported architecture "x86"`.

Expected end of `./build.sh app` (47 s):

```
=== client frontends ===
haiku-remote:     built (headless / PNG capture)
haiku-remote-gui: built (SDL2 frontend)
haiku-remote-x11: SKIPPED (X11 development files not found; install X11 to build the X11 frontend)
built 2 of 3 frontends (SKIPPED:haiku-remote-x11)
```

The X11 skip is expected: Haiku has no X server, and the SDL frontend is the
interactive one there.

**`./build.sh test` exits 2 on stock Haiku, and that is expected.** The suites run in
order and `make` stops at the first failure. The last suite, trust-tests, fails on its
watchdog after 120 s:

```
FAIL: TLS integration did not finish within 120s -- hung (on DeBeOS/Haiku, see kernel issue #605: recv() never returns after a peer reset)
```

This is a kernel bug in upstream Haiku, not a client bug: a `recv()` blocked on a
socket whose peer then resets it never returns. DeBeOS fixed it in its own kernel
(DeBeOS issue #605, fix #606), and stock Haiku still has it. The 120 s watchdog
exists so the suite fails loudly instead of wedging `make test`. Set
`HAIKU_REMOTE_TRUST_TIMEOUT=10` to fail faster (verified: the message then reads
`did not finish within 10s`, after 10 s).

To see every suite's result regardless, run them one by one:

```sh
for t in tests render-tests profile-tests library-tests tunnel-tests \
         connect-tests xplatform-tests trust-tests; do
    echo "--- $t"; ./build/haiku-remote-$t 2>&1 | grep -E '^(PASS|FAIL)|failed'
done
```

Results recorded on hrev60207 x86_64:

| Suite | Result |
|---|---|
| haiku-remote-tests | `PASS - 478 checks (7 SKIPPED, see SKIP: lines)`. The skips are the condensed and fixed-pitch-italic checks; stock Haiku ships no such font files |
| haiku-remote-render-tests | `PASS - 76 render checks` |
| haiku-remote-profile-tests | `PASS - 89 profile checks` |
| haiku-remote-library-tests | `PASS - 74 library checks` |
| haiku-remote-tunnel-tests | `PASS - 56 tunnel/transport checks` |
| haiku-remote-connect-tests | `PASS - 108 connect-flow checks` |
| haiku-remote-xplatform-tests | `PASS - 22 cross-platform checks` |
| haiku-remote-trust-tests | `FAIL` after 120 s, the kernel bug above (exit 1) |

> Before the fixes that landed with this runbook, render-tests failed 3 checks here
> (`the scaled glyphs are as densely inked as the unscaled ones`,
> `a scaled mono string inks more than 3x the identity count`,
> `a 10x view scale is filled, not a lattice`). These were not test flakes. Stock
> Haiku ships all nine Noto Sans weights, and the client picked `NotoSans-Thin.ttf`
> for regular text. See *Defects found* below.

### 2.4 Plain `make` and CMake

```sh
make -j4                     # default CXXFLAGS -O2: 14 s; same frontend report as above
rm -rf out && cmake -S . -B out && cmake --build out -j4 && ctest --test-dir out
```

Both verified. `make` links `-lnetwork` on Haiku automatically. `ctest` ends
`88% tests passed, 1 tests failed out of 8`: tests 1-7 pass, and
`haiku-remote-trust-tests` hits the same 120 s watchdog.

### 2.5 Run

```sh
./build/haiku-remote --help          # exit 0
./build/haiku-remote-gui --help      # exit 0
```

Headless smoke against the mock (verified: client exit 0, mock `RASTER CHECKS: PASS`):

```sh
python3.10 -u tools/rp_mock_server.py --port 10931 --torture --once \
    --cookie-file /tmp/mock-cookie &
sleep 4
./build/haiku-remote --host 127.0.0.1 --port 10931 --cookie-file /tmp/mock-cookie \
    --width 1024 --height 700 --seconds 2 --output /boot/home/torture.png
```

GUI smoke, on the Haiku desktop (verified: a "Haiku Remote" window opened and drew the
mock's scene, text included):

```sh
python3.10 -u tools/rp_mock_server.py --port 10935 --cookie-file /tmp/mock-cookie &
sleep 4
./build/haiku-remote-gui --host 127.0.0.1 --port 10935 --cookie-file /tmp/mock-cookie \
    --width 800 --height 600
```

The SDL frontend prints the OpenGL add-on it loaded (`llvmpipe` in a VM) and then
`hello ack: version 1, capabilities 0x5, ...`.

---

## 3. Haiku x86 (32-bit, x86_gcc2 hybrid)

**Verified on (2026-10-05):** Haiku nightly **hrev60207** x86_gcc2h
(`haiku-master-hrev60207-x86_gcc2h-anyboot`), installed to disk, in a 4-vCPU/4 GiB
QEMU/KVM guest. `uname -m` = `BePC`, `getarch -p` = `x86_gcc2`. Secondary toolchain:
`setarch x86 g++` = gcc 13.3.0 (2023_08_10). Legacy primary: gcc 2.95.3 (not installed
by default). libpng16_x86 1.6.53, freetype_x86 2.14.1, harfbuzz_x86 14.5.1,
openssl3_x86 3.5.9, libsdl2_x86 2.32.10.

The one thing to know: the **primary** compiler on a hybrid is legacy gcc 2.95, kept
for BeOS binary compatibility, and it cannot compile C++20. The modern compiler is
the **secondary** `x86` toolchain, which `setarch x86` puts first on the PATH. The
libraries the client links must be the matching `_x86` secondary packages.

### 3.1 Prerequisites

```sh
pkgman install -y make pkgconfig gcc_x86 haiku_devel haiku_x86_devel \
    freetype_x86_devel harfbuzz_x86_devel libpng16_x86_devel \
    openssl3_x86_devel libsdl2_x86_devel cmake_x86
```

- `gcc_x86` pulls in `binutils_x86`. Do **not** install `gcc`: that is gcc 2.95.
- `haiku_devel` *and* `haiku_x86_devel` are both needed (system headers).
- Every library is the `_x86_devel` variant. The plain names (`freetype_devel`, ...)
  are for the primary gcc2 architecture and do not satisfy the build.
- `pkgconfig` (primary) is fine. The secondary `.pc` files live in
  `/boot/system/develop/lib/x86/pkgconfig`, which is why `PKG_CONFIG_PATH` is set below.
- The PNG package is `libpng16_x86_devel` on hrev60207; older images ship
  `libpng12_x86_devel` (see `packaging-on-haiku.md`). Check with `pkgman search libpng`.
- `python3.10` is available for the mock (the `_x86` build is pulled in as a
  dependency).

### 3.2 Get the source

Same as x86_64:

```sh
git clone --template= https://github.com/felipedbene/DeBeOS-RDP /boot/home/DeBeOS-RDP
cd /boot/home/DeBeOS-RDP
```

### 3.3 Build and test with `build.sh` (recommended)

```sh
./build.sh app
./build.sh test
```

`build.sh` reads `getarch -p`. When it reports `x86_gcc2`, the script runs `make`
under `setarch x86` and prepends `/boot/system/develop/lib/x86/pkgconfig` to
`PKG_CONFIG_PATH`. You do not need either by hand. Verified on hrev60207: the compile
lines show the `_x86` header paths (`-I/packages/freetype_x86-2.14.1-2/.self/develop/headers/x86/freetype2 ...`),
and the build still picks gcc 13 after gcc 2.95 is installed.

Expected end of `./build.sh app` (41 s), identical to x86_64:

```
haiku-remote:     built (headless / PNG capture)
haiku-remote-gui: built (SDL2 frontend)
haiku-remote-x11: SKIPPED (X11 development files not found; install X11 to build the X11 frontend)
built 2 of 3 frontends (SKIPPED:haiku-remote-x11)
```

`file build/haiku-remote` reports `ELF 32-bit LSB shared object, Intel 80386`.

`./build.sh test` exits 2 for the same reason as on x86_64: trust-tests hits the
120 s watchdog. To run every suite, use the loop from 2.3 with `setarch x86` in front
of each binary. Results recorded on hrev60207 x86_gcc2h:

| Suite | Result |
|---|---|
| haiku-remote-tests | `PASS - 478 checks (7 SKIPPED, see SKIP: lines)` |
| haiku-remote-render-tests | `PASS - 76 render checks` |
| haiku-remote-profile-tests | `PASS - 89 profile checks` |
| haiku-remote-library-tests | `PASS - 74 library checks` |
| haiku-remote-tunnel-tests | `PASS - 56 tunnel/transport checks` |
| haiku-remote-connect-tests | `PASS - 108 connect-flow checks` |
| haiku-remote-xplatform-tests | `PASS - 22 cross-platform checks` |
| haiku-remote-trust-tests | `FAIL` after 120 s, the stock-Haiku kernel bug (see 2.3) |

> Before the fixes that landed with this runbook, render-tests failed 5 checks on the
> hybrid: the 3 font checks from 2.3, plus
> `wire cap 4 (B_SQUARE_CAP) extends a full radius past each endpoint` and
> `... inks the square reference pixel count`. The cap pair only failed on 32-bit x86,
> where gcc's default is `-march=pentium -mfpmath=387` (`__FLT_EVAL_METHOD__ 2`). See
> *Defects found*.

### 3.4 Plain `make` and CMake

Outside `build.sh`, apply the same two things yourself:

```sh
export PKG_CONFIG_PATH=/boot/system/develop/lib/x86/pkgconfig
setarch x86 make -j4

rm -rf out
setarch x86 cmake -S . -B out && setarch x86 cmake --build out -j4
setarch x86 ctest --test-dir out
```

All three verified: `make` gives the same frontend report (15 s at `-O2`), and
`ctest` ends `88% tests passed, 1 tests failed out of 8`, the one failure being the
trust-suite timeout described above.

For a RAM-starved machine, build only the headless client with the small-compiler
flags `build.sh` would use:

```sh
setarch x86 make build/haiku-remote \
    CXXFLAGS="-std=c++20 -O0 --param ggc-min-expand=10 --param ggc-min-heapsize=32768"
```

### 3.5 Run

```sh
setarch x86 ./build/haiku-remote --help       # exit 0
setarch x86 ./build/haiku-remote-gui --help   # exit 0
```

On hrev60207 the 32-bit binaries also ran *without* `setarch x86` (`--help` exit 0,
and a full capture against the mock exit 0). The runtime loader resolves the `x86`
libraries on its own. Using `setarch x86` is still the documented, safe form.

The headless and GUI smoke tests from 2.5 work unchanged with `setarch x86` in front.
Both were verified on the hybrid: capture exit 0 with `RASTER CHECKS: PASS`, and the
SDL window drew the mock scene, text included.

To package the result as an `.hpkg`, continue with
[`packaging-on-haiku.md`](packaging-on-haiku.md).

---

## Troubleshooting

Every symptom below was reproduced while writing this runbook, except the two rows
marked *(from #73)*.

| Symptom | Cause | Fix |
|---|---|---|
| Dozens of errors, starting with `types.hpp:4: warning: No include path in which to find array`, `No include path in which to find cstdint`, `g++: unrecognized option '-MP'`, `cc1plus: Invalid option '-Wextra'` | Hybrid, compiled with gcc 2.95 (`g++ --version` prints `2.95.3-haiku-...`) | Use `./build.sh`, or prefix by hand: `setarch x86 make` |
| `make: g++: No such file or directory` / `Error 127` on the hybrid | Plain `make` without `setarch`, and gcc 2.95 not installed, so there is no primary `g++` at all | Same: `./build.sh` or `setarch x86 make` |
| `fatal error: ft2build.h: No such file or directory` | pkg-config returned nothing, so no `-I` flags reached the compiler. On the hybrid: `PKG_CONFIG_PATH` not exported when running `setarch x86 make` by hand, or one `_x86_devel` package missing. The Makefile asks for all modules in one call, so one missing module empties the result for all of them | `export PKG_CONFIG_PATH=/boot/system/develop/lib/x86/pkgconfig`; probe each module on its own (`setarch x86 pkg-config --cflags freetype2`, then `harfbuzz`, then `libpng`) and install the one that fails |
| `Error: Unsupported architecture "x86"` | `setarch x86` on a Haiku that has no x86 secondary (x86_64, arm64) | Drop it. `build.sh` only applies it when `getarch -p` is `x86_gcc2` |
| `fatal error: stdlib.h: No such file` *(from #73 / packaging doc)* | Haiku system headers absent | `pkgman install haiku_devel` (+ `haiku_x86_devel` on the hybrid) |
| Plain `make` on Haiku arm64 dies with `aarch64-unknown-haiku-g++: not found`, Error 127 *(from #73)* | Make's built-in `CXX` on that host names a cross-compiler | Fixed in the Makefile by #73. On an older checkout, `make CXX=g++` |
| `haiku-remote-gui: SKIPPED (SDL2 development files not found ...)` | No SDL2 dev package | Haiku: `libsdl2_devel` / `libsdl2_x86_devel`. Ubuntu: `libsdl2-dev`. Amazon Linux 2023 has none: use CMake `-DHAIKU_REMOTE_FETCH_SDL2=ON` |
| `haiku-remote-x11: SKIPPED` on Haiku | Expected, no X11 | Use `haiku-remote-gui` |
| CMake: `*** ERROR: Missing Xext.h, maybe you need to install the libxext-dev package?` | `-DHAIKU_REMOTE_FETCH_SDL2=ON` builds SDL2's X11 backend | `libXext-devel` (dnf) / `libxext-dev` (apt) |
| `FAIL: TLS integration did not finish within 120s -- hung (... issue #605 ...)`, and `make test` exits 2 | Stock Haiku kernel: `recv()` never returns after a peer reset. Fixed in DeBeOS, not in upstream Haiku | Not a client defect. Read the other suites with the per-suite loop (2.3). `HAIKU_REMOTE_TRUST_TIMEOUT=N` shortens the wait |
| Linux: `FAIL: bold does not measure the same as regular`, `FAIL: a real face was measured, not the no-face estimate ...` | Host has (almost) no font files | Install the Noto packages from 1.1 |
| Linux: `haiku-remote-tunnel-tests` takes ~60 s | No `ssh` binary on the PATH | Install `openssh-client` (passes either way) |
| `connect to 127.0.0.1:... failed: no session cookie ...`, exit 3, against the mock | The mock gates on a cookie like `app_server` does | Start the mock with `--cookie-file FILE` and pass the same `--cookie-file FILE` to the client |
| `unknown argument: --png`, exit 2 | No such option | The PNG path is `--output FILE.png` |
| `python3: command not found` on Haiku | Haiku packages Python as `python3.10` | Run `python3.10 tools/rp_mock_server.py ...` |
| Compiler crawls with low CPU, or is killed, on a small machine | Out of RAM, swapping | `build.sh` already uses `-O0` and small GC params on Haiku. Build one target (`make build/haiku-remote`) and close other apps |
| render-tests `scaled glyphs ... densely inked` / `10x view scale ... lattice` / `B_SQUARE_CAP` fail on Haiku | Checkout predates the fixes below | Update |

## Defects found while verifying this runbook

Running the suites on stock Haiku nightlies, rather than on DeBeOS's minimal image or
on Linux, exposed two client defects. Both are fixed in the same change as this
runbook:

1. **Regular text rendered in a Thin face on stock Haiku.** Font discovery took every
   file of the Noto Sans family and accepted the first whose FreeType bold/italic
   flags matched. A Thin, ExtraLight, Light or Black file has neither flag, so it
   "matched" Regular. Directory order then picked `NotoSans-Thin.ttf` for regular,
   `NotoSans-ThinItalic.ttf` for italic and `NotoSansMono-Thin.ttf` for fixed-pitch.
   That broke three render checks and, more importantly, made the client's text (and
   the string widths it replies to the server) the wrong weight. app_server classifies
   a style named light/thin as `B_LIGHT_FACE` and heavy/black as `B_HEAVY_FACE`, and
   never hands one out for a plain query. `text_engine.cpp` now does the same, and it
   orders a family's files by OS/2 weight class, so Regular beats Medium/SemiBold
   whatever order the directory returns. The regression check
   `test_a_plain_style_never_resolves_to_a_light_or_heavy_cut` failed on hrev60207
   before the fix and passes after.
2. **Square and butt cap ends depended on floating-point rounding.** A pixel centre can
   sit exactly on a cap edge. The test compared `t` against `1 + radius/length`, both
   inexact quotients, so IEEE double (SSE) and x87's 80-bit intermediates disagreed,
   and the B_SQUARE_CAP golden failed on 32-bit x86. `surface.cpp` now compares the
   unnormalised projection against multiples of the length, which is exact for these
   inputs at any precision. The two cap checks failed on the hybrid before the fix and
   pass after, with gcc's default flags. `-fexcess-precision=standard` and
   `-ffloat-store` did not fix them; only `-msse2 -mfpmath=sse` did, and the code fix
   makes that unnecessary.

## How the Haiku hosts were set up (for reproducing this)

No physical Haiku machine was used. Both Haiku hosts were QEMU/KVM guests on an x86_64
Linux host with `/dev/kvm`, booted from the official nightly anyboot images
(`haiku-nightly.cdn.haiku-os.org/<arch>/haiku-master-hrev<N>-<arch>-anyboot.zip`):

- The anyboot `/boot` volume is 650 MiB with about 110-130 MiB free, too small for the
  toolchain. Each guest therefore got a second 40 GiB disk, and a headless install was
  done onto it with Haiku's own tools: `mkfs -t bfs`, `copyattr -d -r` of
  `/boot/system/{packages,settings,cache,var,non-packaged}` and of `/boot/home`, then
  `makebootable`. The guest was then rebooted from that disk.
- SSH: OpenSSH is pre-installed, but login needs `PubkeyAuthentication yes` in
  `/boot/system/settings/ssh/sshd_config`, a key in
  `/boot/home/config/settings/ssh/authorized_keys`, and sshd started by absolute path
  (`/boot/system/bin/sshd`). Those were pre-seeded into the image, along with a
  `UserBootscript` line that starts sshd.
- Guest networking used `passt` (`-netdev stream`), with an `e1000` NIC.

None of this is needed on a Haiku machine you already have installed. Start at
section 2.1 or 3.1.
