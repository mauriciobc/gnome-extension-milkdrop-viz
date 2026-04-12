#!/usr/bin/env bash
# Isolated nested GNOME Shell for extension testing: installs under .nested-shell/
# (not ~/.local) and uses a separate XDG config/data so host gsettings is untouched.
#
# Requires: gnome-shell, dbus-run-session, mutter-devkit (GNOME 49+), Meson, Ninja.
# GNOME 49+: gnome-shell --devkit --wayland. Older: --nested --wayland.
#
# Usage: MILKDROP_NEST_ROOT=/path ./tools/nested_devkit.sh [options]
# Options: --skip-build  --clean --force  --debug  --help

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT_UUID="milkdrop@mauriciobc.github.io"

NEST_ROOT="${MILKDROP_NEST_ROOT:-${ROOT_DIR}/.nested-shell}"
NEST_CONFIG="${NEST_ROOT}/config"
NEST_DATA="${NEST_ROOT}/data"
NEST_BUILD="${NEST_ROOT}/build"

SKIP_BUILD=0
CLEAN=0
FORCE=0
DEBUG=0

usage() {
    sed -n '1,20p' "$0" | tail -n +2
    cat <<EOF

Options:
  --skip-build   Skip meson compile/install (still runs glib-compile-schemas and gsettings).
  --clean        Remove the nest directory before building (requires --force).
  --force        Allow destructive --clean without prompt.
  --debug        Set G_MESSAGES_DEBUG=all and SHELL_DEBUG=all for gnome-shell.
  --help         Show this help.

Environment:
  MILKDROP_NEST_ROOT   Override nest root (default: ${ROOT_DIR}/.nested-shell)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1 ;;
        --clean) CLEAN=1 ;;
        --force) FORCE=1 ;;
        --debug) DEBUG=1 ;;
        --help|-h) usage; exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
    shift
done

if [[ "${CLEAN}" -eq 1 ]]; then
    if [[ "${FORCE}" -ne 1 ]]; then
        echo "Refusing to --clean without --force (prevents accidental data loss)." >&2
        exit 1
    fi
    rm -rf "${NEST_ROOT}"
fi

mkdir -p "${NEST_CONFIG}" "${NEST_DATA}"

gnome_shell_major() {
    gnome-shell --version 2>/dev/null | awk '{print int($3)}'
}

shell_args_for_version() {
    local major
    major="$(gnome_shell_major)"
    if [[ -z "${major}" ]] || [[ "${major}" -lt 1 ]]; then
        echo "Could not parse gnome-shell --version" >&2
        exit 1
    fi
    if [[ "${major}" -ge 49 ]]; then
        echo "--devkit --wayland"
    else
        echo "--nested --wayland"
    fi
}

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
    meson setup "${NEST_BUILD}" --prefix "${NEST_DATA}" --reconfigure
    meson compile -C "${NEST_BUILD}"
    meson install -C "${NEST_BUILD}"
fi

glib-compile-schemas "${NEST_DATA}/share/glib-2.0/schemas"

export XDG_CONFIG_HOME="${NEST_CONFIG}"
# XDG_DATA_HOME must be the *share* directory (like ~/.local/share), not the Meson
# prefix root. Otherwise g_get_user_data_dir() points at NEST_DATA/ and Shell looks
# for extensions in NEST_DATA/gnome-shell/extensions/ while meson installs to
# NEST_DATA/share/gnome-shell/extensions/.
export XDG_DATA_HOME="${NEST_DATA}/share"
export PATH="${NEST_DATA}/bin:${PATH}"

# gsettings does not reliably merge the nest's gschemas.compiled with only
# XDG_DATA_HOME; prepend the nest schema dir so org.gnome.shell.extensions.milkdrop
# resolves while keeping system schemas (org.gnome.shell, etc.).
NEST_SCHEMAS="${NEST_DATA}/share/glib-2.0/schemas"
if [[ -z "${GSETTINGS_SCHEMA_DIR:-}" ]]; then
    export GSETTINGS_SCHEMA_DIR="${NEST_SCHEMAS}:/usr/share/glib-2.0/schemas"
else
    export GSETTINGS_SCHEMA_DIR="${NEST_SCHEMAS}:${GSETTINGS_SCHEMA_DIR}"
fi

gsettings set org.gnome.shell enabled-extensions "['${EXT_UUID}']"
gsettings set org.gnome.shell.extensions.milkdrop enabled true

read -r -a SHELL_EXTRA_ARGS <<< "$(shell_args_for_version)"

ENV_PREFIX=()
if [[ "${DEBUG}" -eq 1 ]]; then
    ENV_PREFIX=(G_MESSAGES_DEBUG=all SHELL_DEBUG=all)
fi

exec dbus-run-session -- env \
    "${ENV_PREFIX[@]}" \
    XDG_CONFIG_HOME="${XDG_CONFIG_HOME}" \
    XDG_DATA_HOME="${XDG_DATA_HOME}" \
    GSETTINGS_SCHEMA_DIR="${GSETTINGS_SCHEMA_DIR}" \
    PATH="${PATH}" \
    gnome-shell "${SHELL_EXTRA_ARGS[@]}"
