#!/usr/bin/env bash
# Sample NVIDIA GPU while milkdrop (or other GL apps) run. Requires nvidia-smi.
# Usage: ./tools/profile_gpu_milkdrop.sh [seconds]
# Output: artifacts/gpu_profile/gpu_profile_<timestamp>/

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/artifacts/gpu_profile/gpu_profile_$(date -u +%Y%m%dT%H%M%SZ)"
SECONDS="${1:-45}"

mkdir -p "${OUT}"

if ! command -v nvidia-smi >/dev/null; then
  echo "nvidia-smi not found; install NVIDIA drivers or use intel_gpu_top / nvtop on other GPUs." >&2
  exit 1
fi

{
  echo "=== $(date -Is) GPU profile (milkdrop) ==="
  echo "hostname: $(hostname)"
  echo "sample_seconds: ${SECONDS}"
  echo
  echo "=== nvidia-smi -L ==="
  nvidia-smi -L
  echo
  echo "=== milkdrop PIDs ==="
  pgrep -af milkdrop || echo "(none)"
  echo
  echo "=== snapshot: gpu + compute apps ==="
  nvidia-smi --query-gpu=index,name,driver_version,temperature.gpu,clocks.current.graphics,clocks.current.memory,utilization.gpu,utilization.memory,memory.used,memory.total,power.draw --format=csv
  echo
  nvidia-smi --query-compute-apps=pid,process_name,used_gpu_memory --format=csv 2>/dev/null || true
  echo
  nvidia-smi --query-accounted-apps=pid,used_gpu_memory --format=csv 2>/dev/null || true
} >"${OUT}/00_header.txt"

echo "Logging ${SECONDS}s process monitor to ${OUT}/pmon.log ..."
# -s um: utilization + framebuffer; -o T: timestamps; -d 1: 1 Hz
nvidia-smi pmon -i 0 -d 1 -c "${SECONDS}" -s um -o T -f "${OUT}/pmon.log"

echo "Device-level dmon (${SECONDS}s, 1 Hz) to ${OUT}/dmon.log ..."
nvidia-smi dmon -i 0 -d 1 -c "${SECONDS}" -s pucvmet >"${OUT}/dmon.log"

{
  echo "=== post-sample snapshot ==="
  nvidia-smi --query-gpu=utilization.gpu,utilization.memory,memory.used --format=csv
  nvidia-smi --query-compute-apps=pid,process_name,used_gpu_memory --format=csv 2>/dev/null || true
} >>"${OUT}/00_header.txt"

echo "=== milkdrop lines from pmon (sm %, mem %) ==="
rg -n "milkdrop" "${OUT}/pmon.log" | head -200 >"${OUT}/milkdrop_lines.txt" || true
wc -l "${OUT}/milkdrop_lines.txt" >>"${OUT}/milkdrop_lines.txt"

echo "Done. Artifacts: ${OUT}/"
echo "  - 00_header.txt"
echo "  - pmon.log (full ${SECONDS}s)"
echo "  - milkdrop_lines.txt (grep)"
