#!/usr/bin/env bash
#
# validate-live -- take the client from a stopped EC2 instance to a rendered
# frame of a real Haiku desktop, in one command.
#
# This exists because the live path has six separate traps, each of which
# produces a timeout or a refusal that looks exactly like the other five:
#
#   1. The public IP changes on every stop/start (auto-assigned, not Elastic).
#   2. `curl checkip.amazonaws.com` reports the proxy's address, not the one the
#      open internet sees for raw TCP. portquiz.net echoes the real one.
#   3. No external probe can tell you what address AWS sees. Measured on one
#      machine at one moment: checkip said 52.94.133.140, portquiz:22 said
#      52.46.80.16, and the instance's own $SSH_CLIENT said 205.251.233.176.
#      Corporate egress NATs per destination, so source-IP rules are unfixable
#      guesswork. This script therefore does not use them: it goes through an
#      EC2 Instance Connect Endpoint, and the security group allows port 22 from
#      the endpoint's security group only.
#   4. The images accept ed25519 keys only (OpenSSH built --without-openssl),
#      so haiku-graviton.pem silently fails.
#   5. app_server binds 127.0.0.1 deliberately, so only `ssh -L` reaches it.
#   6. Reaching the port is not enough: the first frame of a direct connection
#      must be app_server's per-boot session cookie, or the connection is closed
#      with no reply at all. This script reads the cookie off the guest over the
#      same ssh it already has, so a refusal cannot masquerade as a dead server.
#
# Nothing needs starting inside the guest: launch_daemon runs remote-desktop.sh
# at boot (see graviton/ssh/files/remote-desktop.sh), which exports
# TARGET_SCREEN=10900 and brings up the remote Desktop with input_server,
# Tracker and Deskbar.
#
# Usage:
#   ./validate-live.sh                        # start, tunnel, probe, capture
#   ./validate-live.sh --stop-when-done       # stop the instance afterwards
#   ./validate-live.sh --gui                  # leave the tunnel up for the app
#   ./validate-live.sh --direct               # force the public-IP path
#
#   HAIKU_RD_PORT=10900                       # app_server's listener, on the guest
#   HAIKU_RD_LOCAL_PORT=19900                 # this end of the forward, if busy
#
# This script never modifies a security group. If port 22 is unreachable it
# explains how to find the address AWS actually sees, and how to avoid needing
# one at all.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

INSTANCE="${HAIKU_INSTANCE:-i-09eee41853815abfd}"
# EC2 Instance Connect Endpoint. Needs no agent on the guest, which is why it
# works for Haiku where SSM does not, and it removes any dependency on this
# machine's public address.
EICE_LOCAL_PORT="${HAIKU_EICE_PORT:-2222}"
REGION="${AWS_REGION:-us-west-2}"
# Only ever the security group this project created. Never the user's
# haiku-graviton-ssh.
SG="${HAIKU_SG:-sg-0974fc094328104fa}"
KEY="${HAIKU_RD_KEY:-$HOME/.ssh/haiku-rdclient-ed25519}"
USER_NAME="${HAIKU_RD_USER:-baron}"
# Two ports, deliberately separate. PORT is app_server's on the guest: it is
# what the forward's far end addresses and, because app_server names its cookie
# file after its own listener, it is also the number in that filename. LOCAL_PORT
# is this machine's end of the forward and is free to be anything. They defaulted
# to one variable, so the advice printed when the local port was busy -- "set
# HAIKU_RD_PORT" -- silently moved the far end of the tunnel and the cookie path
# too, and the run then failed against a listener that was never there.
PORT="${HAIKU_RD_PORT:-10900}"
LOCAL_PORT="${HAIKU_RD_LOCAL_PORT:-$PORT}"
WIDTH="${HAIKU_RD_WIDTH:-1280}"
HEIGHT="${HAIKU_RD_HEIGHT:-800}"
OUT="${HAIKU_RD_OUT:-/tmp/haiku-live.png}"

STOP_AFTER=0
GUI=0
# Prefer the Instance Connect Endpoint when one exists; --direct forces the
# public-IP path.
DIRECT=0
while [ $# -gt 0 ]; do
	case "$1" in
		--stop-when-done)  STOP_AFTER=1; shift ;;
		--gui)             GUI=1; shift ;;
		--direct)          DIRECT=1; shift ;;
		--instance)        INSTANCE="$2"; shift 2 ;;
		--out)             OUT="$2"; shift 2 ;;
		# The whole header block, usage list included -- the old range stopped
		# short of it, so --help listed none of the options it accepts.
		-h|--help)         sed -n '2,40p' "$0"; exit 0 ;;
		*)                 echo "unknown option $1" >&2; exit 2 ;;
	esac
