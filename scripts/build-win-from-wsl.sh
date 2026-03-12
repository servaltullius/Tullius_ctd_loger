#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${WSL_DISTRO_NAME:-}" ]] && ! grep -qi microsoft /proc/version 2>/dev/null; then
  echo "ERROR: build-win-from-wsl.sh must be run from WSL." >&2
  exit 2
fi

if ! command -v wslpath >/dev/null 2>&1; then
  echo "ERROR: wslpath is required to convert the batch path." >&2
  exit 3
fi

POWERSHELL_EXE="/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"
if [[ ! -x "${POWERSHELL_EXE}" ]]; then
  if command -v powershell.exe >/dev/null 2>&1; then
    POWERSHELL_EXE="powershell.exe"
  else
    echo "ERROR: Windows PowerShell executable not found." >&2
    exit 4
  fi
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BATCH_PATH="${SCRIPT_DIR}/build-win.cmd"
if [[ ! -f "${BATCH_PATH}" ]]; then
  echo "ERROR: batch script not found: ${BATCH_PATH}" >&2
  exit 5
fi

WIN_BATCH_PATH="$(wslpath -w "${BATCH_PATH}")"
exec "${POWERSHELL_EXE}" -NoProfile -Command "& '${WIN_BATCH_PATH}'"
