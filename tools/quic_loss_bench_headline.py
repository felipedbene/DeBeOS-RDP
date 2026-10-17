#!/usr/bin/env python3
"""One-table headline for #766 box 6: run in a directory of res-<workload>-<rtt>.jsonl files
(workload z = stock desktop with zstd, repaint every 1 s; raw = HAIKU_REMOTE_COMPRESSION=0,
repaint every 100 ms, ~276 KB/s offered -- a stand-in for Tier P bulk). HOL share p99 =
(echo_B - echo_A) p99 minus the same at 0 % loss, medians across runs."""
import json,statistics as st,glob,sys
cells={}
for f in glob.glob('res-*.jsonl'):
    w=f.split('-')[1]
    for l in open(f):
        d=json.loads(l); r=d['r']
        if not r.get('ok'): continue
        cells.setdefault((w,d['rtt_ms'],d['loss_pct'],d['scheme']),[]).append(r)
def m(w,rtt,loss,s,k1,k2=None):
    rs=cells.get((w,rtt,loss,s),[]); v=[r[k1][k2] if k2 else r[k1] for r in rs]
    return st.median(v) if v else float('nan')
print("| +RTT | loss % | desktop: echo p99 idle q / w | desktop: echo p99 w/ repaint q / w | desktop: HOL share p99 q / w | bulk: echo p50 w/ repaint q / w | bulk: echo p99 w/ repaint q / w | bulk: HOL share p99 q / w | bulk delivered KB q / w | first frame ms q / w |")
print("|---|---|---|---|---|---|---|---|---|---|")
for rtt in (0,20,80):
  for loss in (0,0.5,1,2,5):
    def hol(w,s):
        if loss==0: return '-'
        a=[r['echo_b']['p99']-r['echo_a']['p99'] for r in cells[(w,rtt,loss,s)]]
        b=[r['echo_b']['p99']-r['echo_a']['p99'] for r in cells[(w,rtt,0,s)]]
        return f"{st.median(a)-st.median(b):.0f}"
    q=lambda w,k1,k2=None:f"{m(w,rtt,loss,'quic',k1,k2):.0f} / {m(w,rtt,loss,'wss',k1,k2):.0f}"
    print(f"| {rtt} | {loss} | {q('z','echo_a','p99')} | {q('z','echo_b','p99')} | {hol('z','quic')} / {hol('z','wss')} | {q('raw','echo_b','p50')} | {q('raw','echo_b','p99')} | {hol('raw','quic')} / {hol('raw','wss')} | {m('raw',rtt,loss,'quic','total_bytes')/1024:.0f} / {m('raw',rtt,loss,'wss','total_bytes')/1024:.0f} | {q('z','first_frame_ms')} |")
n=sum(len(v) for v in cells.values()); print(f"\n{n} runs kept", file=sys.stderr)
