#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="${1:-/home/kdw73/Tullius_ctd_loger}"
WIN_ROOT="${2:-/mnt/c/Users/kdw73/Tullius_ctd_loger}"
ZIP_PATH="${WIN_ROOT}/dist/Tullius_ctd_loger.zip"

echo "[gate] repo=${REPO_ROOT}"
echo "[gate] win=${WIN_ROOT}"

echo "[gate] 1/5 script sync hashes"
sha256sum "${REPO_ROOT}/scripts/build-winui.cmd" "${WIN_ROOT}/scripts/build-winui.cmd"
sha256sum "${REPO_ROOT}/scripts/package.py" "${WIN_ROOT}/scripts/package.py"

echo "[gate] 2/5 required WinUI files"
for f in \
  "${WIN_ROOT}/build-winui/SkyrimDiagDumpToolWinUI.exe" \
  "${WIN_ROOT}/build-winui/SkyrimDiagDumpToolWinUI.pri" \
  "${WIN_ROOT}/build-winui/App.xbf" \
  "${WIN_ROOT}/build-winui/MainWindow.xbf"; do
  [[ -f "${f}" ]] || { echo "missing: ${f}"; exit 1; }
done

echo "[gate] 3/5 required zip entries"
entries="$(unzip -Z1 "${ZIP_PATH}")"
for p in \
  "SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe" \
  "SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.pri" \
  "SKSE/Plugins/SkyrimDiagWinUI/App.xbf" \
  "SKSE/Plugins/SkyrimDiagWinUI/MainWindow.xbf"; do
  echo "${entries}" | rg -x -q "${p}" || { echo "missing zip entry: ${p}"; exit 1; }
done

echo "[gate] 4/5 size guard"
ls -lh "${ZIP_PATH}"
size_bytes="$(stat -c%s "${ZIP_PATH}")"
if (( size_bytes > 25 * 1024 * 1024 )); then
  echo "zip too large: ${size_bytes} bytes (>25MB)"
  exit 1
fi

echo "[gate] 5/5 nested path guard"
if unzip -Z1 "${ZIP_PATH}" | rg -q '^SKSE/Plugins/SkyrimDiagWinUI/(publish|win-x64|x64)/'; then
  echo "nested winui output detected in zip"
  exit 1
fi

echo "[gate] OK"

