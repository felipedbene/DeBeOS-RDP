#!/usr/bin/env bash
#
# Build the DeBeOS-RDP client.
#
# There is one client: the portable C++ one at the repo root. This script is
# a thin front end over its Makefile, kept because the verbs below are what the
# docs, CI and muscle memory already use.
#
# The Swift/AppKit macOS client that used to live in Sources/ was an early
# experiment and is frozen. It has moved, with its build script and full history,
# to its own read-only repo: github.com/felipedbene/DeBeOS-RDP-swift.
#
# Usage:
#   ./build.sh test          # build and run the protocol test suite
#   ./build.sh app           # build the client binaries
#   ./build.sh all           # both (default)
#   ./build.sh syntax-check  # parse the SDL frontend without linking it
#   ./build.sh run [opts]    # build, then run the best available front end
#
# `app`/`all` end by reporting which of the three front ends were built and
# which were skipped (issue #26): haiku-remote-gui needs SDL2 development files,
# and on a host without them the build succeeds but says so loudly on stderr
# instead of silently omitting the target. `syntax-check` parses
# src/sdl_main.cpp when the SDL2 *headers* are present even if the
# link libraries are not, so a typo in the SDL frontend is caught on such a host.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

# Haiku hosts get a low-RAM-friendly CXXFLAGS default. On the 32-bit x86_gcc2
# *hybrid* only, the primary compiler is gcc2 (no C++20) and the secondary x86
# pkg-config dir is off the default path, so builds must also run under
# `setarch x86` (which puts the gcc13 secondary toolchain first on PATH) with
# PKG_CONFIG_PATH set. RUN wraps launching the x86 secondary binaries the same
# way.
#
# setarch is gated on `getarch -p` (the PRIMARY architecture, independent of any
# setarch already in effect) reporting x86_gcc2. Every other Haiku -- x86_64,
# arm64, riscv64 -- has a single modern primary toolchain and no x86 secondary,
# and `setarch x86` there dies with 'Unsupported architecture "x86"'. Other
# hosts are unaffected.
MAKE=(make)
RUN=()
if [ "$(uname -s)" = "Haiku" ]; then
	# -O0 + aggressive GC keep the compiler within reach on low-RAM machines.
	: "${CXXFLAGS:=-std=c++20 -O0 --param ggc-min-expand=10 --param ggc-min-heapsize=32768}"
	export CXXFLAGS
	if [ "$(getarch -p 2>/dev/null)" = "x86_gcc2" ]; then
		export PKG_CONFIG_PATH="/boot/system/develop/lib/x86/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
		MAKE=(setarch x86 make)
		RUN=(setarch x86)
	fi
fi

case "${1:-all}" in
	test)
		exec "${MAKE[@]}" test
		;;
	app)
		exec "${MAKE[@]}" all
		;;
	syntax-check)
		exec "${MAKE[@]}" syntax-check
		;;
	all)
		"${MAKE[@]}" test
		exec "${MAKE[@]}" all
		;;
	run)
		shift
		"${MAKE[@]}" all
		# Prefer the richest front end that exists: SDL GUI, then X11, then the
		# headless/protocol binary.
		if [ -x build/haiku-remote-gui ]; then
			exec "${RUN[@]}" build/haiku-remote-gui "$@"
		elif [ -n "${DISPLAY:-}" ] \
			&& [ -x build/haiku-remote-x11 ]; then
			exec "${RUN[@]}" build/haiku-remote-x11 "$@"
		fi
		exec "${RUN[@]}" build/haiku-remote "$@"
		;;
	icon|install)
		echo "$1 built the archived macOS app bundle; it now lives at github.com/felipedbene/DeBeOS-RDP-swift" >&2
		exit 2
		;;
	*)
		echo "usage: $0 {test|app|all|syntax-check|run [client options]}" >&2
		exit 2
		;;
esac
