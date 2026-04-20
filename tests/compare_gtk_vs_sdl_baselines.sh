#!/usr/bin/env bash
# Compare GtkGLArea reference_renderer output against pre-captured SDL baseline PPMs.
#
# Each baseline was rendered with sdl_preset_snapshot (default framebuffer) and stored
# in tests/baselines/sdl_seed42_f48_w320h240/.  The MANIFEST.txt maps each
# baseline_*.ppm to the source .milk file.  This test renders every preset with our
# GTK FBO renderer and checks pixel-level parity against the baseline.
#
# Some complex presets (e.g. particle/feedback effects) render differently in
# FBO mode vs SDL default framebuffer — those will show high MAE but are still
# informative for tracking regressions.  All presets are run regardless; the
# script exits 1 if any preset exceeds the thresholds.
#
# Modes:
#   - Full manifest (default): compares all presets listed in MANIFEST.txt.
#   - Single preset: set MILKDROP_COMPARE_PRESET=/path/to/file.milk —
#     finds the matching baseline from MANIFEST (fails if not found).
#
# Args: <reference_renderer> <source_root>
# Required env: DISPLAY
# Optional env:
#   MILKDROP_COMPARE_PRESET       — single .milk to test (absolute path)
#   MILKDROP_TEXTURE_SEARCH_EXTRA — extra texture directory
#   MILKDROP_COMPARE_FRAME        — frame to capture (default: 48)
#   MILKDROP_COMPARE_WIDTH        — render width  (default: 320)
#   MILKDROP_COMPARE_HEIGHT       — render height (default: 240)
#   MILKDROP_COMPARE_MAE_MAX      — MAE threshold (default: 14.0, single-preset)
#   MILKDROP_COMPARE_BAD_PCT_MAX  — bad-pixel % threshold (default: 7.5, single-preset)
#   MILKDROP_COMPARE_BATCH_MAE_MAX      — batch MAE threshold (default: 48)
#   MILKDROP_COMPARE_BATCH_BAD_PCT_MAX  — batch bad-pixel % threshold (default: 35)
set -uo pipefail

REF="$1"
SRC="$2"

BASELINE_DIR="${SRC}/tests/baselines/sdl_seed42_f48_w320h240"
MANIFEST="${BASELINE_DIR}/MANIFEST.txt"

if [[ -z "${DISPLAY:-}" ]]; then
    echo "1..0 # SKIP compare-gtk-vs-sdl-baselines (no DISPLAY)"
    exit 77
fi

if [[ ! -x "$REF" ]]; then
    echo "Bail out! reference_renderer not executable: $REF"
    exit 1
fi

if [[ ! -f "$MANIFEST" ]]; then
    echo "Bail out! baseline MANIFEST not found: $MANIFEST"
    exit 1
fi

