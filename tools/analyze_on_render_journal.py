#!/usr/bin/env python3
"""Parse journal lines with milkdrop on_render; print stats + histogram."""
import re
import statistics
import sys
from datetime import datetime

pat_ts = re.compile(r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})([+-]\d{2}:\d{2}|Z)")
pat_calls = re.compile(r"calls=(\d+)")
pat_hms = re.compile(r"milkdrop-Message:\s+(\d{2}:\d{2}:\d{2}\.\d+):")

rows = []
for line in sys.stdin:
    m = pat_ts.match(line)
    if not m:
        continue
    ts_s = m.group(1)
    off = m.group(2)
    if off == "Z":
        off = "+00:00"
    t2 = pat_hms.search(line)
    if not t2:
        continue
    hms = t2.group(1)
    mc = pat_calls.search(line)
    if not mc:
        continue
    calls = int(mc.group(1))
    datepart = ts_s[:10]
    full = f"{datepart}T{hms}{off}"
    try:
        dt = datetime.fromisoformat(full)
    except ValueError:
        continue
    rows.append((dt, calls))

rows.sort(key=lambda x: x[0])
seen = set()
clean = []
for dt, calls in rows:
    k = (dt.isoformat(), calls)
    if k in seen:
        continue
    seen.add(k)
    clean.append((dt, calls))

if len(clean) < 2:
    print("Pontos insuficientes:", len(clean), file=sys.stderr)
    sys.exit(0)

deltas = []
fps_blocks = []
for i in range(1, len(clean)):
    dt0, c0 = clean[i - 1]
    dt1, c1 = clean[i]
    dc = c1 - c0
    if dc <= 0:
        continue
    dt_sec = (dt1 - dt0).total_seconds()
    if dt_sec <= 0:
        continue
    implied_fps = dc / dt_sec
    deltas.append(dt_sec)
    fps_blocks.append(implied_fps)

print("=== Baseline on_render (stdin journal lines) ===")
print(f"Intervalos válidos entre logs consecutivos: {len(deltas)}")
print(
    f"Δt (s): min={min(deltas):.3f}  max={max(deltas):.3f}  "
    f"média={statistics.mean(deltas):.3f}  mediana={statistics.median(deltas):.3f}"
)
if len(deltas) > 1:
    print(f"  desvio padrão: {statistics.stdev(deltas):.3f}")
print(
    f"FPS implícito (Δframes/Δt): min={min(fps_blocks):.1f}  max={max(fps_blocks):.1f}  "
    f"média={statistics.mean(fps_blocks):.1f}  mediana={statistics.median(fps_blocks):.1f}"
)

labels = ["≤1s", "1-1.5", "1.5-2", "2-2.5", "2.5-3", "3-4", "4-5", "5-10", ">10s"]
counts2 = [0] * len(labels)
for d in deltas:
    if d <= 1:
        counts2[0] += 1
    elif d <= 1.5:
        counts2[1] += 1
    elif d <= 2:
        counts2[2] += 1
    elif d <= 2.5:
        counts2[3] += 1
    elif d <= 3:
        counts2[4] += 1
    elif d <= 4:
        counts2[5] += 1
    elif d <= 5:
        counts2[6] += 1
    elif d <= 10:
        counts2[7] += 1
    else:
        counts2[8] += 1

print("\nHistograma de Δt entre logs consecutivos:")
total = sum(counts2)
for lab, c in zip(labels, counts2):
    pct = 100.0 * c / total if total else 0
    bar = "█" * min(50, int(pct))
    print(f"  {lab:8s} {c:5d} ({pct:5.1f}%) {bar}")
print("\nReferência: Δt≈1.0s com +60 calls ≈ 60 fps nesse bloco; Δt≈2s ≈ 30 fps.")
