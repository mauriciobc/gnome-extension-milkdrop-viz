# GPU profile run (2026-04-12)

## Captured

- `00_header.txt` — snapshots + milkdrop PIDs
- `pmon.log` — `nvidia-smi pmon` (per-process; GeForce often shows `-` for SM/mem on milkdrop)
- `milkdrop_lines.txt` — grep milkdrop from pmon

## Findings (this machine)

- **GPU:** RTX 3060 Laptop, driver 595.x
- **Global snapshot:** ~29–34% `utilization.gpu`, ~134 MiB used, **clocks very low** in a follow-up `dmon` sample (~210 MHz graphics / 405 MHz memory) — suggests **power-saving P-state** or mixed workload, not sustained boost during the sample window.
- **milkdrop:** `nvidia-smi` lists **4 MiB** per process; **pmon does not show per-process SM%** for milkdrop (dashes) — **normal limitation on many GeForce consumer drivers**; use **device-level `dmon`** + external tools for attribution.
- **mutter-devkit** (nested session): **~32–37% SM** in pmon when present — useful as compositor load reference, not milkdrop-only.

## Next steps

- Re-run with updated `tools/profile_gpu_milkdrop.sh` (includes `dmon.log`).
- Optional: `MANGOHUD=1 milkdrop ...`, RenderDoc, or `intel_gpu_top` if part of the GL stack uses i915.
