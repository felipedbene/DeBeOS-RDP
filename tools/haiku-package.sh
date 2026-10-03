#!/bin/sh
# haiku-package.sh -- build the client and produce an installable .hpkg, on Haiku.
# Automates docs/packaging-on-haiku.md.
#
#   tools/haiku-package.sh                 # headless haiku-remote -> hpkg
#   tools/haiku-package.sh --gui           # also include the SDL GUI frontend
#   tools/haiku-package.sh --install       # build, package, and pkgman install
#   tools/haiku-package.sh --version 1.2.0-1 --out /path/pkg.hpkg
set -eu
cd "$(dirname "$0")/.."   # repo root

[ "$(uname -s)" = "Haiku" ] || { echo "error: run on Haiku (produces a Haiku hpkg)" >&2; exit 2; }

VERSION=1.0.0-1
OUT=""
INSTALL=0
WITH_GUI=0
while [ $# -gt 0 ]; do
	case "$1" in
		--version) VERSION="$2"; shift 2;;
		--out) OUT="$2"; shift 2;;
		--install) INSTALL=1; shift;;
		--gui) WITH_GUI=1; shift;;
		-h|--help) sed -n '2,9p' "$0"; exit 0;;
		*) echo "unknown arg: $1" >&2; exit 2;;
	esac
done
ARCH=x86_gcc2                      # the PRIMARY arch, even for an x86 secondary binary (#54)
OUT="${OUT:-$PWD/debeos_rdp-$VERSION-$ARCH.hpkg}"

# 1. Build (build.sh is Haiku-aware: setarch x86 + PKG_CONFIG_PATH + lean flags).
./build.sh app

BIN=CrossPlatform/build/haiku-remote
[ -x "$BIN" ] || { echo "error: build did not produce $BIN" >&2; exit 1; }

# 2. Runtime deps. The libpng package depends on which the build linked.
case "$(setarch x86 pkg-config --modversion libpng 2>/dev/null)" in
	1.6*) PNG_REQ=libpng16_x86;;
	1.2*) PNG_REQ=libpng12_x86;;
	*)    PNG_REQ=libpng16_x86;;
esac
GUI_REQ=""
if [ "$WITH_GUI" = 1 ]; then
	[ -x CrossPlatform/build/haiku-remote-gui ] || {
		echo "error: --gui but haiku-remote-gui was not built (install sdl2_x86_devel)" >&2; exit 1; }
	GUI_REQ="	sdl2_x86
"
fi

# 3. Stage + .PackageInfo (architecture MUST be x86_gcc2, or pkgman says the
#    opaque 'not installable', #54).
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/apps/DeBeOS-RDP"
cp "$BIN" "$STAGE/apps/DeBeOS-RDP/"
[ "$WITH_GUI" = 1 ] && cp CrossPlatform/build/haiku-remote-gui "$STAGE/apps/DeBeOS-RDP/"
VER_NODASH="${VERSION%-*}"
cat > "$STAGE/.PackageInfo" <<INFO
name            debeos_rdp
version         $VERSION
architecture    $ARCH
summary         "DeBeOS-RDP remote desktop client"
description     "The headless haiku-remote protocol client (and the SDL GUI frontend if built with --gui)."
packager        "DeBeOS builder <builder@debeos.local>"
vendor          "DeBeOS"
copyright        "2026 Felipe De Bene"
licenses        "MIT"
provides {
	debeos_rdp = $VER_NODASH
}
requires {
	haiku
	freetype_x86
	harfbuzz_x86
	$PNG_REQ
$GUI_REQ}
INFO

# 4. Create the hpkg from the staged dir (-C), NOT -b (which would be empty).
package create -C "$STAGE" "$OUT"
echo "created: $OUT"
package list -i "$OUT" | grep -iE 'name|version|architecture|requires' || true

# 5. Optional install.
if [ "$INSTALL" = 1 ]; then
	pkgman install -y "$OUT"
	echo "installed. run with: setarch x86 /boot/system/apps/DeBeOS-RDP/haiku-remote"
fi
