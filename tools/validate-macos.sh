#!/usr/bin/env bash
# validate-macos.sh -- issue #1's macOS platform validation, run on a real Mac.
#
#   tools/validate-macos.sh --host HOST --identity KEY [--user baron] [--ssh-port 22]
#                           [--seconds 10] [--no-live] [--no-gui]
#
# What it does, in order, and what it never does:
#   1. Checks prerequisites (Xcode command-line tools, pkg-config, SDL2, libpng,
#      freetype, OpenSSL; libzstd optional). It NEVER installs anything
#      and never uses sudo: a missing piece is reported with the command to run.
#   2. Builds the CLI and the SDL GUI (make all) and runs every test suite
#      (make test: core, render, profile, library, tunnel, connect-flow,
#      cross-platform, broker trust).
#   3. Live, against a DeBeOS host you can reach with SSH (HOST and KEY): runs
#      the managed launch (haiku-remote --profile, the same code the GUI library
#      uses) in wss (broker), ssh (tunnel) and auto mode, checks each wrote a
#      1280x800 frame, and that no ssh child survives the client's exit.
#   4. Forced kill: SIGKILLs a live tunnel session and checks whether its ssh
#      child survived. On POSIX nothing guarantees that today (Windows has a
#      kill-on-close job object; see the PR), so this is reported as GAP, not
#      PASS or FAIL, and any orphan is cleaned up.
#   5. GUI smoke: runs haiku-remote-gui against the broker for 10 s -- on the
#      real display first (a window opens briefly), then SDL's dummy and
#      offscreen drivers -- and checks it completed the handshake.
# Finally it prints one summary block to paste into issue #1.
#
# Everything it creates lives in one temp directory (profiles, captures, the
# broker token it fetches for the GUI smoke) and is removed on exit.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

HOST="" IDENTITY="" USER_NAME="baron" SSH_PORT=22 SECONDS_LIVE=10 LIVE=1 GUI=1
usage() { sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
while [ $# -gt 0 ]; do
	case "$1" in
		--host) HOST="$2"; shift 2 ;;
		--identity) IDENTITY="$2"; shift 2 ;;
		--user) USER_NAME="$2"; shift 2 ;;
		--ssh-port) SSH_PORT="$2"; shift 2 ;;
		--seconds) SECONDS_LIVE="$2"; shift 2 ;;
		--no-live) LIVE=0; shift ;;
		--no-gui) GUI=0; shift ;;
		-h|--help) usage ;;
		*) echo "unknown argument: $1" >&2; usage ;;
	esac
done
if [ "$LIVE" = 1 ] && { [ -z "$HOST" ] || [ -z "$IDENTITY" ]; }; then
	echo "--host and --identity are required for the live checks (or pass --no-live)" >&2
	exit 2
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/rdp-validate.XXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

RESULTS=()
FAILED=0
record() { # STATUS NAME DETAIL
	RESULTS+=("$(printf '%-5s %-34s %s' "$1" "$2" "$3")")
	if [ "$1" = FAIL ]; then FAILED=1; fi
	printf '%-5s %s -- %s\n' "$1" "$2" "$3"
}

# --- 1. prerequisites -------------------------------------------------------
echo "== prerequisites"
[ "$(uname -s)" = Darwin ] || { echo "this script is for macOS (uname: $(uname -s))" >&2; exit 2; }
if xcode-select -p >/dev/null 2>&1; then
	record PASS "xcode command-line tools" "$(xcode-select -p)"
else
	record FAIL "xcode command-line tools" "missing: run  xcode-select --install"
fi
# Homebrew's OpenSSL is keg-only; point pkg-config at it (read-only query).
if command -v brew >/dev/null 2>&1; then
	for keg in openssl@3 openssl; do
		prefix="$(brew --prefix "$keg" 2>/dev/null || true)"
		if [ -n "$prefix" ] && [ -d "$prefix/lib/pkgconfig" ]; then
			export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
			break
		fi
	done
fi
MISSING=()
command -v pkg-config >/dev/null 2>&1 || MISSING+=(pkg-config)
for pc in sdl2:sdl2 libpng:libpng freetype2:freetype openssl:openssl@3; do
	if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists "${pc%%:*}"; then :; else
		MISSING+=("${pc##*:}")
	fi