done

say() { printf '\n== %s\n' "$*"; }
die() { echo "validate-live: $*" >&2; exit 1; }

[ -f "$KEY" ] || die "no ed25519 key at $KEY (the images reject RSA keys)"

TUNNEL_PID=""
EICE_PID=""
COOKIE_FILE=""
cleanup() {
	# The cookie is a live secret for as long as the guest's listener is, so it
	# does not outlive this run on disk here either.
	if [ -n "$COOKIE_FILE" ] && [ -f "$COOKIE_FILE" ]; then
		rm -f "$COOKIE_FILE"
	fi
	for pid_var in TUNNEL_PID EICE_PID; do
		eval "pid=\$$pid_var"
		if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
			echo "  tearing down $pid_var (pid $pid)"
			kill "$pid" 2>/dev/null || true
			wait "$pid" 2>/dev/null || true
		fi
	done
}
trap cleanup EXIT INT TERM

# -- 1. start the instance ---------------------------------------------------
say "starting $INSTANCE"
state="$(aws ec2 describe-instances --region "$REGION" --instance-ids "$INSTANCE" \
	--query 'Reservations[].Instances[].State.Name' --output text)"
echo "  current state: $state"
if [ "$state" != "running" ]; then
	aws ec2 start-instances --region "$REGION" --instance-ids "$INSTANCE" \
		--query 'StartingInstances[].CurrentState.Name' --output text
	aws ec2 wait instance-running --region "$REGION" --instance-ids "$INSTANCE"
fi

# The address is allocated at start, so it must be read now and never cached.
IP="$(aws ec2 describe-instances --region "$REGION" --instance-ids "$INSTANCE" \
	--query 'Reservations[].Instances[].PublicIpAddress' --output text)"
[ -n "$IP" ] && [ "$IP" != "None" ] || die "no public IP on $INSTANCE"
echo "  public IP: $IP"

# -- 2/3. how do we reach port 22? ------------------------------------------
# There is deliberately no "work out my public IP and authorize it" step. It
# cannot be done correctly: corporate egress NATs per destination, so every
# external probe gives a different answer and none is the one AWS sees. Measured
# on one machine within one minute:
#
#     checkip.amazonaws.com   52.94.133.140
#     portquiz.net:22         52.46.80.16
#     the instance itself     205.251.233.176   <- the only one that matters
#
# An EC2 Instance Connect Endpoint removes the question entirely: traffic is
# sourced from the endpoint's ENI inside the VPC, so the security group allows
# port 22 from the endpoint's security group and no client address appears
# anywhere. It needs no agent on the guest, which is why it works for Haiku where
# SSM does not.
SSH_HOST="$IP"
SSH_PORT=22
SSH_EXTRA=""

EICE_ID=""
if [ "$DIRECT" = 0 ]; then
	VPC="$(aws ec2 describe-instances --region "$REGION" --instance-ids "$INSTANCE" \
		--query 'Reservations[].Instances[].VpcId' --output text)"
	EICE_ID="$(aws ec2 describe-instance-connect-endpoints --region "$REGION" \
		--filters "Name=vpc-id,Values=$VPC" \
		--query 'InstanceConnectEndpoints[?State==`create-complete`].InstanceConnectEndpointId | [0]' \
		--output text 2>/dev/null)"
	[ "$EICE_ID" = "None" ] && EICE_ID=""
fi

if [ -n "$EICE_ID" ]; then
	say "using Instance Connect Endpoint $EICE_ID (no source-IP rule needed)"
	if lsof -nP -iTCP:"$EICE_LOCAL_PORT" -sTCP:LISTEN >/dev/null 2>&1; then
		die "local port $EICE_LOCAL_PORT is busy; set HAIKU_EICE_PORT to a free one"
	fi
	aws ec2-instance-connect open-tunnel --region "$REGION" \
		--instance-id "$INSTANCE" --local-port "$EICE_LOCAL_PORT" \
		> /tmp/eice-tunnel.$$.log 2>&1 &
	EICE_PID=$!
	for _ in $(seq 1 25); do
		kill -0 "$EICE_PID" 2>/dev/null || { cat "/tmp/eice-tunnel.$$.log" >&2; die "open-tunnel exited"; }
		nc -z 127.0.0.1 "$EICE_LOCAL_PORT" 2>/dev/null && break
		sleep 1
	done
	nc -z 127.0.0.1 "$EICE_LOCAL_PORT" 2>/dev/null || die "endpoint tunnel never came up"
	echo "  endpoint tunnel live on 127.0.0.1:$EICE_LOCAL_PORT"
	SSH_HOST="127.0.0.1"
	SSH_PORT="$EICE_LOCAL_PORT"
	# Every instance appears as 127.0.0.1:<port> through the endpoint, so a
	# recorded host key would collide with the next instance and ssh would refuse
	# on a key mismatch. Skip the known_hosts entry rather than poison it; the
	# endpoint and the key pair are what provide the assurance here.
	SSH_EXTRA="-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o LogLevel=ERROR"
