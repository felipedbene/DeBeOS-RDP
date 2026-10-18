#!/usr/bin/env bash
#
# Is third_party/freetype the FreeType the DeBeOS image renders with? Runs the
# image's own libfreetype.so (Haiku arm64 ELF) side by side with the vendored
# copy and compares, glyph by glyph, everything app_server reads from FreeType:
# the loaded outline, the metrics and advances, the rendered bitmap in LCD,
# grey and mono, unhinted and unscaled loads, and kerning.
#
#   tools/freetype_identity/check.sh LIBFREETYPE.so FONT... [--sizes MIN MAX]
#
# LIBFREETYPE.so is lib/libfreetype.so.6.12.3 out of the image's
# freetype-2.6.3_bootstrap-1-arm64.hpkg (a Haiku-Graviton build extracts it to
# generated.arm64/build_packages/freetype-2.6.3_bootstrap-1-arm64; `make
# freetype-identity-check` finds it there). Haiku and Linux share the arm64
# ELF ABI and the library needs nothing from libroot.so but plain C library
# calls, so on a Linux arm64 host it loads with a stub libroot.so that
# resolves them from glibc. Elsewhere this script says so and stops.
#
# Exit status 0 when every comparison is equal, 1 when any differs, 2 when the
# check cannot run.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
out="${FREETYPE_IDENTITY_BUILD:-$repo/build/freetype_identity}"
cc="${CC:-cc}"

if [ $# -lt 2 ]; then
	sed -n '3,20p' "$0" >&2
	exit 2
fi
library="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
shift
if [ "$(uname -s)-$(uname -m)" != "Linux-aarch64" ]; then
	echo "freetype identity: SKIPPED (needs a Linux arm64 host to load the" \
		"image's arm64 libfreetype.so)" >&2
	exit 2
fi
sizes="5 30"
fonts=()
while [ $# -gt 0 ]; do
	case "$1" in
	--sizes) sizes="$2 $3"; shift 3 ;;
	*) fonts+=("$1"); shift ;;
	esac
done

mkdir -p "$out/freetype"
ft="$repo/third_party/freetype"
objects=()
for source in $(cat "$ft/sources.txt"); do
	object="$out/freetype/$(echo "$source" | tr / _).o"
	if [ ! -f "$object" ] || [ "$ft/src/$source" -nt "$object" ]; then
		"$cc" -O2 -w -DFT2_BUILD_LIBRARY -I "$ft/haiku-config" -I "$ft/include" \
			-c "$ft/src/$source" -o "$object"
	fi
	objects+=("$object")
done
"$cc" -O2 -I "$ft/include" "$here/ftdiff.c" "${objects[@]}" -ldl -o "$out/ftdiff"
"$cc" -shared -fPIC "$here/libroot_stub.c" -o "$out/libroot.so"
cp "$library" "$out/libfreetype-haiku.so"
LD_LIBRARY_PATH="$out" FTDIFF_SIZES="$sizes" \
	"$out/ftdiff" "$out/libfreetype-haiku.so" "${fonts[@]}"