done
if [ ${#MISSING[@]} -eq 0 ]; then
	record PASS "build dependencies" "pkg-config, SDL2, libpng, freetype, OpenSSL found"
else
	record FAIL "build dependencies" "missing: brew install ${MISSING[*]}"
fi
if pkg-config --exists libzstd 2>/dev/null; then
	record PASS "libzstd (optional)" "found: the client offers RP_CAP_COMPRESS_ZSTD"
else
	record INFO "libzstd (optional)" "not found (brew install zstd): plain stream only"
fi
if [ "$FAILED" = 1 ]; then
	echo "prerequisites missing; nothing was installed. Fix the above and re-run." >&2
fi

# --- 2. build + tests ---------------------------------------------------------
if [ "$FAILED" = 0 ]; then
	echo "== build and test suites"
	if make all > "$WORK/build.log" 2>&1; then
		if [ -x build/haiku-remote ] && [ -x build/haiku-remote-gui ]; then
			record PASS "build CLI + SDL GUI" "build/haiku-remote, build/haiku-remote-gui"
		else
			record FAIL "build CLI + SDL GUI" "$(grep -E 'SKIPPED' "$WORK/build.log" | head -2 | tr '\n' ' ')"
		fi
	else
		record FAIL "build CLI + SDL GUI" "make all failed: $(grep -E 'error' "$WORK/build.log" | head -2 | tr '\n' ' ')"
	fi
	if make test > "$WORK/test.log" 2>&1; then
		record PASS "test suites (make test)" "$(grep -c '^PASS' "$WORK/test.log") suites passed"
	else
		record FAIL "test suites (make test)" "$(grep -E 'FAIL|FAILED' "$WORK/test.log" | head -3 | tr '\n' ' ')"
	fi
	grep -E '^(PASS|FAIL)|FAILED' "$WORK/test.log" || true
fi

# --- 3. live managed sessions -------------------------------------------------
ssh_pids() { # ssh processes under client $1: its children, or (since the
	# parent-death guard) its children's children
	{ pgrep -P "$1" ssh 2>/dev/null || true
	  for g in $(pgrep -P "$1" 2>/dev/null || true); do
		pgrep -P "$g" ssh 2>/dev/null || true
	  done; } | sort -u
}
ssh_children() { # number of ssh processes under client $1
	ssh_pids "$1" | grep -c . || true
}
host_ssh_count() { # ssh processes talking to HOST, any parent
	{ pgrep -f "ssh .*${HOST}" 2>/dev/null || true; } | wc -l | tr -d ' '
}
png_size() {
	sips -g pixelWidth -g pixelHeight "$1" 2>/dev/null \
		| awk '/pixelWidth/ {w=$2} /pixelHeight/ {h=$2} END {print w "x" h}'
}
if [ "$LIVE" = 1 ] && [ -x build/haiku-remote ]; then
	echo "== live sessions against $USER_NAME@$HOST"
	PROFILES="$WORK/connections.json"
	profile() { # id name mode
		printf '{"id": "%s", "name": "%s", "favorite": false, "mode": "%s", "host": "%s", "remotePort": 10900, "sshUser": "%s", "sshPort": %s, "identityFile": "%s", "localPort": null, "width": 1280, "height": 800, "autoReconnect": false, "knownHostsFile": "%s", "cookieSource": "", "lastConnectedAt": 0, "lastError": ""}' \
			"$1" "$2" "$3" "$HOST" "$USER_NAME" "$SSH_PORT" "$IDENTITY" "$WORK/known_hosts"
	}
	printf '{"version": 1, "profiles": [%s, %s, %s]}\n' \
		"$(profile w1 val-wss wss)" "$(profile s1 val-ssh ssh)" "$(profile a1 val-auto direct)" > "$PROFILES"
	for p in val-wss val-ssh val-auto; do
		before="$(host_ssh_count)"
		set +e
		./build/haiku-remote --profile "$p" --profiles "$PROFILES" \
			--seconds "$SECONDS_LIVE" --output "$WORK/$p.png" > "$WORK/$p.log" 2>&1
		rc=$?
		set -e
		sleep 2
		after="$(host_ssh_count)"
		route="$(grep -m1 '^route:' "$WORK/$p.log" | cut -c1-60 || true)"
		size="$( [ -f "$WORK/$p.png" ] && png_size "$WORK/$p.png" || echo none)"
		if [ "$rc" = 0 ] && [ "$size" = 1280x800 ] && [ "$after" = "$before" ]; then
			record PASS "live $p" "$(grep -m1 '^wrote' "$WORK/$p.log" | sed 's/^wrote [^ ]* //'); frame $size; ssh left: 0; $route"
		else
			record FAIL "live $p" "exit $rc, frame $size, ssh before/after $before/$after; $(tail -2 "$WORK/$p.log" | tr '\n' ' ')"
		fi
	done

	# --- 4. forced kill of a live tunnel session ------------------------------
	echo "== forced kill (SIGKILL) of a live ssh-tunnel session"
	./build/haiku-remote --profile val-ssh --profiles "$PROFILES" --seconds 120 \
		--output "$WORK/kill.png" > "$WORK/kill.log" 2>&1 &
	client=$!
	child=""
	for _ in $(seq 60); do
		child="$(ssh_pids "$client" | head -1)"
		if [ -n "$child" ] && grep -q '^route:' "$WORK/kill.log"; then break; fi
		sleep 0.5
	done
	if [ -z "$child" ]; then
		record FAIL "forced kill: ssh child" "the tunnel never came up: $(tail -2 "$WORK/kill.log" | tr '\n' ' ')"
		kill "$client" 2>/dev/null || true
	else
		guard="$(ps -o ppid= -p "$child" 2>/dev/null | tr -d ' ' || true)"
		[ "$guard" = "$client" ] && guard=""
		kill -9 "$client"
		wait "$client" 2>/dev/null || true
		left=""
		for _ in $(seq 15); do
			left=""
			kill -0 "$child" 2>/dev/null && left="ssh $child"
			[ -n "$guard" ] && kill -0 "$guard" 2>/dev/null && left="$left guard $guard"
			[ -z "$left" ] && break
			sleep 0.2
		done
		if [ -n "$left" ]; then
			record FAIL "forced kill: ssh child" "${left} outlived a SIGKILLed client after 3 s; killed now"
			kill "$child" ${guard:+"$guard"} 2>/dev/null || true
		else
			record PASS "forced kill: ssh child" "ssh $child${guard:+ and its guard $guard} exited with the SIGKILLed client"
		fi
	fi

	# --- 5. GUI smoke ---------------------------------------------------------
	if [ "$GUI" = 1 ] && [ -x build/haiku-remote-gui ]; then
		echo "== GUI smoke (real display, then SDL dummy/offscreen; wss)"
		ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
			-o UserKnownHostsFile="$WORK/known_hosts" -i "$IDENTITY" -p "$SSH_PORT" \
			"$USER_NAME@$HOST" 'cat /boot/system/settings/remote_desktop/token' \
			> "$WORK/token" 2>/dev/null || true
		ssh -o BatchMode=yes -o UserKnownHostsFile="$WORK/known_hosts" -i "$IDENTITY" \
			-p "$SSH_PORT" "$USER_NAME@$HOST" 'cat /boot/system/settings/remote_desktop/broker.pem' \
			> "$WORK/broker.pem" 2>/dev/null || true
		fp="$(openssl x509 -in "$WORK/broker.pem" -noout -fingerprint -sha256 2>/dev/null \
			| sed 's/.*=//; s/://g' | tr 'A-F' 'a-f' || true)"
		if [ ! -s "$WORK/token" ] || [ -z "$fp" ]; then
			record FAIL "GUI smoke" "could not fetch the broker token/certificate over SSH"
		else
			# The real display first -- that is the macOS check -- then SDL's
			# headless drivers. offscreen comes last: on SDL3 via sdl2-compat
			# (Homebrew's "sdl2") it reached for an EGL/GLES library a GPU-less
			# VM does not have. The first driver that holds a session for 10 s
			# wins; every attempt's outcome is kept for the report.
			smoke=""; tried=""
			for driver in default dummy offscreen; do
				if [ "$driver" = default ]; then
					env -u SDL_VIDEODRIVER ./build/haiku-remote-gui --url "wss://$HOST:10902" \
						--token-file "$WORK/token" --known-broker-fingerprint "SHA256:$fp" \
						> "$WORK/gui.log" 2>&1 &
				else
					SDL_VIDEODRIVER="$driver" ./build/haiku-remote-gui --url "wss://$HOST:10902" \
						--token-file "$WORK/token" --known-broker-fingerprint "SHA256:$fp" \
						> "$WORK/gui.log" 2>&1 &
				fi
				gui=$!
				sleep 10
				alive=no; kill -0 "$gui" 2>/dev/null && alive=yes
				kill "$gui" 2>/dev/null || true
				wait "$gui" 2>/dev/null || true
				renderer="$(grep -m1 '^renderer:' "$WORK/gui.log" | sed 's/renderer: //' || true)"
				if [ "$alive" = yes ] && grep -q 'hello ack' "$WORK/gui.log"; then
					smoke="driver ${driver}, renderer ${renderer:-?}: $(grep -m1 'hello ack' "$WORK/gui.log" | cut -c1-60)"
					break
				fi
				tried="${tried}${driver}: running after 10 s: ${alive}; $(tail -2 "$WORK/gui.log" | tr '\n' ' ')| "
			done
			if [ -n "$smoke" ]; then
				record PASS "GUI smoke" "${smoke}${tried:+ (before that: ${tried})}"
			else
				record FAIL "GUI smoke" "$tried"
			fi
		fi
	fi
fi

# --- summary ------------------------------------------------------------------
echo
echo "----- paste below into issue #1 -----"
echo '```'
echo "macOS $(sw_vers -productVersion) ($(uname -m)), DeBeOS-RDP $(git rev-parse --short HEAD 2>/dev/null || echo '?')"
for line in "${RESULTS[@]}"; do echo "$line"; done
if [ "$FAILED" = 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
echo '```'
exit "$FAILED"