else
	say "checking TCP 22 on $IP"
	if nc -z -G 10 "$IP" 22 2>/dev/null; then
		echo "  reachable"
	else
		echo "  port 22 did not answer." >&2
		echo "  The security group may not admit the address AWS actually sees" >&2
		echo "  for you. Ask an instance you CAN reach:" >&2
		echo "      ssh -i $KEY $USER_NAME@<reachable-ip> 'echo \$SSH_CLIENT'" >&2
		echo "  then authorize its /24 (the low octet drifts). Better, create an" >&2
		echo "  Instance Connect Endpoint and drop source IPs entirely:" >&2
		echo "      aws ec2 create-instance-connect-endpoint --region $REGION \\" >&2
		echo "        --subnet-id <subnet> --security-group-ids <sg> --no-preserve-client-ip" >&2
		die "port 22 unreachable on $IP"
	fi
fi

# -- 4. tunnel --------------------------------------------------------------
# No ControlMaster: a shared connection outlives this script and the next run
# silently reuses a dead one. -N because we only want the forward.
# A pre-existing listener on the local port is the nastiest failure here: our own
# ssh would fail to bind and exit, but the readiness check below (`nc -z`) would
# still succeed against the *other* process, so the probe and the capture would
# silently run against whatever host that tunnel points at. Observed exactly
# that: a leftover tunnel to a different instance produced a plausible frame from
# the wrong machine. Refuse to start instead.
say "checking local port $LOCAL_PORT is free"
if lsof -nP -iTCP:"$LOCAL_PORT" -sTCP:LISTEN >/dev/null 2>&1; then
	echo "  something is already listening on $LOCAL_PORT:" >&2
	lsof -nP -iTCP:"$LOCAL_PORT" -sTCP:LISTEN 2>/dev/null | tail -n +2 | sed 's/^/    /' >&2
	die "refusing to run: a frame captured through that tunnel would come from whatever host it points at, not $INSTANCE. Kill it, or set HAIKU_RD_LOCAL_PORT to a free local port (which leaves the guest's listener, and its cookie file, on $PORT)."
fi
echo "  free"

say "opening ssh -L $LOCAL_PORT:127.0.0.1:$PORT via $SSH_HOST:$SSH_PORT"
# shellcheck disable=SC2086  # SSH_EXTRA is deliberately word-split
ssh -N -T \
	-o ExitOnForwardFailure=yes \
	-o StrictHostKeyChecking=accept-new \
	-o ConnectTimeout=15 \
	-o ControlMaster=no \
	-o ControlPath=none \
	$SSH_EXTRA \
	-p "$SSH_PORT" \
	-i "$KEY" \
	-L "$LOCAL_PORT:127.0.0.1:$PORT" \
	"$USER_NAME@$SSH_HOST" &
TUNNEL_PID=$!

ready=0
for _ in $(seq 1 30); do
	if ! kill -0 "$TUNNEL_PID" 2>/dev/null; then
		die "ssh exited — check the key, the user, and the security group"
	fi
	if nc -z 127.0.0.1 "$LOCAL_PORT" 2>/dev/null; then ready=1; break; fi
	sleep 1
done
[ "$ready" = 1 ] \
	|| die "forward never came up; if ssh authenticated, app_server may not be listening on $PORT on the guest"
# Re-check afterwards: the port being open proves *someone* is listening, not that
# it is us. Since we verified the port was free before starting, our ssh still
# being alive is what makes it ours.
kill -0 "$TUNNEL_PID" 2>/dev/null \
	|| die "our ssh died just after the forward appeared — another process owns $LOCAL_PORT"
echo "  forward is live on 127.0.0.1:$LOCAL_PORT -> guest 127.0.0.1:$PORT (pid $TUNNEL_PID)"

