#!/usr/bin/env bash
# Regenerate tests/baselines/sdl_seed42_f48_w320h240/*.ppm using sdl_preset_snapshot.
# Requires: meson build, DISPLAY, SDL2, libprojectM.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDL="${ROOT}/build/tests/sdl_preset_snapshot"
OUT="${ROOT}/tests/baselines/sdl_seed42_f48_w320h240"
N="${MILKDROP_COMPARE_RANDOM_COUNT:-5}"
SEED="${MILKDROP_COMPARE_RANDOM_SEED:-42}"
FRAME="${MILKDROP_COMPARE_FRAME:-48}"
W="${MILKDROP_COMPARE_WIDTH:-320}"
H="${MILKDROP_COMPARE_HEIGHT:-240}"

if [[ -z "${DISPLAY:-}" ]]; then
    echo "error: DISPLAY is not set" >&2
    exit 1
fi

if [[ ! -x "$SDL" ]]; then
    echo "error: build sdl_preset_snapshot first: meson compile -C build tests/sdl_preset_snapshot" >&2
    exit 1
fi

PRESET_DIR="${MILKDROP_COMPARE_PRESET_DIR:-}"
if [[ -z "$PRESET_DIR" ]]; then
    if [[ -d /home/mauriciobc/presets ]]; then
        PRESET_DIR=/home/mauriciobc/presets
    else
        PRESET_DIR="${ROOT}/reference_codebases/projectm/presets/tests"
    fi
fi

if [[ ! -d "$PRESET_DIR" ]]; then
    echo "error: preset dir not found: $PRESET_DIR" >&2
    exit 1
fi

if [[ -z "${MILKDROP_TEXTURE_SEARCH_EXTRA:-}" && -d /home/mauriciobc/presets/textures ]]; then
    export MILKDROP_TEXTURE_SEARCH_EXTRA=/home/mauriciobc/presets/textures
fi

mkdir -p "$OUT"
export MILKDROP_FIXED_FRAME_TIME=1 MILKDROP_FORCE_GL_API="${MILKDROP_FORCE_GL_API:-1}"
export MILKDROP_COMPARE_RANDOM_SEED="$SEED"
GDK_BACKEND="${GDK_BACKEND:-x11}"

MAN="${OUT}/MANIFEST.txt"
{
    echo "# SDL snapshot baselines — $(date -Iseconds 2>/dev/null || date)"
    echo "# frame=$FRAME size=${W}x${H} MILKDROP_FIXED_FRAME_TIME=1 seed=$SEED"
    echo "# preset_dir=$PRESET_DIR"
    echo
} >"$MAN"

i=0
while IFS= read -r milk; do
    [[ -z "$milk" ]] && continue
    i=$((i + 1))
    tag=$(printf '%02d' "$i")
    ppm="${OUT}/baseline_${tag}.ppm"
    echo "[$tag/$N] $milk"
    timeout 180s env GDK_BACKEND="$GDK_BACKEND" \
        MILKDROP_TEXTURE_SEARCH_EXTRA="${MILKDROP_TEXTURE_SEARCH_EXTRA:-}" \
        "$SDL" "$milk" "$ppm" "$FRAME" "$W" "$H" >"${OUT}/render_${tag}.log" 2>&1
    printf 'baseline_%s.ppm\t%s\n' "$tag" "$milk" >>"$MAN"
done < <(python3 "${ROOT}/tests/pick_random_presets.py" "$PRESET_DIR" -n "$N" --seed "$SEED")

echo "Wrote $OUT (see MANIFEST.txt)"
