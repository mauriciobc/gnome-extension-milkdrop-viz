#!/usr/bin/env bash
set -euo pipefail
LOG_FILE="./nested_shell.log"
rm -f "${LOG_FILE}"
echo "Starting nested shell..."
# Ensure renderer is enabled in gsettings
gsettings set org.gnome.shell.extensions.milkdrop enabled true
# Ensure the extension is actually loaded by the shell
EXT_UUID="milkdrop@mauriciobc.github.io"
# Read current enabled extensions and add milkdrop
CURRENT_EXTS=$(gsettings get org.gnome.shell enabled-extensions)
if [[ "${CURRENT_EXTS}" == "@as []" ]]; then
    NEW_EXTS="['${EXT_UUID}']"
elif [[ "${CURRENT_EXTS}" != *"${EXT_UUID}"* ]]; then
    NEW_EXTS="${CURRENT_EXTS%]*}, '${EXT_UUID}']"
else
    NEW_EXTS="${CURRENT_EXTS}"
fi
gsettings set org.gnome.shell enabled-extensions "${NEW_EXTS}"
# Run nested shell with all debug messages
dbus-run-session -- env G_MESSAGES_DEBUG=all gnome-shell --devkit --wayland >"${LOG_FILE}" 2>&1 &
SHELL_PID=$!
echo "Shell started with PID ${SHELL_PID}. Waiting 45s for more init..."
sleep 45
echo "Killing shell..."
kill "${SHELL_PID}" || true
echo "Log captured in ${LOG_FILE}"
