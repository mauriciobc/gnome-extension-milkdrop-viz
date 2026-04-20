#!/usr/bin/env bash
# Compare GtkGLArea reference_renderer with SDL2 sdl_preset_snapshot (or MILKDROP_BASELINE_PPM).
#
# Modes:
#   - Single preset: set MILKDROP_COMPARE_PRESET=/path/to/file.milk (optional MILKDROP_BASELINE_PPM).
#   - Batch (default): scan preset tree — MILKDROP_COMPARE_PRESET_DIR if set, else
#     ${SRC}/reference_codebases/projectm/presets/tests; pick MILKDROP_COMPARE_RANDOM_COUNT (default 5) via pick_random_presets.py.
#
# Args: <reference_renderer> <sdl_preset_snapshot> <source_root>
# Env: DISPLAY. MILKDROP_COMPARE_RANDOM_SEED for reproducible picks (default 42 in Meson).
set -euo pipefail

REF="$1"
SDL="$2"
SRC="$3"

if [[ -z "${DISPLAY:-}" ]]; then
    echo "1..0 # SKIP compare-gtk-sdl-projectm-snapshots (no DISPLAY)"
    exit 77
fi

if [[ ! -x "$REF" ]]; then
    echo "Bail out! not executable: $REF"
    exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

export MILKDROP_FIXED_FRAME_TIME=1
export MILKDROP_FORCE_GL_API="${MILKDROP_FORCE_GL_API:-1}"
GDK_BACKEND="${GDK_BACKEND:-x11}"

FRAME="${MILKDROP_COMPARE_FRAME:-48}"
W="${MILKDROP_COMPARE_WIDTH:-320}"
H="${MILKDROP_COMPARE_HEIGHT:-240}"

run_pair() {
    local preset="$1"
    local tag="$2"

    if [[ ! -f "$preset" ]]; then
        echo "Bail out! missing preset: $preset"
        exit 1
    fi

    echo "compare_gtk-sdl: [$tag] GTK <- $(basename "$preset")"
    timeout 120s env GDK_BACKEND="$GDK_BACKEND" MILKDROP_TEXTURE_SEARCH_EXTRA="${MILKDROP_TEXTURE_SEARCH_EXTRA:-}" \
        "$REF" "$preset" "${TMP}/gtk-${tag}.ppm" "$FRAME" "$W" "$H" >"${TMP}/gtk-${tag}.log" 2>&1

    if [[ -n "${MILKDROP_BASELINE_PPM:-}" ]]; then
        if [[ ! -f "$MILKDROP_BASELINE_PPM" ]]; then
            echo "Bail out! MILKDROP_BASELINE_PPM not a file: $MILKDROP_BASELINE_PPM"
            exit 1
        fi
        echo "compare_gtk-sdl: [$tag] baseline PPM (external)"
        CMP_B="$MILKDROP_BASELINE_PPM"
    else
        if [[ ! -x "$SDL" ]]; then
            echo "1..0 # SKIP compare-gtk-sdl-projectm-snapshots (sdl_preset_snapshot not built or not executable)"
            exit 77
        fi
        echo "compare_gtk-sdl: [$tag] SDL <- $(basename "$preset")"
        timeout 120s env MILKDROP_TEXTURE_SEARCH_EXTRA="${MILKDROP_TEXTURE_SEARCH_EXTRA:-}" \
            "$SDL" "$preset" "${TMP}/sdl-${tag}.ppm" "$FRAME" "$W" "$H" >"${TMP}/sdl-${tag}.log" 2>&1
        CMP_B="${TMP}/sdl-${tag}.ppm"
    fi

    # Batch mode tolerates heavier presets (FBO vs default FB); single-file default stays strict.
    if [[ -n "${MILKDROP_COMPARE_PRESET:-}" ]]; then
        python3 "${SRC}/tests/compare_projectm_snapshots.py" "${TMP}/gtk-${tag}.ppm" "$CMP_B"
    else
        python3 "${SRC}/tests/compare_projectm_snapshots.py" "${TMP}/gtk-${tag}.ppm" "$CMP_B" \
            --mae-max "${MILKDROP_COMPARE_BATCH_MAE_MAX:-48}" \
            --bad-pct-max "${MILKDROP_COMPARE_BATCH_BAD_PCT_MAX:-35}"
    fi
}

if [[ -n "${MILKDROP_COMPARE_PRESET:-}" ]]; then
    run_pair "$MILKDROP_COMPARE_PRESET" "single"
else
    if [[ -n "${MILKDROP_BASELINE_PPM:-}" ]]; then
        echo "Bail out! MILKDROP_BASELINE_PPM requires MILKDROP_COMPARE_PRESET (single file)"
        exit 1
    fi

    # Default scan: explicit dir, else vendor tests (CI / other checkouts).
    if [[ -n "${MILKDROP_COMPARE_PRESET_DIR:-}" ]]; then
        PRESET_DIR="$MILKDROP_COMPARE_PRESET_DIR"
    else
        PRESET_DIR="${SRC}/reference_codebases/projectm/presets/tests"
    fi
    if [[ -z "${MILKDROP_TEXTURE_SEARCH_EXTRA:-}" && -d "${PRESET_DIR}/textures" ]]; then
        export MILKDROP_TEXTURE_SEARCH_EXTRA="${PRESET_DIR}/textures"
    fi

    if [[ ! -d "$PRESET_DIR" ]]; then
        echo "Bail out! preset directory is not a directory: $PRESET_DIR"
        exit 1
    fi

    N="${MILKDROP_COMPARE_RANDOM_COUNT:-5}"
    export MILKDROP_COMPARE_RANDOM_SEED="${MILKDROP_COMPARE_RANDOM_SEED:-42}"
    echo "compare_gtk-sdl: picking ${N} preset(s) from ${PRESET_DIR} (seed=${MILKDROP_COMPARE_RANDOM_SEED})"

    mapfile -t PICKS < <(python3 "${SRC}/tests/pick_random_presets.py" "$PRESET_DIR" -n "$N")
    if [[ ${#PICKS[@]} -eq 0 ]]; then
        echo "Bail out! no presets picked from $PRESET_DIR"
        exit 1
    fi

    idx=0
    for milk in "${PICKS[@]}"; do
        idx=$((idx + 1))
        echo "======== batch ${idx}/${#PICKS[@]}: ${milk} ========"
        run_pair "$milk" "$(printf '%02d' "$idx")"
    done
fi

echo "compare_gtk-sdl-projectm-snapshots: OK"
