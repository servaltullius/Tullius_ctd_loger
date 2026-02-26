#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="${1:-/home/kdw73/Tullius_ctd_loger}"
WIN_ROOT="${2:-/mnt/c/Users/kdw73/Tullius_ctd_loger}"
ZIP_PATH="${WIN_ROOT}/dist/Tullius_ctd_loger.zip"
PYTHON_BIN="$(command -v python3 || command -v python || true)"

if [[ -z "${PYTHON_BIN}" ]]; then
  echo "python interpreter not found (python3/python required)"
  exit 1
fi

readarray -t REQUIRED_WINUI_BUILD_OUTPUTS < <(
  PYTHONPATH="${REPO_ROOT}/scripts" "${PYTHON_BIN}" - <<'PY'
from release_contract import REQUIRED_WINUI_BUILD_OUTPUTS

for item in REQUIRED_WINUI_BUILD_OUTPUTS:
    print(item)
PY
)

readarray -t REQUIRED_ZIP_ENTRIES < <(
  PYTHONPATH="${REPO_ROOT}/scripts" "${PYTHON_BIN}" - <<'PY'
from release_contract import REQUIRED_ZIP_ENTRIES

for item in REQUIRED_ZIP_ENTRIES:
    print(item)
PY
)

NESTED_WINUI_REGEX="$({
  PYTHONPATH="${REPO_ROOT}/scripts" "${PYTHON_BIN}" - <<'PY'
from release_contract import nested_winui_path_regex

print(nested_winui_path_regex())
PY
})"

hash_of() {
  sha256sum "$1" | cut -d' ' -f1
}

assert_synced() {
  local left="$1"
  local right="$2"
  local label="$3"
  local left_hash
  local right_hash

  left_hash="$(hash_of "${left}")"
  right_hash="$(hash_of "${right}")"

  echo "  - ${label}"
  echo "    repo=${left_hash}"
  echo "    win =${right_hash}"

  if [[ "${left_hash}" != "${right_hash}" ]]; then
    echo "hash mismatch: ${label}"
    exit 1
  fi
}

echo "[gate] repo=${REPO_ROOT}"
echo "[gate] win=${WIN_ROOT}"

echo "[gate] 1/5 script sync hashes"
if [[ "${REPO_ROOT}" == "${WIN_ROOT}" ]]; then
  echo "  - same root path; sync hash comparison skipped"
else
  assert_synced "${REPO_ROOT}/scripts/build-winui.cmd" "${WIN_ROOT}/scripts/build-winui.cmd" "build-winui.cmd"
  assert_synced "${REPO_ROOT}/scripts/package.py" "${WIN_ROOT}/scripts/package.py" "package.py"
fi

echo "[gate] 2/5 required WinUI files"
for asset in "${REQUIRED_WINUI_BUILD_OUTPUTS[@]}"; do
  f="${WIN_ROOT}/build-winui/${asset}"
  [[ -f "${f}" ]] || { echo "missing: ${f}"; exit 1; }
done

echo "[gate] 3/5 required zip entries"
entries="$(unzip -Z1 "${ZIP_PATH}")"
for p in "${REQUIRED_ZIP_ENTRIES[@]}"; do
  printf '%s\n' "${entries}" | grep -Fxq "${p}" || { echo "missing zip entry: ${p}"; exit 1; }
done

echo "[gate] 4/5 size guard"
ls -lh "${ZIP_PATH}"
size_bytes="$(stat -c%s "${ZIP_PATH}")"
if (( size_bytes > 25 * 1024 * 1024 )); then
  echo "zip too large: ${size_bytes} bytes (>25MB)"
  exit 1
fi

echo "[gate] 5/5 nested path guard"
if unzip -Z1 "${ZIP_PATH}" | grep -Eq "${NESTED_WINUI_REGEX}"; then
  echo "nested winui output detected in zip"
  exit 1
fi

echo "[gate] OK"
