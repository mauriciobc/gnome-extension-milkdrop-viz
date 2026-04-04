#!/usr/bin/env bash
set -euo pipefail

EXT_UUID="milkdrop@mauriciobc.github.io"

gnome-extensions disable "${EXT_UUID}" || true
gnome-extensions enable "${EXT_UUID}"

echo "Reloaded ${EXT_UUID}."