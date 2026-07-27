#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

REPO_ROOT="${1:-${DEFAULT_REPO_ROOT}}"
WIN_ROOT="${2:-${REPO_ROOT}}"
ZIP_PATH="${3:-}"
PYTHON_BIN="$(command -v python3 || command -v python || true)"

if [[ -z "${PYTHON_BIN}" ]]; then
  echo "python interpreter not found (python3/python required)"
  exit 1
fi

RELEASE_ZIP_GLOB="$({
  PYTHONPATH="${REPO_ROOT}/scripts" "${PYTHON_BIN}" - <<'PY'
from release_contract import release_zip_glob

print(release_zip_glob())
PY
})"
RELEASE_ZIP_GLOB="${RELEASE_ZIP_GLOB%$'\r'}"

readarray -t REQUIRED_WINUI_BUILD_OUTPUTS < <(
  PYTHONPATH="${REPO_ROOT}/scripts" "${PYTHON_BIN}" - <<'PY'
from release_contract import REQUIRED_WINUI_BUILD_OUTPUTS

for item in REQUIRED_WINUI_BUILD_OUTPUTS:
    print(item)
PY
)
for i in "${!REQUIRED_WINUI_BUILD_OUTPUTS[@]}"; do
  REQUIRED_WINUI_BUILD_OUTPUTS[$i]="${REQUIRED_WINUI_BUILD_OUTPUTS[$i]%$'\r'}"
done

WINUI_BUILD_ROOT="$({
  PYTHONPATH="${REPO_ROOT}/scripts" "${PYTHON_BIN}" - "${WIN_ROOT}/build-winui" <<'PY'
from pathlib import Path
import sys

from release_contract import find_winui_build_root

root = find_winui_build_root(Path(sys.argv[1]))
if root is not None:
    print(root)
PY
})"
WINUI_BUILD_ROOT="${WINUI_BUILD_ROOT%$'\r'}"

if [[ -n "${WINUI_BUILD_ROOT}" ]] && command -v cygpath >/dev/null 2>&1; then
  WINUI_BUILD_ROOT="$(cygpath -u "${WINUI_BUILD_ROOT}")"
fi

readarray -t REQUIRED_ZIP_ENTRIES < <(
  PYTHONPATH="${REPO_ROOT}/scripts" "${PYTHON_BIN}" - <<'PY'
from release_contract import REQUIRED_ZIP_ENTRIES

for item in REQUIRED_ZIP_ENTRIES:
    print(item)
PY
)
for i in "${!REQUIRED_ZIP_ENTRIES[@]}"; do
  REQUIRED_ZIP_ENTRIES[$i]="${REQUIRED_ZIP_ENTRIES[$i]%$'\r'}"
done

NESTED_WINUI_REGEX="$({
  PYTHONPATH="${REPO_ROOT}/scripts" "${PYTHON_BIN}" - <<'PY'
from release_contract import nested_winui_path_regex

print(nested_winui_path_regex())
PY
})"
NESTED_WINUI_REGEX="${NESTED_WINUI_REGEX%$'\r'}"

resolve_zip_path() {
  local dist_dir="$1"
  local zip_glob="$2"
  local latest=""
  local candidate

  shopt -s nullglob
  for candidate in "${dist_dir}"/${zip_glob}; do
    if [[ -z "${latest}" || "${candidate}" -nt "${latest}" ]]; then
      latest="${candidate}"
    fi
  done
  shopt -u nullglob

  if [[ -n "${latest}" ]]; then
    printf '%s\n' "${latest}"
    return 0
  fi

  return 1
}

if [[ -z "${ZIP_PATH}" ]]; then
  ZIP_PATH="$(resolve_zip_path "${WIN_ROOT}/dist" "${RELEASE_ZIP_GLOB}" || true)"
fi

if [[ -n "${ZIP_PATH}" ]] && command -v cygpath >/dev/null 2>&1 && [[ "${ZIP_PATH}" =~ ^[A-Za-z]:\\ ]]; then
  ZIP_PATH="$(cygpath -u "${ZIP_PATH}")"
fi

if [[ -z "${ZIP_PATH}" || ! -f "${ZIP_PATH}" ]]; then
  echo "missing versioned release zip under: ${WIN_ROOT}/dist (expected ${RELEASE_ZIP_GLOB})"
  exit 1
fi

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
echo "[gate] zip=${ZIP_PATH}"

echo "[gate] 1/7 script sync hashes"
if [[ "${REPO_ROOT}" == "${WIN_ROOT}" ]]; then
  echo "  - same root path; sync hash comparison skipped"
else
  sync_files=(
    "scripts/build-win.cmd"
    "scripts/build-winui.cmd"
    "scripts/package.py"
    "scripts/release_contract.py"
    "scripts/verify_release_zip.py"
    "scripts/analyze_bucket_quality.py"
    "scripts/verify_release_gate.sh"
  )
  for rel in "${sync_files[@]}"; do
    assert_synced "${REPO_ROOT}/${rel}" "${WIN_ROOT}/${rel}" "${rel}"
  done
