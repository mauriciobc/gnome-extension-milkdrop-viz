#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT_UUID="milkdrop@mauriciobc.github.io"
EXT_DIR="${HOME}/.local/share/gnome-shell/extensions/${EXT_UUID}"
SCHEMA_FILE="${HOME}/.local/share/glib-2.0/schemas/org.gnome.shell.extensions.milkdrop.gschema.xml"
SCHEMA_DIR="${HOME}/.local/share/glib-2.0/schemas"
BIN_FILE="${HOME}/.local/bin/milkdrop"

rm -f "${BIN_FILE}"
rm -rf "${EXT_DIR}"
rm -f "${SCHEMA_FILE}"

if [[ -d "${SCHEMA_DIR}" ]]; then
  glib-compile-schemas "${SCHEMA_DIR}"
fi

echo "Uninstalled local extension scaffold files."