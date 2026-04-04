#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
PREFIX="${HOME}/.local"
EXT_UUID="milkdrop@mauriciobc.github.io"
EXT_DIR="${HOME}/.local/share/gnome-shell/extensions/${EXT_UUID}"
SCHEMA_DIR="${HOME}/.local/share/glib-2.0/schemas"
EXT_SCHEMA_DIR="${EXT_DIR}/schemas"

meson setup "${BUILD_DIR}" --prefix "${PREFIX}" --reconfigure
meson compile -C "${BUILD_DIR}"
meson install -C "${BUILD_DIR}"

# Keep local installs deterministic and avoid stale schema/key conflicts.
rm -rf "${EXT_DIR}"
mkdir -p "${EXT_DIR}"
cp -f "${ROOT_DIR}/extension/${EXT_UUID}/metadata.json" "${EXT_DIR}/metadata.json"
cp -f "${ROOT_DIR}/extension/${EXT_UUID}/extension.js" "${EXT_DIR}/extension.js"
cp -f "${ROOT_DIR}/extension/${EXT_UUID}/prefs.js" "${EXT_DIR}/prefs.js"
cp -f "${ROOT_DIR}/extension/${EXT_UUID}/constants.js" "${EXT_DIR}/constants.js"
cp -f "${ROOT_DIR}/extension/${EXT_UUID}/controlClient.js" "${EXT_DIR}/controlClient.js"

mkdir -p "${EXT_SCHEMA_DIR}"
cp -f "${ROOT_DIR}/data/org.gnome.shell.extensions.milkdrop.gschema.xml" "${EXT_SCHEMA_DIR}/"
glib-compile-schemas "${EXT_SCHEMA_DIR}"

mkdir -p "${SCHEMA_DIR}"
cp -f "${ROOT_DIR}/data/org.gnome.shell.extensions.milkdrop.gschema.xml" "${SCHEMA_DIR}/"
glib-compile-schemas "${SCHEMA_DIR}"

echo "Installed renderer and extension scaffold."