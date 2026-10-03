#!/bin/sh
# haiku-remote-connect.sh
#
# Open an SSH tunnel to a remote Haiku app_server and launch the DeBeOS-RDP
# client through it. The tunnel is backgrounded (ssh -f) and left running so it
# can be reused; kill it with:  pkill -f "ssh -f -N -L ${RDP_LOCAL_PORT:-10900}"
#
# Every setting has a default and can be overridden by an env var OR a flag
# (flag wins). Anything after the recognised flags is passed straight to the
# client (e.g. --png shot.png for the headless build, or --width/--height).
#
#   setting          env var          flag            default
#   server host/IP   RDP_HOST         --host          (required)
#   session cookie   RDP_COOKIE       --cookie        (required; or --cookie-file)
#   cookie file      RDP_COOKIE_FILE  --cookie-file   -
#   app_server port  RDP_PORT         --port          10900
#   ssh user         RDP_USER         --user          user
#   ssh private key  RDP_KEY          --key           ~/config/settings/ssh/id_ed25519
#   local port       RDP_LOCAL_PORT   --local-port    10900
#   client binary    RDP_CLIENT       --client        CrossPlatform/build/haiku-remote-gui
#   sans font        RDP_FONT         -               /boot/system/data/fonts/ttfonts/NotoSans-Regular.ttf
#   mono font        RDP_MONO_FONT    -               /boot/system/data/fonts/ttfonts/NotoMono-Regular.ttf
set -eu

RDP_HOST="${RDP_HOST:-}"
RDP_COOKIE="${RDP_COOKIE:-}"
RDP_COOKIE_FILE="${RDP_COOKIE_FILE:-}"
RDP_PORT="${RDP_PORT:-10900}"
RDP_USER="${RDP_USER:-user}"
RDP_KEY="${RDP_KEY:-$HOME/config/settings/ssh/id_ed25519}"
RDP_LOCAL_PORT="${RDP_LOCAL_PORT:-10900}"
RDP_CLIENT="${RDP_CLIENT:-CrossPlatform/build/haiku-remote-gui}"
RDP_FONT="${RDP_FONT:-/boot/system/data/fonts/ttfonts/NotoSans-Regular.ttf}"
RDP_MONO_FONT="${RDP_MONO_FONT:-/boot/system/data/fonts/ttfonts/NotoMono-Regular.ttf}"

while [ $# -gt 0 ]; do
	case "$1" in
		--host) RDP_HOST="$2"; shift 2;;
		--cookie) RDP_COOKIE="$2"; shift 2;;
		--cookie-file) RDP_COOKIE_FILE="$2"; shift 2;;
		--port) RDP_PORT="$2"; shift 2;;
		--user) RDP_USER="$2"; shift 2;;
		--key) RDP_KEY="$2"; shift 2;;
		--local-port) RDP_LOCAL_PORT="$2"; shift 2;;
		--client) RDP_CLIENT="$2"; shift 2;;
		--) shift; break;;
		-h|--help) sed -n '2,30p' "$0"; exit 0;;
		*) break;;
	esac
done

[ -n "$RDP_HOST" ] || { echo "error: RDP_HOST (or --host) is required" >&2; exit 2; }
if [ -z "$RDP_COOKIE" ] && [ -n "$RDP_COOKIE_FILE" ]; then
	RDP_COOKIE="$(tr -d '[:space:]' < "$RDP_COOKIE_FILE")"
fi
[ -n "$RDP_COOKIE" ] || { echo "error: RDP_COOKIE / --cookie / --cookie-file is required" >&2; exit 2; }

# Open the tunnel, backgrounded. ExitOnForwardFailure makes ssh fail fast if the
# local port can't bind -- which usually means a tunnel is already up, so reuse.
echo "tunnel: 127.0.0.1:$RDP_LOCAL_PORT -> $RDP_USER@$RDP_HOST:$RDP_PORT"
if ssh -f -N -L "$RDP_LOCAL_PORT:localhost:$RDP_PORT" \
	-i "$RDP_KEY" -o StrictHostKeyChecking=accept-new \
	-o ExitOnForwardFailure=yes "$RDP_USER@$RDP_HOST" 2>/tmp/rdp-ssh.err; then
	echo "  tunnel established (backgrounded)"
elif grep -qiE 'in use|cannot listen|already' /tmp/rdp-ssh.err; then
	echo "  local port busy -- reusing the tunnel already there"
else
	echo "ssh tunnel failed:" >&2; cat /tmp/rdp-ssh.err >&2; exit 1
fi

export HAIKU_REMOTE_FONT="$RDP_FONT"
export HAIKU_REMOTE_MONO_FONT="$RDP_MONO_FONT"
echo "launching: $RDP_CLIENT (setarch x86) -> 127.0.0.1:$RDP_LOCAL_PORT"
exec setarch x86 "$RDP_CLIENT" \
	--host 127.0.0.1 --port "$RDP_LOCAL_PORT" --cookie "$RDP_COOKIE" "$@"
