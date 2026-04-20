#!/usr/bin/env python3
"""Compare two P6 (binary RGB) PPM files from GTK vs SDL snapshot renderers.

Exits 0 if the images are close enough (GTK uses an internal FBO; SDL uses
default framebuffer — small GPU/driver differences are expected).

Usage: compare_projectm_snapshots.py <a.ppm> <b.ppm> [--mae-max F] [--bad-pct-max F]
"""
from __future__ import annotations

import argparse
import sys


def read_p6(path: str) -> tuple[int, int, bytes]:
    with open(path, "rb") as f:
        if f.readline() != b"P6\n":
            raise ValueError(f"{path}: expected P6 header")
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        parts = line.split()
        if len(parts) != 2:
            raise ValueError(f"{path}: bad dimension line {line!r}")
        w, h = int(parts[0]), int(parts[1])
        if f.readline() != b"255\n":
            raise ValueError(f"{path}: expected 255 maxval line")
        data = f.read(w * h * 3)
        if len(data) != w * h * 3:
            raise ValueError(f"{path}: short read {len(data)} != {w * h * 3}")
    return w, h, data


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--mae-max", type=float, default=14.0, help="Mean abs RGB error per channel")
    p.add_argument("--bad-pct-max", type=float, default=7.5, help="%% pixels with L1 RGB diff > 36")
    args = p.parse_args()

    wa, ha, da = read_p6(args.a)
    wb, hb, db = read_p6(args.b)

    def crop_top_left(rgb: bytes, w: int, h: int, cw: int, ch: int) -> bytes:
        rows = []
        for row in range(ch):
            start = row * w * 3
            rows.append(rgb[start : start + cw * 3])
        return b"".join(rows)

    cw, ch = min(wa, wb), min(ha, hb)
    if (wa, ha) != (wb, hb):
        print(
            f"  note: cropping to common {cw}x{ch} (was {wa}x{ha} vs {wb}x{hb})",
            file=sys.stderr,
        )
    da = crop_top_left(da, wa, ha, cw, ch)
    db = crop_top_left(db, wb, hb, cw, ch)

    n = cw * ch * 3
    total_abs = 0
    bad = 0
    for i in range(0, n, 3):
        d = abs(da[i] - db[i]) + abs(da[i + 1] - db[i + 1]) + abs(da[i + 2] - db[i + 2])
        total_abs += d
        if d > 36:
            bad += 1

    mae = total_abs / float(n)
    bad_pct = 100.0 * bad / float(cw * ch)

    print(f"compare_projectm_snapshots: {args.a} vs {args.b}")
    print(f"  compared {cw}x{ch}  MAE={mae:.3f}  strong-diff-pixels={bad_pct:.2f}%")

    if mae > args.mae_max or bad_pct > args.bad_pct_max:
        print(
            f"  FAIL thresholds mae_max={args.mae_max} bad_pct_max={args.bad_pct_max}",
            file=sys.stderr,
        )
        # Per-image channel stats help distinguish blank frames from genuine divergence.
        for label, raw in ((args.a, da), (args.b, db)):
            vals = raw
            vmin = min(vals)
            vmax = max(vals)
            vmean = sum(vals) / len(vals)
            flag = "  ALL-BLACK (wrong FBO?)" if vmax == 0 else ""
            print(f"  {label}: min={vmin} max={vmax} mean={vmean:.1f}{flag}",
                  file=sys.stderr)
        return 1
    print("  OK within thresholds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
