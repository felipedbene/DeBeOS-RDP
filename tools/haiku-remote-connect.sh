#!/bin/sh
# haiku-remote-connect.sh
#
# Open an SSH tunnel to a remote Haiku app_server, fetch its session cookie over
# that same SSH, and launch the DeBeOS-RDP client -- so the usual case is just:
#
#     tools/haiku-remote-connect.sh                 # one host in hosts.txt
#     tools/haiku-remote-connect.sh --host mybox    # pick one of several
#
# HOSTS FILE (default ~/.config/haiku-remote/hosts.txt, or $RDP_HOSTS / --hosts):
# one host per line, comma-separated, '#' comments and blank lines ignored:
#
#     # host[,user],key
#     34.213.138.118,baron,/boot/home/config/settings/ssh/id_ed25519
#     10.0.0.9,/boot/home/config/settings/ssh/other_id      # user defaults
#
# The cookie is read live from the server each run (handles reboots); override
# with --cookie / --cookie-file if you must. Tunnel is backgrounded (ssh -f) and
# reused; kill with:  pkill -f "ssh -f -N -L ${RDP_LOCAL_PORT:-10900}"
#
# Any setting: env var or flag (flag wins). Trailing args pass to the client
# (e.g. --png shot.png, --width/--height).
set -eu

# Optional config file (belt and suspenders): a sourced shell snippet that can
# set any RDP_* default. Use ': "${VAR:=value}"' in it so environment and
# flags still win; a plain 'VAR=value' would force the value. Override the path
# with RDP_CONFIG.
RDP_CONFIG="${RDP_CONFIG:-$HOME/.config/haiku-remote/config}"
[ -f "$RDP_CONFIG" ] && . "$RDP_CONFIG"

RDP_HOSTS="${RDP_HOSTS:-$HOME/.config/haiku-remote/hosts.txt}"
RDP_HOST="${RDP_HOST:-}"
RDP_USER="${RDP_USER:-user}"
RDP_KEY="${RDP_KEY:-$HOME/config/settings/ssh/id_ed25519}"
RDP_COOKIE="${RDP_COOKIE:-}"
RDP_COOKIE_FILE="${RDP_COOKIE_FILE:-}"
RDP_PORT="${RDP_PORT:-10900}"
RDP_LOCAL_PORT="${RDP_LOCAL_PORT:-10900}"
RDP_CLIENT="${RDP_CLIENT:-build/haiku-remote-gui}"
RDP_FONT="${RDP_FONT:-/boot/system/data/fonts/ttfonts/NotoSans-Regular.ttf}"
RDP_MONO_FONT="${RDP_MONO_FONT:-/boot/system/data/fonts/ttfonts/NotoMono-Regular.ttf}"
user_set=0; key_set=0

while [ $# -gt 0 ]; do
	case "$1" in
		--host) RDP_HOST="$2"; shift 2;;
		--user) RDP_USER="$2"; user_set=1; shift 2;;
		--key) RDP_KEY="$2"; key_set=1; shift 2;;
		--cookie) RDP_COOKIE="$2"; shift 2;;
		--cookie-file) RDP_COOKIE_FILE="$2"; shift 2;;
		--hosts) RDP_HOSTS="$2"; shift 2;;
		--port) RDP_PORT="$2"; shift 2;;
		--local-port) RDP_LOCAL_PORT="$2"; shift 2;;
		--client) RDP_CLIENT="$2"; shift 2;;
		--) shift; break;;
		-h|--help) sed -n '2,33p' "$0"; exit 0;;
		*) break;;
	esac
done

