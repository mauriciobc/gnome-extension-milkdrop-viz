#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GTK_TEST="${ROOT}/build/tests/test-gtk-glarea-projectm"
SDL_SNAPSHOT="${ROOT}/build/tests/sdl_preset_snapshot"

SUSPECT_PRESET="${1:-}"
GOOD_PRESET="${2:-${ROOT}/reference_codebases/projectm/presets/tests/100-square.milk}"

FRAME="${MILKDROP_COMPARE_FRAME:-48}"
W="${MILKDROP_COMPARE_WIDTH:-320}"
H="${MILKDROP_COMPARE_HEIGHT:-240}"

if [[ -z "$SUSPECT_PRESET" ]]; then
    echo "uso: $0 <preset-suspeito.milk> [preset-bom.milk]" >&2
    exit 1
fi

if [[ -z "${DISPLAY:-}" ]]; then
    echo "erro: DISPLAY nao definido" >&2
    exit 1
fi

if [[ ! -x "$GTK_TEST" ]]; then
    echo "erro: binario ausente: $GTK_TEST" >&2
    echo "compile com: meson compile -C build test-gtk-glarea-projectm" >&2
    exit 1
fi

if [[ ! -x "$SDL_SNAPSHOT" ]]; then
    echo "erro: binario ausente: $SDL_SNAPSHOT" >&2
    echo "compile com: meson compile -C build sdl_preset_snapshot" >&2
    exit 1
fi

for preset in "$GOOD_PRESET" "$SUSPECT_PRESET"; do
    if [[ ! -f "$preset" ]]; then
        echo "erro: preset nao encontrado: $preset" >&2
        exit 1
    fi
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

analyze_ppm() {
    local ppm="$1"
    python3 - "$ppm" <<'PY'
import sys
path = sys.argv[1]
with open(path, 'rb') as f:
    magic = f.readline().strip()
    dims = f.readline().strip().decode()
    maxv = f.readline().strip()
    data = f.read()
triples = [data[i:i+3] for i in range(0, len(data), 3)]
nonblack = sum(1 for px in triples if px != b'\x00\x00\x00')
print(f"magic={magic.decode()} dims={dims} max={maxv.decode()} nonblack_pixels={nonblack}")
PY
}

run_sdl_case() {
    local label="$1"
    local preset="$2"
    local ppm="${TMP}/${label}.ppm"
    local log="${TMP}/${label}.sdl.log"

    set +e
    env \
        MILKDROP_FIXED_FRAME_TIME=1 \
        MILKDROP_FORCE_GL_API="${MILKDROP_FORCE_GL_API:-1}" \
        MILKDROP_TEXTURE_SEARCH_EXTRA="${MILKDROP_TEXTURE_SEARCH_EXTRA:-}" \
        "$SDL_SNAPSHOT" "$preset" "$ppm" "$FRAME" "$W" "$H" >"$log" 2>&1
    local rc=$?
    set -e

    echo "SDL  [$label] rc=$rc"
    if [[ -f "$ppm" ]]; then
        analyze_ppm "$ppm"
    else
        echo "ppm=ausente"
    fi
    echo "log=$log"
    sed -n '1,20p' "$log"
    echo
}

run_gtk_case() {
    local label="$1"
    local preset="$2"
    local log="${TMP}/${label}.gtk.log"

    set +e
    env \
        MILKDROP_PM_TEST_PRESET="$preset" \
        MILKDROP_PM_TEST_DIRECT_LOAD=1 \
        MILKDROP_FORCE_GL_API="${MILKDROP_FORCE_GL_API:-1}" \
        "$GTK_TEST" >"$log" 2>&1
    local rc=$?
    set -e

    echo "GTK  [$label] rc=$rc"
    echo "log=$log"
    sed -n '1,20p' "$log"
    echo
}

echo "== Controle: preset bom =="
echo "$GOOD_PRESET"
run_sdl_case "good" "$GOOD_PRESET"
run_gtk_case "good" "$GOOD_PRESET"

echo "== Suspeito: preset alvo =="
echo "$SUSPECT_PRESET"
run_sdl_case "suspect" "$SUSPECT_PRESET"
run_gtk_case "suspect" "$SUSPECT_PRESET"

echo "Artefatos temporarios: $TMP"
