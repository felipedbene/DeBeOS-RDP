#!/usr/bin/env python3
"""Summarise tools/quic-loss-bench.sh output (JSON lines) into markdown tables.

    tools/quic_loss_bench_report.py results.jsonl [more.jsonl ...]

Per cell (added RTT x loss x transport): the median across runs of each run's
statistic, with the min..max across runs as the spread. Runs that ended early
(evicted, closed, failed to connect) are counted and excluded.

The head-of-line estimate, per cell and percentile p:

    hol(p) = [echo_B(p) - echo_A(p)]  -  [echo_B(p) - echo_A(p)] at 0 % loss

echo_A is input-to-echo with nothing else on the stream -- what the cursor
would see on a stream of its own, waiting only for its own losses. echo_B is
the same probes while full repaints share the one stream. Their difference at
0 % loss is plain queueing behind the repaint bytes (bandwidth, not loss; a
second stream does not remove it without prioritisation), so what remains is
the stall a loss in *unrelated* content imposes on the cursor: the share that a
multi-stream layout (debeos-urp/2) would recover.
"""
import json
import statistics
import sys
from collections import defaultdict


def load(paths):
    rows = []
    for path in paths:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line:
                    rows.append(json.loads(line))
    return rows


def med(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def fmt(values, digits=0):
    values = [v for v in values if v is not None]
    if not values:
        return "-"
    m = statistics.median(values)
    return f"{m:.{digits}f} ({min(values):.{digits}f}-{max(values):.{digits}f})"


def main():
    rows = load(sys.argv[1:])
    cells = defaultdict(list)
    bad = defaultdict(int)
    for row in rows:
        key = (row["rtt_ms"], row["loss_pct"], row["scheme"])
        r = row["r"]
        if not r.get("ok") or r.get("repaints_done", 0) == 0:
            bad[key] += 1
            continue
        cells[key].append(r)
    keys = sorted(set(cells) | set(bad))

    def g(rs, path):
        out = []
        for r in rs:
            v = r
            for p in path.split("."):
                v = v.get(p) if isinstance(v, dict) else None
            out.append(v)
        return out

    print("| +RTT ms | loss % | transport | runs ok/bad | first frame ms | "
          "repaint ms p50 | stall p50 / p95 / p99 / max ms | "
          "echo idle p50 / p99 ms | echo under repaint p50 / p95 / p99 ms | "
          "msgs/s | KB total |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for key in keys:
        rs = cells.get(key, [])
        rtt, loss, scheme = key
        stall = " / ".join(fmt(g(rs, f"repaint_gap.{p}")) for p in
                           ("p50", "p95", "p99", "max"))
        echo_a = " / ".join(fmt(g(rs, f"echo_a.{p}")) for p in ("p50", "p99"))
        echo_b = " / ".join(fmt(g(rs, f"echo_b.{p}")) for p in
                            ("p50", "p95", "p99"))
        kb = [t / 1024 for t in g(rs, "total_bytes") if t is not None]
        print(f"| {rtt} | {loss} | {scheme} | {len(rs)}/{bad.get(key, 0)} | "
              f"{fmt(g(rs, 'first_frame_ms'))} | {fmt(g(rs, 'repaint_ms.p50'))} | "
              f"{stall} | {echo_a} | {echo_b} | "
              f"{fmt(g(rs, 'bulk_messages_per_s'))} | {fmt(kb)} |")

    print()
    print("Head-of-line share (ms): cursor delay added by losses in unrelated "
          "repaint content = (echo_B - echo_A) - (echo_B - echo_A at 0 % loss), "
          "medians across runs.")
    print()
    print("| +RTT ms | loss % | transport | hol p50 | hol p95 | hol p99 |")
    print("|---|---|---|---|---|---|")
    for key in keys:
        rtt, loss, scheme = key
        rs = cells.get(key, [])
        base = cells.get((rtt, 0, scheme)) or cells.get((rtt, 0.0, scheme))
        if not rs or not base or loss in (0, 0.0):
            continue
        out = []
        for p in ("p50", "p95", "p99"):
            d = med([b - a for a, b in zip(g(rs, f"echo_a.{p}"),
                                           g(rs, f"echo_b.{p}"))
                     if a is not None and b is not None])
            d0 = med([b - a for a, b in zip(g(base, f"echo_a.{p}"),
                                            g(base, f"echo_b.{p}"))
                      if a is not None and b is not None])
            out.append("-" if d is None or d0 is None else f"{d - d0:.0f}")
        print(f"| {rtt} | {loss} | {scheme} | " + " | ".join(out) + " |")


if __name__ == "__main__":
    main()
