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
#   ./build.sh deps          # check the prerequisites, offer to install them
#
# `app`/`all` end by reporting which of the three front ends were built and
# which were skipped (issue #26): haiku-remote-gui needs SDL2 development files,
# and on a host without them the build succeeds but says so loudly on stderr
# instead of silently omitting the target. `syntax-check` parses
# src/sdl_main.cpp when the SDL2 *headers* are present even if the
# link libraries are not, so a typo in the SDL frontend is caught on such a host.
#
# Every verb first checks the prerequisites and, if any are missing, names them,
# says what each one gates, and asks before installing anything -- it never
# installs or sudoes behind your back, and declining is always allowed, because
# a missing optional dependency only costs the front end or the test case it
# gates. Environment knobs:
#
#   HAIKU_REMOTE_DEP_CHECK=0        skip the check altogether
#   HAIKU_REMOTE_INSTALL_DEPS=yes   answer yes without asking (provisioning)
#   HAIKU_REMOTE_INSTALL_DEPS=no    only report, never ask
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

# ---------------------------------------------------------------------------
# Prerequisites
#
# One row per dependency:
#
#   key|class|what it gates|brew|apt|dnf|pacman|pkgman
#
# class is `required` (nothing builds without it), `optional` (one front end or
# transport is dropped, which the Makefile already reports) or `test` (a test or
# tool cannot run; checked only for verbs that run tests). A package field of
# `-` means this manager has no package for it; `cask:NAME` is a Homebrew cask.
# Written for bash 3.2, which is what /bin/bash on macOS is: no associative
# arrays.
#
# The package names come from docs/build-runbook.md, which installed them on the
# platforms it documents; keep the two in step.
DEP_TABLE='
cxx|required|the C++20 compiler everything is built with|-|g++|gcc-c++|base-devel|gcc
make|required|this script and the Makefile under it|-|make|make|base-devel|make
pkg-config|required|finding every library below|pkgconf|pkg-config|pkgconf-pkg-config|pkgconf|pkgconfig
libpng|required|PNG capture (--output) and the render tests|libpng|libpng-dev|libpng-devel|libpng|libpng16_devel
sdl2|optional|haiku-remote-gui, the SDL2 front end|sdl2|libsdl2-dev|SDL2-devel|sdl2|libsdl2_devel
x11|optional|haiku-remote-x11, the X11 front end|cask:xquartz|libx11-dev|libX11-devel|libx11|-
openssl|optional|ws:// and wss:// (raw TCP still works without it)|openssl|libssl-dev|openssl-devel|openssl|openssl3_devel
python3|test|the mock server, the e2e suite and tools/rp_probe.py|python@3.12|python3|python3|python|-
freetype|test|the mock own rasteriser, for the font e2e cases|freetype|libfreetype6|freetype|freetype2|freetype_devel
fonts|test|the font measurement checks, which read real faces off this host|-|fonts-noto-core|google-noto-sans-fonts|noto-fonts|-
ssh|test|haiku-remote-tunnel-tests (which pass without ssh, but take 60 s instead of 1 s)|-|openssh-client|openssh-clients|openssh|openssh
Xvfb|test|the X11 e2e cases (tests/e2e/x11_e2e.py)|cask:xquartz|xvfb|xorg-x11-server-Xvfb|xorg-server-xvfb|-
'

# Is this dependency present? Each check is the same question the Makefile or
# the test script itself asks, so "present" here means that consumer is
# satisfied -- not merely that some file of that name exists.
dep_present() {
	case "$1" in
	cxx)			command -v "${CXX:-g++}" >/dev/null 2>&1 ;;
	make)			command -v make >/dev/null 2>&1 ;;
	pkg-config)		command -v "${PKG_CONFIG:-pkg-config}" >/dev/null 2>&1 ;;
	libpng|sdl2|x11|openssl)
				command -v "${PKG_CONFIG:-pkg-config}" >/dev/null 2>&1 \
					&& "${PKG_CONFIG:-pkg-config}" --exists "$1" ;;
	python3)		command -v python3 >/dev/null 2>&1 ;;
	freetype)
		# What the mock loads, loaded the way the mock loads it, so this
		# cannot drift from it: a host libfreetype reachable through
		# ctypes. pkg-config is the wrong question -- the client's own
		# FreeType is vendored (Makefile:14-21) and the mock dlopens the
		# library at runtime rather than linking it.
		# -B: importing the mock must not leave a tools/__pycache__
		# behind in the working tree. -I: no user site, no stray path.
		command -v python3 >/dev/null 2>&1 && python3 -IB -c '
