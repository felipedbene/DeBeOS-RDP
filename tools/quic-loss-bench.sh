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
#       [--if ens5] [-- extra quic_loss_bench arguments]
#
# Each cell (added RTT x loss) runs --runs pairs, alternating which transport
# goes first so drift is not charged to one of them. The added RTT is split
# half each way; the loss is applied in BOTH directions at the given rate.
# netem's queue limit is raised so the delay line never drops on its own.
# Output: one JSON object per run, the bench's own fields plus cell metadata.
# Summarise with tools/quic_loss_bench_report.py.
set -euo pipefail

BROKER= CREDS= PORT=10902 LOSSES="0 0.5 1 2 5" RTTS="0 20 80" RUNS=5
OUT=quic-loss-bench.jsonl IF=ens5 BENCH="$(dirname "$0")/../build/quic_loss_bench"
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
trap netem_off EXIT HUP INT TERM

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
    printf '{"scheme":"%s","rtt_ms":%s,"loss_pct":%s,"run":%s,"at":"%s","r":%s}\n' \
        "$1" "$2" "$3" "$4" "$(date -u +%FT%TZ)" "$line" >> "$OUT"
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
