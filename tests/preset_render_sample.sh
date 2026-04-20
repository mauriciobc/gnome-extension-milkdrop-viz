#!/usr/bin/env bash
# Smoke: reference_renderer must produce non-blank frames for a few vendor test presets.
# Requires DISPLAY; exits 77 when skipped (CI without head).
set -euo pipefail

REF="$1"
SRC="$2"

if [[ -z "${DISPLAY:-}" ]]; then
    echo "1..0 # SKIP preset-render-sample (no DISPLAY)"
    exit 77
fi

if [[ ! -x "$REF" ]]; then
    echo "Bail out! reference_renderer not executable: $REF"
    exit 1
fi

BASE="${SRC}/reference_codebases/projectm/presets/tests"
if [[ ! -d "$BASE" ]]; then
    echo "Bail out! missing vendor preset tree: $BASE"
    exit 1
fi

export MILKDROP_FORCE_GL_API="${MILKDROP_FORCE_GL_API:-1}"
GDK_BACKEND="${GDK_BACKEND:-x11}"

run_one() {
    local milk="$1"
    local tmp
    tmp="$(mktemp)"
    if ! timeout 35s env GDK_BACKEND="$GDK_BACKEND" "$REF" "$milk" "${tmp}.ppm" 50 320 240 >"${tmp}.log" 2>&1; then
        echo "FAIL: reference_renderer exit/timeout for $milk"
        sed -n '1,120p' "${tmp}.log" || true
        rm -f "$tmp" "${tmp}.ppm" "${tmp}.log"
        exit 1
    fi
    if grep -q 'Non-zero bytes: 0 /' "${tmp}.log"; then
        echo "FAIL: blank framebuffer for $milk"
        sed -n '1,120p' "${tmp}.log" || true
        rm -f "$tmp" "${tmp}.ppm" "${tmp}.log"
        exit 1
    fi
    rm -f "$tmp" "${tmp}.ppm" "${tmp}.log"
}

run_one "${BASE}/100-square.milk"
run_one "${BASE}/260-compshader-noise_lq.milk"
run_one "${BASE}/110-per_pixel.milk"
echo "preset-render-sample: ok (3 presets)"