import sys
sys.path.insert(0, "tools")
import rp_mock_server
rp_mock_server.FreeTypeOracle._load()' >/dev/null 2>&1 ;;
	fonts)
		# A real face on the host, because the font tests measure the
		# host's files rather than estimating: without them seven checks
		# fail (docs/build-runbook.md:64-69). Either family satisfies them.
		find /usr/share/fonts /usr/local/share/fonts /usr/share/X11/fonts \
			"${HOME:-/nonexistent}/.fonts" \
			"${HOME:-/nonexistent}/.local/share/fonts" \
			\( -iname 'notosans*.tt*' -o -iname 'dejavusans*.tt*' \) \
			-print 2>/dev/null | head -n 1 | grep -q . ;;
	ssh)			command -v ssh >/dev/null 2>&1 ;;
	Xvfb)			command -v Xvfb >/dev/null 2>&1 ;;
	*)			return 0 ;;
	esac
}

# Rows that make no sense on this platform. Haiku has no X11 and no Xvfb, and
# never will; both it and macOS ship the system fonts the font tests need. Not
# reporting these beats offering a package that does not exist.
dep_applies() {
	case "$(uname -s)" in
	Haiku)	case "$1" in x11|Xvfb|fonts) return 1 ;; esac ;;
	Darwin)	case "$1" in fonts) return 1 ;; esac ;;
	esac
	return 0
}

# The host's package manager, and which column of the table to read.
dep_manager() {
	for mgr in brew apt-get dnf pacman pkgman; do
		command -v "$mgr" >/dev/null 2>&1 || continue
		case "$mgr" in
		brew)		echo "brew 4" ;;
		apt-get)	echo "apt 5" ;;
		dnf)		echo "dnf 6" ;;
		pacman)		echo "pacman 7" ;;
		pkgman)		echo "pkgman 8" ;;
		esac
		return 0
	done
	return 1
}

