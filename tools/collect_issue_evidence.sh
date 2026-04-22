#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

DEFAULT_SINCE="30 minutes ago"
DEFAULT_CORE_LIMIT=5
DEFAULT_JOURNAL_LINES=400
OUTPUT_DIR=""
SINCE="${DEFAULT_SINCE}"
CORE_LIMIT="${DEFAULT_CORE_LIMIT}"
JOURNAL_LINES="${DEFAULT_JOURNAL_LINES}"
BOOT_ONLY=0
CORE_SINCE="${DEFAULT_SINCE}"

usage() {
    cat <<'EOF'
Usage: collect_issue_evidence.sh [options]

Collect recent Milkdrop extension evidence from the user journal and coredumps.

Options:
  --since <window>        journalctl/coredumpctl time window (default: 30 minutes ago)
  --core-limit <count>    number of newest milkdrop coredumps to inspect (default: 5)
  --journal-lines <count> max lines saved for filtered journal excerpts (default: 400)
  --boot                  restrict journal and coredumps to the current boot
  --output-dir <path>     write bundle to this directory (default: logs/evidence-<timestamp>)
  --help                  show this help text

Examples:
  ./tools/collect_issue_evidence.sh
  ./tools/collect_issue_evidence.sh --since "2 hours ago" --core-limit 8
  ./tools/collect_issue_evidence.sh --boot --output-dir ./logs/manual-capture
EOF
}

require_command() {
    local command_name="$1"

    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "error: required command not found: ${command_name}" >&2
        exit 1
    fi
}

optional_command() {
    local command_name="$1"

    command -v "${command_name}" >/dev/null 2>&1
}

append_note() {
    local note_file="$1"
    local message="$2"

    printf '%s\n' "${message}" >>"${note_file}"
}

