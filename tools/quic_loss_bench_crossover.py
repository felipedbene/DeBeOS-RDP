#!/usr/bin/env python3
"""Crossover and head-of-line-share-with-spread tables for #766 box 6 (run beside res-<workload>-<rtt>.jsonl)."""
import json,statistics as st,glob
cells={}
for f in glob.glob('res-*.jsonl'):
    w=f.split('-')[1]
    for l in open(f):
        d=json.loads(l); r=d['r']
        if r.get('ok'): cells.setdefault((w,d['rtt_ms'],d['loss_pct'],d['scheme']),[]).append(r)
L=(0,0.5,1,2,5)
def med(w,rtt,loss,s,f): return st.median([f(r) for r in cells[(w,rtt,loss,s)]])
metrics={
 'desktop echo p99 idle':('z',lambda r:r['echo_a']['p99'],'lower'),
 'desktop echo p99 w/ repaint':('z',lambda r:r['echo_b']['p99'],'lower'),
 'bulk echo p50 w/ repaint':('raw',lambda r:r['echo_b']['p50'],'lower'),
 'bulk echo p99 w/ repaint':('raw',lambda r:r['echo_b']['p99'],'lower'),
 'bulk delivered KB':('raw',lambda r:r['total_bytes']/1024,'higher'),
 'first frame ms (desktop)':('z',lambda r:r['first_frame_ms'],'lower'),
 'stall p99 (bulk, repaint gaps)':('raw',lambda r:r['repaint_gap']['p99'],'lower'),
}
print("| metric | +RTT | winner at 0 / 0.5 / 1 / 2 / 5 % loss (q=QUIC, w=wss, = within 10%) | crossover |")
print("|---|---|---|---|")
for name,(w,f,better) in metrics.items():
  for rtt in (0,20,80):
    marks=[]
    for loss in L:
        q=med(w,rtt,loss,'quic',f); ws=med(w,rtt,loss,'wss',f)
        if abs(q-ws)<=0.1*max(abs(q),abs(ws),1) or abs(q-ws)<3: marks.append('=')
        else: marks.append('q' if (q<ws)==(better=='lower') else 'w')
    strict=[m for m in marks if m!='=']
    if not strict: c='none: tied throughout'
    elif len(set(strict))==1: c=f"none in range: {'QUIC' if strict[0]=='q' else 'wss'} wins or ties every cell"
    else:
        i=next(k for k in range(1,5) if marks[k]!='=' and any(m!='=' and m!=marks[k] for m in marks[:k]))
        c=f"yes, between {L[i-1]} and {L[i]} %"
    print(f"| {name} | {rtt} | {' / '.join(marks)} | {c} |")
print()
print("HOL share p99 (ms) per run: run's (echo_B-echo_A) p99 minus median of the same at 0% loss; median (min-max) across 5 runs")
print("| workload | +RTT | loss % | QUIC | wss |"); print("|---|---|---|---|---|")
for w in ('z','raw'):
  for rtt in (0,20,80):
    for loss in L[1:]:
      out=[]
      for s in ('quic','wss'):
        base=st.median([r['echo_b']['p99']-r['echo_a']['p99'] for r in cells[(w,rtt,0,s)]])
        v=[r['echo_b']['p99']-r['echo_a']['p99']-base for r in cells[(w,rtt,loss,s)]]
        out.append(f"{st.median(v):.0f} ({min(v):.0f} to {max(v):.0f})")
      print(f"| {'desktop' if w=='z' else 'bulk'} | {rtt} | {loss} | {out[0]} | {out[1]} |")
