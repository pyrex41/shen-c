"""Mean of rounds per (workload, label) from a run.py JSON-lines log, as a markdown table."""
import json, sys, collections
rows = [json.loads(l) for l in open(sys.argv[1])]
order = sys.argv[2].split(",") if len(sys.argv) > 2 else []
agg = collections.defaultdict(list)
for r in rows:
    agg[(r["workload"], r["label"])].append(r)
print("| workload | build | mode | instr (G) | CPU s | peak RSS MB | payload ok | load |")
print("|---|---|---|---|---|---|---|---|")
for w in ("b-typed", "c-untyped", "d-g5"):
    labels = [l for l in order if (w, l) in agg] or sorted(l for (x, l) in agg if x == w)
    for l in labels:
        rs = agg[(w, l)]
        n = len(rs)
        ok = all(r["payload_ok"] for r in rs) and all(all(c == 0 for c in r["code"]) for r in rs)
        mode = "-" if l == "scheme" else rs[0].get("mode", "native") + ("" if rs[0].get("mode") == "interpret" else " (warm cache)")
        print(f"| {w} | {l} | {mode} | {sum(r['instr'] for r in rs)/n/1e9:.1f} | {sum(r['cpu'] for r in rs)/n:.1f} "
              f"| {max(r['rss'] for r in rs)/1e6:.0f} | {'yes' if ok else 'NO'} ({n} runs) | "
              f"{'/'.join(str(r['load_before']) for r in rs)} |")
