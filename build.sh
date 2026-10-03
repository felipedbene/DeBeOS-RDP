#!/usr/bin/env bash
#
# Build the DeBeOS-RDP client.
#
# There is one client: the portable C++ one under CrossPlatform/. This script is
# a thin front end over its Makefile, kept because the verbs below are what the
# docs, CI and muscle memory already use.
#
# The Swift/AppKit macOS client that used to live in Sources/ was an early
# experiment and is frozen — see archive/swift-prototype/. Its build script moved
# with it (archive/swift-prototype/build-macos.sh) rather than being deleted, so
# the prototype is still buildable by anyone who wants to look at it.
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
# CrossPlatform/src/sdl_main.cpp when the SDL2 *headers* are present even if the
# link libraries are not, so a typo in the SDL frontend is caught on such a host.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

# On a 32-bit x86_gcc2 Haiku hybrid the default compiler is gcc2 (no C++20) and
# the secondary pkg-config dir is off the default path, so builds must run under
# `setarch x86` with PKG_CONFIG_PATH set. Wire that up here so the verbs below
# just work on Haiku; other hosts are unaffected. RUN wraps launching the x86
# secondary binaries the same way.
MAKE=(make)
RUN=()
if [ "$(uname -s)" = "Haiku" ]; then
	export PKG_CONFIG_PATH="/boot/system/develop/lib/x86/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
	# -O0 + aggressive GC keep the compiler within reach on low-RAM machines.
	: "${CXXFLAGS:=-std=c++20 -O0 --param ggc-min-expand=10 --param ggc-min-heapsize=32768}"
	export CXXFLAGS
	MAKE=(setarch x86 make)
	RUN=(setarch x86)
fi

case "${1:-all}" in
	test)
		exec "${MAKE[@]}" -C CrossPlatform test
		;;
	app)
		exec "${MAKE[@]}" -C CrossPlatform all
		;;
	syntax-check)
		exec "${MAKE[@]}" -C CrossPlatform syntax-check
		;;
	all)
		"${MAKE[@]}" -C CrossPlatform test
		exec "${MAKE[@]}" -C CrossPlatform all
		;;
	run)
		shift
		"${MAKE[@]}" -C CrossPlatform all
		# Prefer the richest front end that exists: SDL GUI, then X11, then the
		# headless/protocol binary.
		if [ -x CrossPlatform/build/haiku-remote-gui ]; then
			exec "${RUN[@]}" CrossPlatform/build/haiku-remote-gui "$@"
		elif [ -n "${DISPLAY:-}" ] \
			&& [ -x CrossPlatform/build/haiku-remote-x11 ]; then
			exec "${RUN[@]}" CrossPlatform/build/haiku-remote-x11 "$@"
		fi
		exec "${RUN[@]}" CrossPlatform/build/haiku-remote "$@"
		;;
	icon|install)
		echo "$1 built the archived macOS app bundle; see archive/swift-prototype/build-macos.sh" >&2
		exit 2
		;;
	*)
		echo "usage: $0 {test|app|all|syntax-check|run [client options]}" >&2
		exit 2
		;;
esac