# Check the prerequisites for $1: `build` (the required and optional rows),
# `test` (those plus the test-only rows, because a build verb is about to run
# the tests) or `report` (the same rows as `test`, but nothing is about to be
# built, so it does not comment on what the build will do). Reports what is
# missing, offers to install it, and returns non-zero only when something
# *required* is still missing afterwards, so the `deps` verb has a status worth
# testing.
check_deps() {
	local mode="$1" want="$1"
	[ "$mode" = report ] && want=test || true
	# What the caller cannot read off the return status, which is about
	# *required* dependencies only: `missing` is how many rows were missing at
	# all (so `deps` does not claim nothing was), `ran` whether the check even
	# happened.
	DEPS_MISSING=0
	DEPS_CHECKED=1
	if [ "${HAIKU_REMOTE_DEP_CHECK:-1}" = 0 ]; then
		DEPS_CHECKED=0
		return 0
	fi

	local missing_keys=() missing_required=0 row key class gates
	local IFS_save=$IFS
	while IFS='|' read -r key class gates _; do
		[ -n "$key" ] || continue
		[ "$class" = test ] && [ "$want" != test ] && continue
		dep_applies "$key" || continue
		dep_present "$key" && continue
		missing_keys+=("$key")
		[ "$class" = required ] && missing_required=1
		printf 'prerequisite: %s is missing (%s: %s)\n' \
			"$key" "$class" "$gates" >&2
	done <<-EOF
	$(printf '%s\n' "$DEP_TABLE" | grep '|')
	EOF
	IFS=$IFS_save
	DEPS_MISSING=${#missing_keys[@]}
	[ ${#missing_keys[@]} -eq 0 ] && return 0

	# What to run, if anything can be: the manager's column for each missing
	# key, casks split out because `brew install --cask` is its own command.
	local mgr_spec column mgr pkgs=() casks=() unknown=()
	if ! mgr_spec=$(dep_manager); then
		echo "prerequisites: no package manager recognised here; install the above by hand" >&2
		[ "$missing_required" = 1 ] && return 1
		return 0
	fi
	mgr=${mgr_spec% *}
	column=${mgr_spec#* }
	for key in "${missing_keys[@]}"; do
		local pkg
		pkg=$(printf '%s\n' "$DEP_TABLE" | grep "^$key|" | cut -d'|' -f"$column")
		case "$pkg" in
		-|'')		unknown+=("$key") ;;
		cask:*)		casks+=("${pkg#cask:}") ;;
		*)		pkgs+=("$pkg") ;;
		esac
	done
	# A cask or package named twice (XQuartz is both x11 and Xvfb) is one install.
	if [ ${#casks[@]} -gt 0 ]; then
		casks=($(printf '%s\n' "${casks[@]}" | sort -u))
	fi
	if [ ${#pkgs[@]} -gt 0 ]; then
		pkgs=($(printf '%s\n' "${pkgs[@]}" | sort -u))
	fi
	if [ ${#unknown[@]} -gt 0 ]; then
		echo "prerequisites: $mgr has no package here for: ${unknown[*]} -- install those by hand" >&2
		case "$(uname -s)" in
		Darwin) echo "  (a missing compiler or make on macOS is the Xcode command line tools: xcode-select --install)" >&2 ;;
		esac
	fi

	local commands=()
	case "$mgr" in
	brew)
		[ ${#pkgs[@]} -gt 0 ] && commands+=("brew install ${pkgs[*]}")
		[ ${#casks[@]} -gt 0 ] && commands+=("brew install --cask ${casks[*]}")
		;;
	apt)	[ ${#pkgs[@]} -gt 0 ] && commands+=("sudo apt-get install -y ${pkgs[*]}") ;;
	dnf)	[ ${#pkgs[@]} -gt 0 ] && commands+=("sudo dnf install -y ${pkgs[*]}") ;;
	pacman)	[ ${#pkgs[@]} -gt 0 ] && commands+=("sudo pacman -S --needed ${pkgs[*]}") ;;
	pkgman)	[ ${#pkgs[@]} -gt 0 ] && commands+=("pkgman install -y ${pkgs[*]}") ;;
	esac
	if [ ${#commands[@]} -eq 0 ]; then
		[ "$missing_required" = 1 ] && return 1
		return 0
	fi

	echo "prerequisites: this would install them:" >&2
	local cmd
	for cmd in "${commands[@]}"; do
		echo "    $cmd" >&2
	done

	local answer=""
	case "${HAIKU_REMOTE_INSTALL_DEPS:-ask}" in
	yes|y)	answer=y ;;
	no|n)	echo "prerequisites: not installing (HAIKU_REMOTE_INSTALL_DEPS=no)" >&2 ;;
	*)
		if [ -t 0 ] && [ -t 2 ]; then
			# The prompt and the answer go to the terminal, not into a
			# pipeline reading this script's stdout.
			printf 'Run it now? [y/N] ' >&2
			read -r answer || answer=""
		else
			echo "prerequisites: not a terminal, so not asking; run the above, or set HAIKU_REMOTE_INSTALL_DEPS=yes" >&2
		fi
		;;
	esac

	case "$answer" in
	y|Y|yes|YES)
		for cmd in "${commands[@]}"; do
			echo "+ $cmd" >&2
			# Deliberately not `set -e`-fatal: a package name this
			# table has wrong on some distro should not take the
			# build with it, and the recheck below is the real test.
			eval "$cmd" || echo "prerequisites: '$cmd' failed" >&2
		done
		# Say what the install actually achieved rather than assuming.
		local still=()
		for key in "${missing_keys[@]}"; do
			dep_present "$key" || still+=("$key")
		done
		DEPS_MISSING=${#still[@]}
		if [ ${#still[@]} -gt 0 ]; then
			echo "prerequisites: still missing after the install: ${still[*]}" >&2
			missing_required=0
			while IFS='|' read -r key class gates _; do
				[ -n "$key" ] || continue
				case " ${still[*]} " in *" $key "*)
					[ "$class" = required ] && missing_required=1 ;;
				esac
			done <<-EOF
			$(printf '%s\n' "$DEP_TABLE" | grep '|')
			EOF
		else
			echo "prerequisites: all present now" >&2
			missing_required=0
		fi
		;;
	*)
		if [ "$mode" = report ]; then
			: # `deps` builds nothing, so it has nothing to promise.
		elif [ "$missing_required" = 1 ]; then
			echo "prerequisites: continuing without them -- the build will fail" >&2
		else
			echo "prerequisites: continuing without them -- what they gate is skipped" >&2
		fi
		;;
	esac

	[ "$missing_required" = 1 ] && return 1
	return 0
}

case "${1:-all}" in
	deps)
		# The only verb that says so when there is nothing to say: the
		# build verbs stay quiet on a host that already has everything.
		# Exit 0 means the build and its tests can run, which is not the
		# same as "everything is installed" -- so say which it was.
		status=0
		check_deps report || status=1
		if [ "${DEPS_CHECKED}" = 0 ]; then
			echo "prerequisites: not checked (HAIKU_REMOTE_DEP_CHECK=0)" >&2
		elif [ "${DEPS_MISSING}" = 0 ]; then
			echo "prerequisites: nothing missing" >&2
		elif [ "$status" = 0 ]; then
			echo "prerequisites: $DEPS_MISSING missing, none of them required -- the build and its tests still run, minus what is named above" >&2
		fi
		exit "$status"
		;;
	test)
		# `|| true`: a missing prerequisite is reported above, and then make
		# gets to speak for itself -- its error is the authority on whether
		# the build can proceed, and declining an install is not an error.
		check_deps test || true
		exec "${MAKE[@]}" test
		;;
	app)
		check_deps build || true
		exec "${MAKE[@]}" all
		;;
	syntax-check)
		check_deps build || true
		exec "${MAKE[@]}" syntax-check
		;;
	all)
		check_deps test || true
		"${MAKE[@]}" test
		exec "${MAKE[@]}" all
		;;
	run)
		shift
		check_deps build || true
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
		echo "usage: $0 {test|app|all|syntax-check|deps|run [client options]}" >&2
		exit 2
		;;
esac
