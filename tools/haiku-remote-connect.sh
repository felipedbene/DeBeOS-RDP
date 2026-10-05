#!/bin/sh
# haiku-remote-connect.sh
#
# Launch the DeBeOS-RDP client against a remote Haiku box over the best
# available route -- so the usual case is just:
#
#     tools/haiku-remote-connect.sh                 # one host in hosts.txt
#     tools/haiku-remote-connect.sh --host mybox    # pick one of several
#
# ROUTE (default: auto). The DIRECT route is the remote_broker: wss on
# :10902, which binds all interfaces and so is reachable straight at the host
# over a VPN -- fast, no tunnel. The native client speaks it directly; the
# script just ensures the broker is up, grabs its token, and fetches its
# self-signed cert to trust. app_server's own port (:10900) is loopback-only
# by design, so it is reached only through an SSH -L tunnel. Auto tries the
# broker first and falls back to the tunnel. Force with:
#     --direct | --broker     always use the broker (wss, no tunnel)
#     --tunnel | --ssh        always use the SSH tunnel to :10900
# --broker-port overrides :10902. In both routes the host/user/key come from
# the hosts file (below) unless overridden by flags.
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
# Route: auto tries the DIRECT route first -- the remote_broker (wss on
# RDP_BROKER_PORT), which binds all interfaces and is reachable straight at the
# host over a VPN -- and falls back to an SSH -L tunnel to app_server's
# loopback-only port only if the broker is unreachable. Force with
# --direct/--broker or --tunnel/--ssh.
RDP_ROUTE="${RDP_ROUTE:-auto}"
RDP_BROKER_PORT="${RDP_BROKER_PORT:-10902}"
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
		--direct|--broker|--wss) RDP_ROUTE="direct"; shift;;
		--tunnel|--ssh) RDP_ROUTE="tunnel"; shift;;
		--broker-port) RDP_BROKER_PORT="$2"; shift 2;;
		--client) RDP_CLIENT="$2"; shift 2;;
		--) shift; break;;
		-h|--help) sed -n '2,34p' "$0"; exit 0;;
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

# Is host:port reachable with a direct TCP connection (2s timeout)? Prefers nc,
# falls back to python3; if neither can probe, returns non-zero so auto mode
# falls through to the tunnel rather than guessing the port is open.
probe_tcp() { # host port -> 0 reachable, 1 not reachable/unknown
	if command -v nc >/dev/null 2>&1; then
		nc -z -w 2 "$1" "$2" >/dev/null 2>&1
	elif command -v python3 >/dev/null 2>&1; then
		python3 - "$1" "$2" <<-'PY'
		import socket, sys
		try:
		    socket.create_connection((sys.argv[1], int(sys.argv[2])), 2).close()
		except Exception:
		    sys.exit(1)
		PY
	else
		return 1
	fi
}

# Pick the route. The DIRECT route is the remote_broker (wss on $RDP_BROKER_PORT),
# which binds all interfaces and so is reachable straight at the host over the
# VPN -- that is the fast, tunnel-free path. app_server's own port ($RDP_PORT) is
# loopback-only by design (the broker is the front door), so it is reachable only
# through an SSH -L tunnel. AUTO tries the broker first and falls back to the
# tunnel; --direct/--broker and --tunnel/--ssh force the choice.
route="$RDP_ROUTE"
if [ "$route" = auto ]; then
	if probe_tcp "$RDP_HOST" "$RDP_BROKER_PORT"; then
		echo "direct route: broker reachable at $RDP_HOST:$RDP_BROKER_PORT -- no tunnel"
		route=direct
	else
		echo "broker not reachable at $RDP_HOST:$RDP_BROKER_PORT -- falling back to SSH tunnel"
		route=tunnel
	fi
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

if [ "$route" = direct ]; then
	# Broker (wss) transport -- the native client speaks it directly. Ensure the
	# broker is up on the host, read its auth token, and fetch its self-signed
	# certificate so the client can trust it (--ca-file beats pinning guesswork).
	# The broker presents app_server's session cookie itself, so none is needed
	# here. No SSH -L tunnel: wss goes straight to the host over the VPN.
	echo "broker route: ensuring remote_broker on $RDP_USER@$RDP_HOST ..."
	token="$($SSH_BASE "$RDP_USER@$RDP_HOST" '
		if ! ps 2>/dev/null | grep -q "[r]emote_broker"; then
			nohup /system/servers/remote_broker >/tmp/remote_broker.log 2>&1 &
			sleep 2
		fi
		cat /boot/system/settings/remote_desktop/token 2>/dev/null
	' 2>/dev/null | tr -d '[:space:]' || true)"
	[ -n "$token" ] || { echo "error: no broker token -- is remote_broker present (openssl build) and app_server up?" >&2; exit 1; }

	cafile="$(mktemp "${TMPDIR:-/tmp}/rdp-broker-ca.XXXXXX")"
	$SSH_BASE "$RDP_USER@$RDP_HOST" \
		'cat /boot/system/settings/remote_desktop/broker.pem 2>/dev/null' \
		> "$cafile" 2>/dev/null || true
	tls_args=""
	if [ -s "$cafile" ]; then
		tls_args="--ca-file $cafile"
	else
		echo "  warning: could not fetch broker.pem; proceeding without a pinned CA" >&2
		rm -f "$cafile"
	fi

	url="wss://$RDP_HOST:$RDP_BROKER_PORT"
	echo "launching: $RDP_CLIENT${SETARCH:+ ($SETARCH)} -> $url"
	exec $SETARCH "$RDP_CLIENT" --url "$url" --token "$token" $tls_args "$@"
else
	# Tunnel route: app_server's port is loopback-only, so forward it over SSH and
	# present the session cookie as the first frame.
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

	echo "launching: $RDP_CLIENT${SETARCH:+ ($SETARCH)} -> 127.0.0.1:$RDP_LOCAL_PORT"
	exec $SETARCH "$RDP_CLIENT" \
		--host 127.0.0.1 --port "$RDP_LOCAL_PORT" --cookie "$RDP_COOKIE" "$@"
fi
