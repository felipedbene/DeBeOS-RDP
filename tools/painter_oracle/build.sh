#!/usr/bin/env bash
#
# Build the Painter oracle against a Haiku source tree and print the goldens.
#
#   tools/painter_oracle/build.sh [HAIKU_TREE [--full]] > tests/painter_goldens.inc
#
# --full appends every frame run-length encoded, for a per-pixel diff.
# --text-blend prints tests/text_blend_goldens.inc instead (text_blend.cpp).
# --text prints tests/text_goldens.inc (text_oracle.cpp), --text-full the same
# with every frame run-length encoded. Either builds the text oracle: every unit
# with ORACLE_TEXT and text_shim/ ahead of shim/, plus app_server's
# AGGTextRenderer, FontCacheEntry and FontEngine and the vendored FreeType in
# third_party/freetype, into its own directory ($out-text).
#
# HAIKU_TREE defaults to ~/Projects/Haiku-Graviton (CLAUDE.md). The Makefile
# targets painter-goldens and painter-oracle-check wrap this, and pin the
# server revision the goldens come from (ORACLE_REF there).
#
# ORACLE_SANITIZE=address builds the oracle under AddressSanitizer, so a
# blitter that reads past its source bitmap aborts the run. ORACLE_CXXFLAGS is
# appended to the compile flags as is (e.g. -DORACLE_BITMAP_PAD_ROWS=2 to
# render a tree from before Haiku-Graviton #812, see oracle.cpp; -DORACLE_SIMD
# takes BilinearSimd with a C model of its x loop, which needs a tree with the
# __i386__ guards around it removed). ORACLE_MODEL_818 is described below.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
tree="${1:-${HAIKU_TREE:-$HOME/Projects/Haiku-Graviton}}"
out="${ORACLE_BUILD:-$repo/build/painter_oracle}"
cxx="${CXX:-g++}"

if [ ! -f "$tree/src/servers/app/drawing/Painter/Painter.cpp" ]; then
	echo "painter oracle: no Haiku tree at $tree (set HAIKU_TREE)" >&2
	exit 2
fi

text=
case "${2:-}" in
--text|--text-full) text=1; out="$out-text" ;;
esac

mkdir -p "$out"
python3 "$here/extract.py" "$tree" "$out/extracted.cpp" ${text:+--text}

painter="$tree/src/servers/app/drawing/Painter"
agg="$tree/src/libs/agg/src"
# -ffp-contract=off as in the client's Makefile, so both sides round the same
# way on every host; see README "Shapes are antialiased" for what that means
# for an arm64 server built with contraction on.
flags=(-std=gnu++17 -O1 -ffp-contract=off -w)
# ORACLE_MODEL_818=crop|crop-int renders with the bilinear index scaled by the
# crop rather than the whole bitmap, Haiku-Graviton #818's proposed fix
# (model_818.py), for diffing against the tree as it is. Never for goldens.
# The fix landed as #819 (8ec57ad); crop-int matches it on every integral
# crop. A tree with #819 no longer has the code the model rewrites, so the
# model says so and stops.
rm -rf "$out/model_818"
if [ -n "${ORACLE_MODEL_818:-}" ]; then
	mkdir -p "$out/model_818"
	python3 "$here/model_818.py" "$tree" "$out/model_818" "$ORACLE_MODEL_818"
	flags+=(-I "$out/model_818")
fi
if [ -n "$text" ]; then
	# The server's text sources are copied, unmodified, into one directory of
	# their own: a quoted #include looks beside the including file first, so
	# compiled in place AGGTextRenderer.h would reach the real
	# Transformable.h, FontCache.h and ServerFont.h next to it rather than the
	# shims. In the copy, only these files are beside each other.
	textsrc="$out/text-src"
	rm -rf "$textsrc"
	mkdir -p "$textsrc"
	for file in drawing/Painter/AGGTextRenderer.cpp drawing/Painter/AGGTextRenderer.h \
			font/FontCacheEntry.cpp font/FontCacheEntry.h font/FontEngine.cpp \
			font/FontEngine.h font/GlyphLayoutEngine.h IntRect.cpp IntRect.h \
			IntPoint.cpp IntPoint.h; do
		cp "$tree/src/servers/app/$file" "$textsrc/"
	done
	flags+=(-DORACLE_TEXT -DORACLE_FONT_DIR="\"$repo/tests/fonts\""
		-I "$textsrc"
		-I "$here/text_shim"
		-I "$repo/third_party/freetype/include")
fi
flags+=(
	-I "$here/shim"
	-I "$tree/headers/libs/agg"
	-I "$painter"
	-I "$painter/drawing_modes"
	-I "$painter/bitmap_painter"
	-I "$tree/src/servers/app/drawing"
	-I "$tree/src/servers/app"
	-I "$repo/tests")
if [ -n "$text" ]; then
	# util/OpenHashTable.h, its TypeOperation.h and utf8_functions.h, behind
	# every shim.
	flags+=(-I "$tree/headers/private" -I "$tree/headers/private/interface"
		-I "$tree/headers/private/shared")
fi
link=()
if [ -n "${ORACLE_SANITIZE:-}" ]; then
	flags+=(-fsanitize="$ORACLE_SANITIZE" -fno-omit-frame-pointer)
	link+=(-fsanitize="$ORACLE_SANITIZE")
fi
# shellcheck disable=SC2206 # word splitting is the point
flags+=(${ORACLE_CXXFLAGS:-})

sources=(
	"$here/oracle.cpp"
	"$here/text_blend.cpp"
	"$out/extracted.cpp"
	"$painter/drawing_modes/PixelFormat.cpp"
	"$tree/src/servers/app/drawing/PatternHandler.cpp"
	"$tree/src/servers/app/drawing/drawing_support.cpp"
	"$agg/agg_sqrt_tables.cpp"
	"$agg/agg_arc.cpp"
	"$agg/agg_bezier_arc.cpp"
	"$agg/agg_curves.cpp"
	"$agg/agg_rounded_rect.cpp"
	"$agg/agg_trans_affine.cpp"
	"$agg/agg_vcgen_stroke.cpp"
)

if [ -n "$text" ]; then
	sources+=(
		"$here/text_oracle.cpp"
		"$textsrc/AGGTextRenderer.cpp"
		"$textsrc/FontCacheEntry.cpp"
		"$textsrc/FontEngine.cpp"
		"$textsrc/IntRect.cpp"
		"$textsrc/IntPoint.cpp"
		"$agg/agg_vcgen_contour.cpp"
	)
fi

objects=()
if [ -n "$text" ]; then
	# The vendored FreeType, compiled as the client's Makefile compiles it
	# (FREETYPE_CFLAGS there).
	ft="$repo/third_party/freetype"
	cc="${CC:-cc}"
	mkdir -p "$out/freetype"
	for source in $(cat "$ft/sources.txt"); do
		object="$out/freetype/$(echo "$source" | tr / _).o"
		"$cc" -O2 -w -DFT2_BUILD_LIBRARY -I "$ft/haiku-config" -I "$ft/include" \
			-c "$ft/src/$source" -o "$object"
		objects+=("$object")
	done
fi
for source in "${sources[@]}"; do
	object="$out/$(basename "${source%.cpp}").o"
	"$cxx" "${flags[@]}" -c "$source" -o "$object"
	objects+=("$object")
done
"$cxx" "${link[@]}" -o "$out/painter-oracle" "${objects[@]}"
"$out/painter-oracle" "${@:2}"
