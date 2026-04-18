#!/usr/bin/env bash
# Visual smoke: milkdrop on /usr/local/share/projectM/presets with --verbose.
#
# "Multiple presets" check (automated, no control socket):
#  - Async scan loads the tree in chunks; we require >=2 log lines for added batches
#    (proves more than one .milk batch was merged into the playlist).
#  - At least one preset_switched line (proves a real preset became active).
#
# Invoked by Meson: first arg = path to built milkdrop binary.
set -euo pipefail

MILKDROP_BIN="${1:?missing milkdrop binary path}"
PRESET_DIR="/usr/local/share/projectM/presets"

RUN_SEC="${MILKDROP_VIS_RUN_SEC:-12}"
MIN_BATCH_LINES="${MILKDROP_VIS_MIN_PLAYLIST_BATCH_LINES:-2}"

if [[ ! -d "$PRESET_DIR" ]]; then
  echo "skip: preset directory not found: $PRESET_DIR"
  exit 77
fi

if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
  echo "skip: no DISPLAY or WAYLAND_DISPLAY"
  exit 77
fi

export MILKDROP_FORCE_GL_API="${MILKDROP_FORCE_GL_API:-1}"

LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT

set +e
timeout "${RUN_SEC}s" "$MILKDROP_BIN" --verbose --preset-dir "$PRESET_DIR" >"$LOG" 2>&1
code=$?
set -e

# GNU timeout: 124 = time limit reached (expected).
if [[ "$code" -ne 124 ]] && [[ "$code" -ne 0 ]]; then
  echo "milkdrop exited with status $code"
  tail -80 "$LOG" >&2 || true
  exit 1
fi

batches=$(grep -c 'playlist: added batch of .*presets asynchronously' "$LOG" || true)
switches=$(grep -c 'preset_switched:' "$LOG" || true)

if [[ "$batches" -lt "$MIN_BATCH_LINES" ]]; then
  echo "FAIL: expected at least $MIN_BATCH_LINES playlist batch log lines, got $batches"
  tail -120 "$LOG" >&2 || true
  exit 1
fi

if [[ "$switches" -lt 1 ]]; then
  echo "FAIL: expected at least 1 preset_switched log line, got $switches"
  tail -120 "$LOG" >&2 || true
  exit 1
fi

echo "OK: playlist batches=$batches, preset_switched=$switches (${RUN_SEC}s)"
exit 0
