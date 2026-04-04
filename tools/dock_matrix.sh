#!/usr/bin/env bash
set -euo pipefail

orig_enabled=$(gsettings get org.gnome.shell enabled-extensions)
mkdir -p /tmp/milkdrop-matrix
printf '%s\n' "$orig_enabled" > /tmp/milkdrop-matrix/orig-enabled.txt

restore_settings() {
  if [[ -f /tmp/milkdrop-matrix/orig-enabled.txt ]]; then
    gsettings set org.gnome.shell enabled-extensions "$(cat /tmp/milkdrop-matrix/orig-enabled.txt)" || true
  fi
}
trap restore_settings EXIT

milk_uuid='milkdrop@mauriciobc.github.io'
dash_uuid=$(gnome-extensions list | rg -i 'dash2dock-lite|dash2dock' | head -n1 || true)
tile_uuid=$(gnome-extensions list | rg -i 'tilingshell' | head -n1 || true)

if [[ -z "$dash_uuid" ]]; then
  echo 'ERROR: Could not find dash2dock UUID via gnome-extensions list' >&2
  exit 2
fi

summary=/tmp/milkdrop-matrix/summary.tsv
printf 'case\textensions\tlayout_count\tdash_enabled\tdash_disabled\tmilk_anchor\truntime_js_errors\n' > "$summary"

run_case() {
  local case_name="$1"
  local ext_list="$2"
  local log_file="/tmp/milkdrop-matrix/${case_name}.log"

  gsettings set org.gnome.shell enabled-extensions "$ext_list"
  timeout -k 5s 18s dbus-run-session -- gnome-shell --devkit --wayland > "$log_file" 2>&1 || true

  local layout_count dash_enabled dash_disabled milk_anchor shutdown_line runtime_js_errors
  layout_count=$(rg -c 'unable to layout\(\)' "$log_file" || true)
  dash_enabled=$(rg -c 'dash2dock-lite enabled' "$log_file" || true)
  dash_disabled=$(rg -c 'dash2dock-lite disabled' "$log_file" || true)
  milk_anchor=$(rg -c '\[milkdrop\] renderer window anchored at bottom' "$log_file" || true)

  shutdown_line=$(rg -n 'Shutting down GNOME Shell' "$log_file" | head -n1 | cut -d: -f1 || true)
  if [[ -n "$shutdown_line" ]]; then
    runtime_js_errors=$(head -n "$((shutdown_line - 1))" "$log_file" | rg -c 'Gjs-CRITICAL|JS ERROR|TypeError' || true)
  else
    runtime_js_errors=$(rg -c 'Gjs-CRITICAL|JS ERROR|TypeError' "$log_file" || true)
  fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$case_name" "$ext_list" "$layout_count" "$dash_enabled" "$dash_disabled" "$milk_anchor" "$runtime_js_errors" >> "$summary"
}

run_case 'dash_only' "['$dash_uuid']"
run_case 'dash_milk' "['$dash_uuid', '$milk_uuid']"
run_case 'dash_tile' "['$dash_uuid', '$tile_uuid']"
run_case 'dash_tile_milk' "['$dash_uuid', '$tile_uuid', '$milk_uuid']"

cat "$summary"
