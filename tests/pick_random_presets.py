#!/usr/bin/env python3
"""List N pseudo-random .milk paths under a directory (one path per line).

Uses a seed for reproducible CI (MILKDROP_COMPARE_RANDOM_SEED or --seed).
"""
from __future__ import annotations

import argparse
import os
import random
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("preset_dir", help="Root directory to scan recursively for *.milk")
    ap.add_argument("-n", type=int, default=5, help="How many presets to pick")
    ap.add_argument(
        "--seed",
        type=int,
        default=None,
        help="RNG seed (default: env MILKDROP_COMPARE_RANDOM_SEED or 42)",
    )
    args = ap.parse_args()

    seed = args.seed
    if seed is None:
        seed = int(os.environ.get("MILKDROP_COMPARE_RANDOM_SEED", "42"))
    random.seed(seed)

    root = args.preset_dir
    if not os.path.isdir(root):
        print(f"pick_random_presets: not a directory: {root}", file=sys.stderr)
        return 1

    paths: list[str] = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.endswith(".milk"):
                paths.append(os.path.join(dirpath, name))

    if not paths:
        print(f"pick_random_presets: no .milk under {root}", file=sys.stderr)
        return 1

    paths.sort()
    random.shuffle(paths)
    n = min(args.n, len(paths))
    for p in paths[:n]:
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
