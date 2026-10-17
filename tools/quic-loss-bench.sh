#!/bin/bash
# quic-loss-bench.sh -- QUIC vs wss under tc netem loss (Haiku-Graviton #766, box 6).
#
# Runs build/quic_loss_bench against the broker over quic:// and wss://, under
# netem on THIS host only (egress on $IF; ingress through ifb0), matched to the
# broker's addresses only, and removes every qdisc it added on exit -- including
# on error or Ctrl-C. Run it on a Linux client host (needs sudo for tc).
#
#   tools/quic-loss-bench.sh --broker HOST --creds FILE [--port 10902]
#       [--loss "0 0.5 1 2 5"] [--rtt "0 20 80"] [--runs 5] [--out results.jsonl]
#       [--if ens5] [--tcp-mss 1448] [--gro keep|off]
#       [-- extra quic_loss_bench arguments]
#
# Each cell (added RTT x loss) runs --runs pairs, alternating which transport
# goes first so drift is not charged to one of them. The added RTT is split
# half each way; the loss is applied in BOTH directions at the given rate.
# netem's queue limit is raised so the delay line never drops on its own.
#
# Packet size is part of the loss model. netem drops *packets*, and a Reno-style
# sender's fair rate is MSS/RTT * 1.22/sqrt(p), so at the same p a transport
# with 7x larger packets is allowed 7x the rate. Inside a VPC the MTU is 9001
# and DeBeOS's TCP negotiates MSS 8365 (8353 payload with timestamps) through
# the NLB, while QUIC stays at its 1200-byte datagrams: un-clamped, wss got ~7x
# QUIC's loss budget, which was the whole "wss beats the TCP-fair rate" result
# of Haiku-Graviton #826. So by default the bench clamps the MSS this host
# advertises to the broker's addresses to an internet-path 1448 (a per-host
# route with advmss), which the server's TCP then honours. --tcp-mss 0 leaves
# the path MSS alone (the old behaviour).
#
# GRO runs before the ingress hook, so with it on, netem drops coalesced TCP
# super-packets (up to 64 KB) whole -- one loss event for several segments --
# while QUIC datagrams are never coalesced (DeBeOS-RDP #105). On the ENA
# client that was a small effect next to the MSS (#826), but it is a bias in
# the same direction; --gro off (the default) turns GRO, LRO and UDP GRO
# forwarding off for the run, records their state in every result line, and
# restores what it changed on exit. --gro keep leaves them alone.
#
# Output: one JSON object per run, the bench's own fields plus cell metadata.
# Summarise with tools/quic_loss_bench_report.py.
set -euo pipefail

BROKER= CREDS= PORT=10902 LOSSES="0 0.5 1 2 5" RTTS="0 20 80" RUNS=5
OUT=quic-loss-bench.jsonl IF=ens5 TCP_MSS=1448 GRO=off BENCH="$(dirname "$0")/../build/quic_loss_bench"
EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
    --broker) BROKER=$2; shift 2 ;;
    --creds) CREDS=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --loss) LOSSES=$2; shift 2 ;;
    --rtt) RTTS=$2; shift 2 ;;
    --runs) RUNS=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --if) IF=$2; shift 2 ;;
    --tcp-mss) TCP_MSS=$2; shift 2 ;;
    --gro) GRO=$2; shift 2 ;;
    --bench) BENCH=$2; shift 2 ;;
    --) shift; EXTRA=("$@"); break ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$BROKER" ] && [ -n "$CREDS" ] || { echo "need --broker and --creds" >&2; exit 2; }
[ -x "$BENCH" ] || { echo "no bench binary at $BENCH (make build/quic_loss_bench)" >&2; exit 2; }