TMP=$(mktemp -d)
ARTIFACTS="${ARTIFACTS_DIR:-/tmp/milkdrop-baseline-artifacts}"
_failed_tags=()
_cleanup() {
    if [[ ${#_failed_tags[@]} -gt 0 ]]; then
        echo "compare-gtk-vs-sdl-baselines: failing artifacts saved to $ARTIFACTS"
    else
        rm -rf "$TMP"
    fi
}
trap '_cleanup' EXIT

export MILKDROP_FIXED_FRAME_TIME=1
export MILKDROP_FORCE_GL_API="${MILKDROP_FORCE_GL_API:-1}"
GDK_BACKEND="${GDK_BACKEND:-x11}"

FRAME="${MILKDROP_COMPARE_FRAME:-48}"
W="${MILKDROP_COMPARE_WIDTH:-320}"
H="${MILKDROP_COMPARE_HEIGHT:-240}"

# Single-preset mode uses strict thresholds; batch uses looser ones to tolerate
# FBO vs default-framebuffer divergence on complex particle/feedback presets.
if [[ -n "${MILKDROP_COMPARE_PRESET:-}" ]]; then
    MAE_MAX="${MILKDROP_COMPARE_MAE_MAX:-14.0}"
    BAD_PCT_MAX="${MILKDROP_COMPARE_BAD_PCT_MAX:-7.5}"
else
    MAE_MAX="${MILKDROP_COMPARE_BATCH_MAE_MAX:-48}"
    BAD_PCT_MAX="${MILKDROP_COMPARE_BATCH_BAD_PCT_MAX:-35}"
fi

run_one() {
    local preset="$1"
    local baseline="$2"
    local tag="$3"

    if [[ ! -f "$preset" ]]; then
        echo "SKIP [$tag] preset not found: $preset" >&2
        return 0
    fi

    if [[ ! -f "$baseline" ]]; then
        echo "Bail out! baseline PPM not found: $baseline"
        exit 1
    fi

    local gtk_ppm="${TMP}/gtk-${tag}.ppm"
    local gtk_log="${TMP}/gtk-${tag}.log"

    echo "compare_gtk-sdl-baselines: [$tag] rendering $(basename "$preset")"
    timeout 120s env \
        GDK_BACKEND="$GDK_BACKEND" \
        MILKDROP_TEXTURE_SEARCH_EXTRA="${MILKDROP_TEXTURE_SEARCH_EXTRA:-}" \
        "$REF" "$preset" "$gtk_ppm" "$FRAME" "$W" "$H" \
        >"$gtk_log" 2>&1 \
    || {
        echo "Bail out! reference_renderer failed for [$tag]. Log:"
        cat "$gtk_log" >&2
        exit 1
    }

    if ! python3 "${SRC}/tests/compare_projectm_snapshots.py" \
            "$gtk_ppm" "$baseline" \
            --mae-max "$MAE_MAX" \
            --bad-pct-max "$BAD_PCT_MAX"; then
        # Save artifacts for post-mortem inspection; continue to next preset.
        mkdir -p "$ARTIFACTS"
        cp "$gtk_ppm"  "$ARTIFACTS/failed-${tag}-gtk.ppm"
        cp "$baseline" "$ARTIFACTS/failed-${tag}-baseline.ppm"
        cp "$gtk_log"  "$ARTIFACTS/failed-${tag}-gtk.log"
        _failed_tags+=("$tag")
    fi
}

if [[ -n "${MILKDROP_COMPARE_PRESET:-}" ]]; then
    # Single-preset mode: find the matching baseline in MANIFEST.
    target=$(realpath "$MILKDROP_COMPARE_PRESET")
    matched_baseline=""
    while IFS=$'\t' read -r bfile bpreset; do
        [[ "$bfile" == \#* ]] && continue
        [[ -z "$bfile" ]] && continue
        if [[ "$(realpath "$bpreset" 2>/dev/null || echo "$bpreset")" == "$target" ]]; then
            matched_baseline="${BASELINE_DIR}/${bfile}"
            break
        fi
    done < "$MANIFEST"

    if [[ -z "$matched_baseline" ]]; then
        echo "Bail out! preset not in MANIFEST: $MILKDROP_COMPARE_PRESET"
        echo "  Known presets in MANIFEST:"
        grep -v '^#' "$MANIFEST" | awk -F'\t' '{print "  " $2}' >&2
        exit 1
    fi

    run_one "$MILKDROP_COMPARE_PRESET" "$matched_baseline" "single"
else
    # Full manifest mode: run all presets, collect failures.
    idx=0
    while IFS=$'\t' read -r bfile bpreset; do
        [[ "$bfile" == \#* ]] && continue
        [[ -z "$bfile" ]] && continue
        idx=$((idx + 1))
        baseline="${BASELINE_DIR}/${bfile}"
        echo "======== manifest ${idx}: ${bpreset} ========"
        run_one "$bpreset" "$baseline" "$(printf '%02d' "$idx")"
    done < "$MANIFEST"

    if [[ "$idx" -eq 0 ]]; then
        echo "Bail out! MANIFEST is empty: $MANIFEST"
        exit 1
    fi
fi

if [[ ${#_failed_tags[@]} -gt 0 ]]; then
    echo "compare-gtk-vs-sdl-baselines: FAILED ${#_failed_tags[@]} preset(s): ${_failed_tags[*]}"
    exit 1
fi

echo "compare-gtk-vs-sdl-baselines: OK"