# Resolve host/user/key from the hosts file.
if [ -f "$RDP_HOSTS" ]; then
	lines="$(grep -vE '^[[:space:]]*(#|$)' "$RDP_HOSTS" || true)"
	line=""
	if [ -n "$RDP_HOST" ]; then
		line="$(printf '%s\n' "$lines" | awk -F, -v h="$RDP_HOST" '$1==h{print;exit}')"
	else
		n="$(printf '%s\n' "$lines" | grep -c . || true)"
		if [ "$n" = 1 ]; then line="$lines"
		elif [ "$n" = 0 ]; then :
		else
			echo "several hosts in $RDP_HOSTS -- choose with --host <host>:" >&2
			printf '%s\n' "$lines" | awk -F, '{print "  "$1}' >&2
			exit 2
		fi
	fi
	if [ -n "$line" ]; then
		RDP_HOST="$(printf '%s' "$line" | cut -d, -f1)"
		nf="$(printf '%s' "$line" | awk -F, '{print NF}')"
		if [ "$nf" -ge 3 ]; then
			[ "$user_set" = 1 ] || RDP_USER="$(printf '%s' "$line" | cut -d, -f2)"
			[ "$key_set" = 1 ]  || RDP_KEY="$(printf '%s' "$line" | cut -d, -f3)"
		else
			[ "$key_set" = 1 ]  || RDP_KEY="$(printf '%s' "$line" | cut -d, -f2)"
		fi
	fi
fi
[ -n "$RDP_HOST" ] || { echo "error: no host -- pass --host or add $RDP_HOSTS" >&2; exit 2; }

SSH_BASE="ssh -i $RDP_KEY -o StrictHostKeyChecking=accept-new"

# Fetch the session cookie live over SSH unless one was supplied.
if [ -z "$RDP_COOKIE" ] && [ -n "$RDP_COOKIE_FILE" ]; then
	RDP_COOKIE="$(tr -d '[:space:]' < "$RDP_COOKIE_FILE")"
fi
if [ -z "$RDP_COOKIE" ]; then
	echo "fetching session cookie from $RDP_USER@$RDP_HOST ..."
	RDP_COOKIE="$($SSH_BASE "$RDP_USER@$RDP_HOST" \
		"cat /boot/system/settings/remote_desktop/session_cookie.$RDP_PORT" \
		2>/dev/null | tr -d '[:space:]' || true)"
	[ -n "$RDP_COOKIE" ] || { echo "error: could not read session_cookie.$RDP_PORT (is app_server up?)" >&2; exit 1; }
fi

echo "tunnel: 127.0.0.1:$RDP_LOCAL_PORT -> $RDP_USER@$RDP_HOST:$RDP_PORT"
if $SSH_BASE -f -N -L "$RDP_LOCAL_PORT:localhost:$RDP_PORT" \
	-o ExitOnForwardFailure=yes "$RDP_USER@$RDP_HOST" 2>/tmp/rdp-ssh.err; then
	echo "  tunnel established (backgrounded)"
elif grep -qiE 'in use|cannot listen|already' /tmp/rdp-ssh.err; then
	echo "  local port busy -- reusing existing tunnel"
else
	echo "ssh tunnel failed:" >&2; cat /tmp/rdp-ssh.err >&2; exit 1
fi

# Point the client at specific fonts only when they resolve. The defaults are
# Haiku paths; on macOS/Linux they do not exist, so leave the client to its own
# font discovery rather than forcing it at a missing file.
[ -f "$RDP_FONT" ] && export HAIKU_REMOTE_FONT="$RDP_FONT"
[ -f "$RDP_MONO_FONT" ] && export HAIKU_REMOTE_MONO_FONT="$RDP_MONO_FONT"

# `setarch x86` forces the 32-bit personality the hybrid build needs to run on
# Haiku x86_64. It is a Linux (util-linux) tool that is neither present nor
# needed on macOS or Linux, so gate it on Haiku -- same as build.sh's RUN wrapper.
SETARCH=""
if [ "$(uname -s)" = "Haiku" ] && command -v setarch >/dev/null 2>&1; then
	SETARCH="setarch x86"
fi
echo "launching: $RDP_CLIENT${SETARCH:+ ($SETARCH)} -> 127.0.0.1:$RDP_LOCAL_PORT"
exec $SETARCH "$RDP_CLIENT" \
	--host 127.0.0.1 --port "$RDP_LOCAL_PORT" --cookie "$RDP_COOKIE" "$@"