# The broker's addresses: an NLB name resolves to one per AZ, and the filter
# must catch whichever the client picks.
mapfile -t ADDRS < <(getent ahostsv4 "$BROKER" | awk '{print $1}' | sort -u)
[ ${#ADDRS[@]} -gt 0 ] || { echo "cannot resolve $BROKER" >&2; exit 2; }

netem_off() {
    sudo tc qdisc del dev "$IF" root 2>/dev/null || true
    sudo tc qdisc del dev "$IF" ingress 2>/dev/null || true
    sudo tc qdisc del dev ifb0 root 2>/dev/null || true
}

# Receive-side coalescing: GRO, LRO, and UDP GRO forwarding. Each is turned
# off for the run if it is on and not [fixed], and put back on exit.
offload() { # <ethtool -k feature name>
    ethtool -k "$IF" 2>/dev/null | awk -v f="$1:" '$1 == f {print $2 ($3 == "[fixed]" ? "-fixed" : "")}'
}
offloads() {
    printf 'gro=%s,lro=%s,rx-udp-gro-forwarding=%s' "$(offload generic-receive-offload)" \
        "$(offload large-receive-offload)" "$(offload rx-udp-gro-forwarding)"
}
RESTORE=()
ROUTES=()
cleanup() {
    netem_off
    local a
    for a in "${ROUTES[@]}"; do sudo ip route del "$a"/32 dev "$IF" 2>/dev/null || true; done
    [ ${#RESTORE[@]} -eq 0 ] || sudo ethtool -K "$IF" "${RESTORE[@]}" || true
}
# A signal must end the run, not just clean up: with the handler on the
# signals themselves, bash ran it and carried on into the next run with netem
# and the offloads already restored -- unshaped runs filed as lossy ones.
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

if [ "$GRO" = off ]; then
    for f in generic-receive-offload:gro large-receive-offload:lro \
             rx-udp-gro-forwarding:rx-udp-gro-forwarding; do
        if [ "$(offload "${f%%:*}")" = on ]; then
            sudo ethtool -K "$IF" "${f#*:}" off
            RESTORE+=("${f#*:}" on)
        fi
    done
fi
OFFLOADS=$(offloads)
echo "receive offloads for the run: $OFFLOADS" >&2
if [ "$TCP_MSS" != 0 ]; then
    for a in "${ADDRS[@]}"; do
        # Only add (and later delete) a route we created: never touch one that
        # was already there.
        if ip route show "$a"/32 dev "$IF" | grep -q .; then
            echo "a route for $a already exists; not clamping its MSS" >&2
            continue
        fi
        sudo ip route add "$a"/32 dev "$IF" advmss "$TCP_MSS"
        ROUTES+=("$a")
    done
fi

netem_on() { # <one-way delay ms> <loss %>
    netem_off
    [ "$1" = 0 ] && [ "$2" = 0 ] && return 0
    sudo modprobe ifb numifbs=1
    ip link show ifb0 >/dev/null 2>&1 || sudo ip link add ifb0 type ifb
    sudo ip link set dev ifb0 up
    local spec=(netem limit 100000 delay "${1}ms" loss "${2}%")
    sudo tc qdisc add dev "$IF" root handle 1: prio
    sudo tc qdisc add dev "$IF" parent 1:3 handle 30: "${spec[@]}"
    sudo tc qdisc add dev "$IF" handle ffff: ingress
    sudo tc qdisc add dev ifb0 root "${spec[@]}"
    local a
    for a in "${ADDRS[@]}"; do
        sudo tc filter add dev "$IF" protocol ip parent 1:0 prio 3 u32 \
            match ip dst "$a"/32 flowid 1:3
        sudo tc filter add dev "$IF" parent ffff: protocol ip u32 \
            match ip src "$a"/32 action mirred egress redirect dev ifb0
    done
}

one() { # <scheme> <rtt> <loss> <run>
    local url
    case "$1" in
    quic) url="quic://$BROKER:$PORT" ;;
    wss) url="wss://$BROKER:$PORT" ;;
    esac
    local line
    line=$("$BENCH" --url "$url" --creds "$CREDS" --label "$1" "${EXTRA[@]}" \
           2>/dev/null | tail -1) || true
    [ -n "$line" ] || line='{"ok":false,"error":"no output"}'
    printf '{"scheme":"%s","rtt_ms":%s,"loss_pct":%s,"run":%s,"tcp_mss":%s,"offloads":"%s","at":"%s","r":%s}\n' \
        "$1" "$2" "$3" "$4" "$TCP_MSS" "$OFFLOADS" "$(date -u +%FT%TZ)" "$line" >> "$OUT"
    echo "$1 rtt=$2 loss=$3 run=$4: $(echo "$line" | cut -c1-160)" >&2
}

for rtt in $RTTS; do
    for loss in $LOSSES; do
        netem_on "$(awk -v r="$rtt" 'BEGIN{printf "%g", r/2}')" "$loss"
        for run in $(seq 1 "$RUNS"); do
            if [ $((run % 2)) = 1 ]; then
                one quic "$rtt" "$loss" "$run"; one wss "$rtt" "$loss" "$run"
            else
                one wss "$rtt" "$loss" "$run"; one quic "$rtt" "$loss" "$run"
            fi
        done
        netem_off
    done
done
