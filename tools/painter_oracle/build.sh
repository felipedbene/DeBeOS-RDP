#!/usr/bin/env bash
#
# Build the Painter oracle against a Haiku source tree and print the goldens.
#
#   tools/painter_oracle/build.sh [HAIKU_TREE [--full]] > tests/painter_goldens.inc
#
# --full appends every frame run-length encoded, for a per-pixel diff.
#
# HAIKU_TREE defaults to ~/Projects/Haiku-Graviton (CLAUDE.md). The Makefile
# targets painter-goldens and painter-oracle-check wrap this.
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

mkdir -p "$out"
python3 "$here/extract.py" "$tree" "$out/extracted.cpp"

painter="$tree/src/servers/app/drawing/Painter"
agg="$tree/src/libs/agg/src"
# -ffp-contract=off as in the client's Makefile, so both sides round the same
# way on every host; see README "Shapes are antialiased" for what that means
# for an arm64 server built with contraction on.
flags=(-std=gnu++17 -O1 -ffp-contract=off -w
	-I "$here/shim"
	-I "$tree/headers/libs/agg"
	-I "$painter"
	-I "$painter/drawing_modes"
	-I "$painter/bitmap_painter"
	-I "$tree/src/servers/app/drawing"
	-I "$tree/src/servers/app"
	-I "$repo/tests")

sources=(
	"$here/oracle.cpp"
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

objects=()
for source in "${sources[@]}"; do
	object="$out/$(basename "${source%.cpp}").o"
	"$cxx" "${flags[@]}" -c "$source" -o "$object"
	objects+=("$object")
done
"$cxx" -o "$out/painter-oracle" "${objects[@]}"
"$out/painter-oracle" "${@:2}"
