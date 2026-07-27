#!/usr/bin/env bash
# Run clang-tidy over the production sources that are reachable on Linux.
#
#   scripts/run_clang_tidy.sh [build-dir] [clang-tidy-binary]
#
# The check set and WarningsAsErrors live in .clang-tidy, so this script only
# decides *what* gets linted. Scope notes:
#
#   - Test and fuzz translation units are excluded. They exist to exercise the
#     production code, and holding harness code to the same lint bar would only
#     add noise to the signal this gate is meant to carry.
#   - Windows-only sources (the SKSE plugin, the helper's capture path) never
#     appear in a Linux compile database, so they are not covered here. That is
#     a real gap, not a claim of coverage — the file list is printed so the gap
#     is visible in the CI log instead of being implied away.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${repo_root}/build-tidy}"
tidy_bin="${2:-clang-tidy}"

if ! command -v "${tidy_bin}" >/dev/null 2>&1; then
  echo "error: ${tidy_bin} not found on PATH" >&2
  exit 127
fi

if [[ ! -f "${build_dir}/compile_commands.json" ]]; then
  echo "==> configuring ${build_dir} (compile database)"
  cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
fi

mapfile -t sources < <(python3 - "${build_dir}/compile_commands.json" "${repo_root}" <<'PY'
import json
import pathlib
import sys

db_path, repo_root = sys.argv[1], pathlib.Path(sys.argv[2]).resolve()
included = ("dump_tool/", "helper/", "plugin/", "shared/")

seen = set()
for entry in json.loads(pathlib.Path(db_path).read_text(encoding="utf-8")):
    path = pathlib.Path(entry["file"])
    if not path.is_absolute():
        path = pathlib.Path(entry["directory"]) / path
    try:
        rel = path.resolve().relative_to(repo_root).as_posix()
    except ValueError:
        continue
    if rel.startswith(included):
        seen.add(rel)

# Sorted so the CI log diffs cleanly when the covered set changes.
for rel in sorted(seen):
    print(rel)
PY
)

if [[ ${#sources[@]} -eq 0 ]]; then
  echo "error: no production sources found in ${build_dir}/compile_commands.json" >&2
  exit 1
fi

echo "==> clang-tidy over ${#sources[@]} file(s)"
printf '  - %s\n' "${sources[@]}"

status=0
for source in "${sources[@]}"; do
  if ! "${tidy_bin}" -p "${build_dir}" --quiet "${repo_root}/${source}"; then
    status=1
  fi
done

if [[ ${status} -ne 0 ]]; then
  echo "==> clang-tidy FAILED" >&2
  exit 1
fi

echo "==> clang-tidy clean"