sanitize_filename() {
    printf '%s' "$1" | tr ' /:' '___'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --since)
            [[ $# -ge 2 ]] || { echo "error: --since requires a value" >&2; exit 1; }
            SINCE="$2"
            shift 2
            ;;
        --core-limit)
            [[ $# -ge 2 ]] || { echo "error: --core-limit requires a value" >&2; exit 1; }
            CORE_LIMIT="$2"
            shift 2
            ;;
        --journal-lines)
            [[ $# -ge 2 ]] || { echo "error: --journal-lines requires a value" >&2; exit 1; }
            JOURNAL_LINES="$2"
            shift 2
            ;;
        --boot)
            BOOT_ONLY=1
            shift
            ;;
        --output-dir)
            [[ $# -ge 2 ]] || { echo "error: --output-dir requires a value" >&2; exit 1; }
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

require_command journalctl

if ! [[ "${CORE_LIMIT}" =~ ^[0-9]+$ ]] || [[ "${CORE_LIMIT}" -lt 1 ]]; then
    echo "error: --core-limit must be a positive integer" >&2
    exit 1
fi

if ! [[ "${JOURNAL_LINES}" =~ ^[0-9]+$ ]] || [[ "${JOURNAL_LINES}" -lt 1 ]]; then
    echo "error: --journal-lines must be a positive integer" >&2
    exit 1
fi

TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
if [[ -z "${OUTPUT_DIR}" ]]; then
    OUTPUT_DIR="${ROOT_DIR}/logs/evidence-${TIMESTAMP}"
fi

mkdir -p "${OUTPUT_DIR}"

NOTES_FILE="${OUTPUT_DIR}/notes.txt"
SUMMARY_FILE="${OUTPUT_DIR}/summary.txt"
JOURNAL_FILE="${OUTPUT_DIR}/journal-user-all.txt"
MILKDROP_FILE="${OUTPUT_DIR}/journal-milkdrop-filtered.txt"
SHELL_FILE="${OUTPUT_DIR}/journal-shell-filtered.txt"
CORE_LIST_FILE="${OUTPUT_DIR}/coredump-list.txt"
ENV_FILE="${OUTPUT_DIR}/environment.txt"

append_note "${NOTES_FILE}" "Milkdrop issue evidence bundle"
append_note "${NOTES_FILE}" "Generated at: $(date --iso-8601=seconds)"
append_note "${NOTES_FILE}" "Since: ${SINCE}"
append_note "${NOTES_FILE}" "Boot only: ${BOOT_ONLY}"
append_note "${NOTES_FILE}" "Core limit: ${CORE_LIMIT}"
append_note "${NOTES_FILE}" "Journal lines: ${JOURNAL_LINES}"

JOURNAL_ARGS=(--user --since "${SINCE}" --no-pager -o short-iso)
CORE_ARGS=(--no-pager --since "${CORE_SINCE}")

if [[ "${BOOT_ONLY}" -eq 1 ]]; then
    JOURNAL_ARGS+=(--boot)
    if command -v uptime >/dev/null 2>&1; then
        CORE_SINCE="$(uptime -s)"
        CORE_ARGS=(--no-pager --since "${CORE_SINCE}")
    else
        append_note "${NOTES_FILE}" "uptime not available; coredump capture remains bounded by --since=${SINCE}."
    fi
fi

{
    echo "timestamp=$(date --iso-8601=seconds)"
    echo "cwd=${ROOT_DIR}"
    echo "since=${SINCE}"
    echo "boot_only=${BOOT_ONLY}"
    echo "core_limit=${CORE_LIMIT}"
    echo "journal_lines=${JOURNAL_LINES}"
    echo "uname=$(uname -a)"
    if optional_command gnome-shell; then
        echo "gnome_shell_version=$(gnome-shell --version 2>/dev/null || true)"
    fi
    if optional_command gsettings; then
        echo "extension_enabled=$(gsettings get org.gnome.shell.extensions.milkdrop enabled 2>/dev/null || echo unavailable)"
        echo "extension_monitor=$(gsettings get org.gnome.shell.extensions.milkdrop monitor 2>/dev/null || echo unavailable)"
        echo "extension_preset_dir=$(gsettings get org.gnome.shell.extensions.milkdrop preset-dir 2>/dev/null || echo unavailable)"
    fi
} >"${ENV_FILE}"

journalctl "${JOURNAL_ARGS[@]}" >"${JOURNAL_FILE}" || true

if optional_command rg; then
    rg -i '\[milkdrop\]|milkdrop|renderer wait failed|failed to spawn renderer|control (connect|write) failed|output read failed|saveStateSync failed|gnome-shell' \
        "${JOURNAL_FILE}" | tail -n "${JOURNAL_LINES}" >"${MILKDROP_FILE}" || true
    rg -i 'gnome-shell|gjs|mutter|meta\.waylandclient|waylandclient|clutter|mutter-' \
        "${JOURNAL_FILE}" | tail -n "${JOURNAL_LINES}" >"${SHELL_FILE}" || true
else
    grep -Ei '\[milkdrop\]|milkdrop|renderer wait failed|failed to spawn renderer|control (connect|write) failed|output read failed|saveStateSync failed|gnome-shell' \
        "${JOURNAL_FILE}" | tail -n "${JOURNAL_LINES}" >"${MILKDROP_FILE}" || true
    grep -Ei 'gnome-shell|gjs|mutter|meta\.waylandclient|waylandclient|clutter|mutter-' \
        "${JOURNAL_FILE}" | tail -n "${JOURNAL_LINES}" >"${SHELL_FILE}" || true
fi

if optional_command coredumpctl; then
    coredumpctl list milkdrop "${CORE_ARGS[@]}" >"${CORE_LIST_FILE}" || true

    mapfile -t CORE_IDS < <(
        coredumpctl list milkdrop --json=short "${CORE_ARGS[@]}" 2>/dev/null \
            | awk -F'"pid":' 'NF > 1 {
                split($2, parts, ",");
                gsub(/[^0-9]/, "", parts[1]);
                if (parts[1] != "")
                    print parts[1];
            }' \
            | tail -n "${CORE_LIMIT}"
    )

    if [[ ${#CORE_IDS[@]} -eq 0 ]]; then
        append_note "${NOTES_FILE}" "No milkdrop coredumps found for the requested window."
    else
        append_note "${NOTES_FILE}" "Detailed coredump info saved for ${#CORE_IDS[@]} entries."
        for core_id in "${CORE_IDS[@]}"; do
            core_file="${OUTPUT_DIR}/coredump-info-$(sanitize_filename "${core_id}").txt"
            coredumpctl info "${core_id}" "${CORE_ARGS[@]}" >"${core_file}" || true
        done
    fi
else
    append_note "${NOTES_FILE}" "coredumpctl not available; skipping coredump inspection."
fi

MILKDROP_LINES=$(wc -l <"${MILKDROP_FILE}" 2>/dev/null || echo 0)
SHELL_LINES=$(wc -l <"${SHELL_FILE}" 2>/dev/null || echo 0)
CORE_FILES=$(find "${OUTPUT_DIR}" -maxdepth 1 -type f -name 'coredump-info-*.txt' | wc -l)

{
    echo "Milkdrop evidence bundle"
    echo "output_dir=${OUTPUT_DIR}"
    echo "generated_at=$(date --iso-8601=seconds)"
    echo "since=${SINCE}"
    echo "boot_only=${BOOT_ONLY}"
    echo "milkdrop_log_lines=${MILKDROP_LINES}"
    echo "shell_log_lines=${SHELL_LINES}"
    echo "coredump_details=${CORE_FILES}"
    echo
    echo "Files:"
    echo "- ${ENV_FILE}"
    echo "- ${JOURNAL_FILE}"
    echo "- ${MILKDROP_FILE}"
    echo "- ${SHELL_FILE}"
    echo "- ${CORE_LIST_FILE}"
} >"${SUMMARY_FILE}"

echo "Evidence written to ${OUTPUT_DIR}"
echo "Summary: ${SUMMARY_FILE}"