# -- 5. the session cookie --------------------------------------------------
# app_server mints a per-boot cookie before it binds and publishes it in an
# owner-only file; presenting it is the first frame of any direct connection.
# Fetched over the ssh we already have, because the file is mode 0600 on the
# guest and the cookie changes on every boot -- a cached one is worse than none,
# since a wrong cookie is refused exactly like a missing one.
# The filename carries app_server's OWN listener port, so it is $PORT here and
# never $LOCAL_PORT. Getting that backwards is what issue #20 was about.
say "fetching the session cookie for the guest's listener port $PORT"
COOKIE_PATH="/boot/system/settings/remote_desktop/session_cookie.$PORT"
COOKIE_FILE="$(umask 077 && mktemp -t haiku-session-cookie.XXXXXX)"
# shellcheck disable=SC2086  # SSH_EXTRA is deliberately word-split
if ! ssh -T \
	-o StrictHostKeyChecking=accept-new \
	-o ConnectTimeout=15 \
	-o ControlMaster=no \
	-o ControlPath=none \
	$SSH_EXTRA \
	-p "$SSH_PORT" \
	-i "$KEY" \
	"$USER_NAME@$SSH_HOST" "cat $COOKIE_PATH" > "$COOKIE_FILE" 2>/dev/null \
	|| [ ! -s "$COOKIE_FILE" ]; then
	echo "  could not read $COOKIE_PATH on the guest." >&2
	echo "  It is created by app_server before it starts listening, mode 0600," >&2
	echo "  owned by the user app_server runs as. If it is absent, the remote" >&2
	echo "  Desktop is not up; if it is unreadable, $USER_NAME is not that user." >&2
	die "no session cookie — a direct connection cannot be opened without it"
fi
echo "  read $(wc -c < "$COOKIE_FILE" | tr -d ' ') bytes into $COOKIE_FILE"

# -- 6. protocol probe ------------------------------------------------------
# Pure Python, no client involved: separates "the protocol works" from "the
# client renders it", so a failure here is unambiguous.
say "probing the protocol (rp_probe.py)"
python3 tools/rp_probe.py --port "$LOCAL_PORT" --width "$WIDTH" --height "$HEIGHT" \
	--cookie-file "$COOKIE_FILE" --seconds 5 \
	|| die "probe failed — either the protocol did not come up or the session cookie was refused (the gate closes without replying, so the two look alike from the client side)"

# -- 7. the client ----------------------------------------------------------
# The C++ client in CrossPlatform/. `haiku-remote` is the headless capture
# binary and writes --output; -gui (SDL2) and -x11 are the interactive ones.
say "rendering with the real client"
[ -x CrossPlatform/build/haiku-remote ] || make -C CrossPlatform build/haiku-remote
# The client's exit codes are distinct on purpose (see its --help): 3 means it
# was never given a cookie, which would be this script's bug and not the
# server's, so name it rather than letting `set -e` report a bare 3.
client_status=0
CrossPlatform/build/haiku-remote --output "$OUT" --port "$LOCAL_PORT" \
	--cookie-file "$COOKIE_FILE" \
	--width "$WIDTH" --height "$HEIGHT" --seconds 6 || client_status=$?
case "$client_status" in
	0) ;;
	3) die "the client refused to connect for lack of a session cookie, yet one was fetched into $COOKIE_FILE — that is a bug in this script, not in the guest" ;;
	*) die "the client failed (exit $client_status): the session was refused or died on the wire; a stale cookie exits 1 this way, and the guest's syslog says which" ;;
esac

say "wrote $OUT"
echo "  open it and compare against the browser demo:"
echo "    graviton/scripts/haiku-remote-desktop --key $KEY $IP"

if [ "$GUI" = 1 ]; then
	say "tunnel held open for the interactive client"
	echo "  make -C CrossPlatform interactive    # or: make -C CrossPlatform all"
	echo "  CrossPlatform/build/haiku-remote-gui --port $LOCAL_PORT \\"
	echo "    --cookie-file $COOKIE_FILE --width $WIDTH --height $HEIGHT"
	echo "  (the cookie file is removed when this script exits, and the cookie"
	echo "   itself is only valid until the guest reboots)"
	echo "  Ctrl-C when finished."
	wait "$TUNNEL_PID" || true
fi

cleanup
TUNNEL_PID=""

if [ "$STOP_AFTER" = 1 ]; then
	say "stopping $INSTANCE"
	aws ec2 stop-instances --region "$REGION" --instance-ids "$INSTANCE" \
		--query 'StoppingInstances[].CurrentState.Name' --output text
else
	echo
	echo "instance left running — stop it when done:"
	echo "  aws ec2 stop-instances --region $REGION --instance-ids $INSTANCE"
fi
