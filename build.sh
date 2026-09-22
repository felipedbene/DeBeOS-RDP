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
#   ./build.sh run [opts]    # build, then run the best available front end
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

case "${1:-all}" in
	test)
		exec make -C CrossPlatform test
		;;
	app)
		exec make -C CrossPlatform all
		;;
	all)
		make -C CrossPlatform test
		exec make -C CrossPlatform all
		;;
	run)
		shift
		make -C CrossPlatform all
		# Prefer the richest front end that exists: SDL GUI, then X11, then the
		# headless/protocol binary.
		if [ -x CrossPlatform/build/haiku-remote-gui ]; then
			exec CrossPlatform/build/haiku-remote-gui "$@"
		elif [ -n "${DISPLAY:-}" ] \
			&& [ -x CrossPlatform/build/haiku-remote-x11 ]; then
			exec CrossPlatform/build/haiku-remote-x11 "$@"
		fi
		exec CrossPlatform/build/haiku-remote "$@"
		;;
	icon|install)
		echo "$1 built the archived macOS app bundle; see archive/swift-prototype/build-macos.sh" >&2
		exit 2
		;;
	*)
		echo "usage: $0 {test|app|all|run [client options]}" >&2
		exit 2
		;;
esac