fi

echo "[gate] 2/7 required WinUI files"
if [[ -z "${WINUI_BUILD_ROOT}" ]]; then
  echo "missing WinUI publish root under: ${WIN_ROOT}/build-winui"
  exit 1
fi
for asset in "${REQUIRED_WINUI_BUILD_OUTPUTS[@]}"; do
  f="${WINUI_BUILD_ROOT}/${asset}"
  [[ -f "${f}" ]] || { echo "missing: ${f}"; exit 1; }
done

echo "[gate] 3/7 required zip entries"
entries="$(unzip -Z1 "${ZIP_PATH}")"
for p in "${REQUIRED_ZIP_ENTRIES[@]}"; do
  printf '%s\n' "${entries}" | grep -Fxq "${p}" || { echo "missing zip entry: ${p}"; exit 1; }
done

echo "[gate] 4/7 size guard"
ls -lh "${ZIP_PATH}"
size_bytes="$(stat -c%s "${ZIP_PATH}")"
if (( size_bytes > 100 * 1024 * 1024 )); then
  echo "zip too large: ${size_bytes} bytes (>100MB)"
  exit 1
fi

echo "[gate] 5/7 nested path guard"
if unzip -Z1 "${ZIP_PATH}" | grep -Eq "${NESTED_WINUI_REGEX}"; then
  echo "nested winui output detected in zip"
  exit 1
fi

echo "[gate] 6/7 version, x64, no-PDB, and current-build match"
"${PYTHON_BIN}" "${REPO_ROOT}/scripts/verify_release_zip.py" \
  --repo-root "${REPO_ROOT}" \
  --zip "${ZIP_PATH}" \
  --build-dir "${WIN_ROOT}/build-win" \
  --winui-dir "${WINUI_BUILD_ROOT}"

echo "[gate] 7/7 reviewed-corpus analysis quality"

# This step measures accuracy against reviewed real incidents. Hand-authored
# fixtures cannot substitute for that, so without a real corpus the honest report
# is "not measured" -- never a pass. Behavior regressions are covered separately
# by skydiag_quality_corpus_gate_tests in ctest, which is a different claim:
# "the analyzer still decides what it was designed to decide", not "the analyzer
# is accurate on real crashes".
QUALITY_CORPUS="${SKYDIAG_QUALITY_CORPUS:-}"
if [[ -n "${QUALITY_CORPUS}" ]]; then
  # An external corpus has unknown characteristics, so every threshold must be
  # stated explicitly.
  quality_vars=(
    SKYDIAG_QUALITY_MIN_GROUND_TRUTH
    SKYDIAG_QUALITY_MIN_HIGH_CONFIDENCE_PREDICTIONS
    SKYDIAG_QUALITY_MIN_TOP1_ACCURACY
    SKYDIAG_QUALITY_MIN_TOP3_RECALL
    SKYDIAG_QUALITY_MIN_HIGH_CONFIDENCE_PRECISION
    SKYDIAG_QUALITY_MAX_ABSTENTION_RATE
  )
  for name in "${quality_vars[@]}"; do
    if [[ -z "$(printenv "${name}" || true)" ]]; then
      echo "quality corpus is configured but required threshold is missing: ${name}"
      exit 1
    fi
  done
  echo "  - corpus=${QUALITY_CORPUS} (external, thresholds from environment)"
  QUALITY_REPORT="${SKYDIAG_QUALITY_REPORT:-${WIN_ROOT}/build/analysis-quality.json}"
  mkdir -p "$(dirname "${QUALITY_REPORT}")"
  "${PYTHON_BIN}" "${REPO_ROOT}/scripts/analyze_bucket_quality.py" \
    --root "${QUALITY_CORPUS}" \
    --out-json "${QUALITY_REPORT}" \
    --min-ground-truth "${SKYDIAG_QUALITY_MIN_GROUND_TRUTH}" \
    --min-high-confidence-predictions "${SKYDIAG_QUALITY_MIN_HIGH_CONFIDENCE_PREDICTIONS}" \
    --min-top1-accuracy "${SKYDIAG_QUALITY_MIN_TOP1_ACCURACY}" \
    --min-top3-recall "${SKYDIAG_QUALITY_MIN_TOP3_RECALL}" \
    --min-high-confidence-precision "${SKYDIAG_QUALITY_MIN_HIGH_CONFIDENCE_PRECISION}" \
    --max-abstention-rate "${SKYDIAG_QUALITY_MAX_ABSTENTION_RATE}"
else
  echo "  - SKIPPED (not measured; SKYDIAG_QUALITY_CORPUS is not set)"
  echo "    Attribution accuracy on real incidents is unverified for this release."
  echo "    See tests/data/quality_corpus/README.md to configure a reviewed corpus."
  echo "    (Behavior regressions are covered separately by skydiag_quality_corpus_gate_tests.)"
fi

echo "[gate] OK"
