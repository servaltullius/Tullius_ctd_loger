# Development (Contributors)

For end-users (player-facing docs), start here:
- `README.md` (Korean main)
- `docs/README_KO.md` (Korean expanded)
- `docs/BETA_TESTING.md` (issue reporting guide)

This repository contains an MVP implementation of the design in:
- `doc/1.툴리우스_ctd_로거_개발명세서.md`
- `doc/2.코드골격참고.md`

## What's Included

- SKSE plugin (DLL): shared-memory blackbox ringbuffer, main-thread heartbeat, passive crash mark (VEH)
  - Optional: recent resource load log (e.g. `.nif/.hkx/.tri`)
  - Optional: best-effort hitch/stutter signal (PerfHitch)
- Helper (EXE): attach/monitor, hang detection, WCT capture, MiniDumpWriteDump with user streams (blackbox + WCT JSON)
- DumpTool (CLI + WinUI + native analyzer DLL):
  - Headless CLI: `SkyrimDiagDumpToolCli.exe`
  - WinUI launcher: `SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
  - Self-contained WinUI app/runtime/native analyzer: `SkyrimDiagWinUI/app/`

## Install (MO2) (Local Testing)

Install as a mod with:
- `SKSE/Plugins/SkyrimDiag.dll`
- `SKSE/Plugins/SkyrimDiag.ini`
- `SKSE/Plugins/SkyrimDiagHelper.exe`
- `SKSE/Plugins/SkyrimDiagHelper.ini`
- `SKSE/Plugins/SkyrimDiagDumpToolCli.exe`
- `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
- `SKSE/Plugins/SkyrimDiagWinUI/app/SkyrimDiagDumpToolWinUI.exe`
- `SKSE/Plugins/SkyrimDiagWinUI/app/SkyrimDiagDumpToolNative.dll`

Default behavior: launching SKSE will auto-start the helper (`AutoStartHelper=1` in `SkyrimDiag.ini`).

Runtime prerequisites for release distribution:
- WinUI release builds are self-contained (`v0.2.52+`). In `v0.2.53+`, the easy-to-find top-level `SkyrimDiagDumpToolWinUI.exe` is a native launcher and the .NET + Windows App SDK files live under `SkyrimDiagWinUI/app/`.
- Microsoft Visual C++ Redistributable 2015-2022 (x64): https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist

## Use (For Testing)

- Outputs:
  - Dumps/WCT/stats are written by the helper. Set `OutputDir` in `SkyrimDiagHelper.ini` for an easy-to-find folder.
  - Startup preflight output: `SkyrimDiag_Preflight.json` (`EnableCompatibilityPreflight=1`)
  - Dump-failure fallback hint: `SkyrimDiag_WER_LocalDumps_Hint.txt` (`EnableWerDumpFallbackHint=1`)
- Manual capture:
  - `Ctrl+Shift+F12` writes a dump + WCT JSON (snapshot / capture evidence during a problematic moment).
- Dump analysis (no WinDbg required):
  - Helper can auto-run analysis after a dump is written (`AutoAnalyzeDump=1` in `SkyrimDiagHelper.ini`).
  - Viewer auto-open policy is configured via `SkyrimDiagHelper.ini` (see `dist/SkyrimDiagHelper.ini` for defaults).

DumpTool language:
- Headless CLI: `SkyrimDiagDumpToolCli.exe --lang en|ko <dump> [--out-dir <dir>]`
- WinUI: `SkyrimDiagDumpToolWinUI.exe --lang en|ko <dump>`
  - Compatibility flags accepted: `--simple-ui`, `--advanced-ui`

## Validate (Optional)

For in-game validation without waiting:
- In `SkyrimDiag.ini`, set `EnableTestHotkeys=1`
  - `Ctrl+Shift+F10` intentional crash (tests crash capture)
  - `Ctrl+Shift+F11` intentional hang on the main thread (tests hang detection + WCT/dump)

## CI (GitHub Actions)

- Local verification is the release source of truth for this repository.
- GitHub Actions is optional/reference only and should not be the sole release gate.
- Main workflow: `.github/workflows/ci.yml`
- Main workflow scope: Linux tests, Windows build/package/gate, complete Windows production clang-tidy coverage, extracted-package launcher smoke, and repo guard checks
- Tag-triggered releases rerun Linux unit, ASan+UBSan, the Linux clang-tidy subset, parser fuzz, complete Windows production clang-tidy, and the packaged launcher smoke before publication.
- Manual rerun of the same packaged WinUI smoke: `.github/workflows/winui-headless-smoke.yml`
- Manual smoke trigger: `workflow_dispatch`

Equivalent local commands:
```bash
cmake -S . -B build-linux -G Ninja
cmake --build build-linux
ctest --test-dir build-linux --output-on-failure
```

The CI-only checks below are not part of the default build, so run them locally
before pushing changes to the analyzer sources:
```bash
# Fast Linux static-analysis subset. The printed list contains only production
# sources reachable in the Linux compile database; it is intentionally not the
# complete Windows product claim.
bash scripts/run_clang_tidy.sh

# Parser fuzz smoke. The crash-log parsers read files written by other mods, so
# they take the least trusted input in the project.
cmake -S . -B build-fuzz -G Ninja -DSKYDIAG_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz --target fuzz_crashlogger_parser fuzz_wct_parser
# The scratch directory goes first: libFuzzer writes new units into the first
# corpus argument, so this keeps the checked-in seeds curated.
mkdir -p /tmp/fuzz-crashlogger /tmp/fuzz-wct
./build-fuzz/bin/fuzz_crashlogger_parser /tmp/fuzz-crashlogger fuzz/corpus/crashlogger -max_total_time=90
./build-fuzz/bin/fuzz_wct_parser /tmp/fuzz-wct fuzz/corpus/wct -max_total_time=90
```

After `scripts\build-win.cmd` on Windows, require that the Ninja compile database
contains every production `.cpp` under `dump_tool/src`, `helper/src`, and
`plugin/src` (plus the generated SKSE metadata source), then lint that full set:

```powershell
python scripts/run_clang_tidy.py `
  --build-dir build-win `
  --clang-tidy "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-tidy.exe" `
  --jobs 4
```

Recommended release-time local verification bundle:
```bash
cmake -S . -B build-linux-test -G Ninja
cmake --build build-linux-test
ctest --test-dir build-linux-test --output-on-failure
bash scripts/build-win-from-wsl.sh
bash scripts/build-winui-from-wsl.sh
python3 scripts/package.py --build-dir build-win --config RelWithDebInfo --winui-dir build-winui --no-pdb
bash scripts/verify_release_gate.sh
```

With no third argument, the gate resolves exactly
`dist/Tullius_ctd_loger_v<project-version>.zip`; it never chooses the newest
matching file by mtime. For an RC name, pass the exact ZIP path as argument 3.

The hard gate rejects PDBs and non-x64 key executables, requires the self-contained
Windows App SDK runtime files, reads the actual PE `FileVersion`/`ProductVersion`
and SKSE `PluginDeclaration` from the ZIP, and verifies byte-for-byte identity
against one exact native configuration and the flat `build-winui` output.
It also validates commit-bound native, WinUI, and package provenance before
extracting the ZIP to a fresh directory and analyzing a valid Windows minidump
through the packaged top-level WinUI launcher. Exit 0 plus an identity-matched
report/Summary pair is required.

Dirty source trees remain packageable for local diagnosis, but public artifacts
must pass with clean provenance:

```bash
SKYDIAG_REQUIRE_CLEAN_PROVENANCE=1 \
SKYDIAG_RELEASE_CONFIG=RelWithDebInfo \
bash scripts/verify_release_gate.sh "$PWD" "$PWD" "$PWD/dist/Tullius_ctd_loger_v<version-or-rc>.zip"
```

Windows-only synthetic helper runtime trigger checks:
- Purpose: verify that CTD and freeze paths trigger the helper, and that normal exit / weak crash paths do not misfire.
- Scope: game-off helper/runtime behavior only. This is not an analysis-quality regression suite.
- Run the three Windows tests sequentially after `build-win` succeeds:

```powershell
build-win\bin\skydiag_helper_runtime_smoke_tests.exe
build-win\bin\skydiag_helper_false_positive_runtime_tests.exe
build-win\bin\skydiag_helper_hang_runtime_tests.exe
```

If launching from WSL, prefer Windows PowerShell `Start-Process -Wait -PassThru` and run them one at a time:

```bash
SKYDIAG_REPO="$(wslpath -w "$PWD")" /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -ExecutionPolicy Bypass -Command '$tests = @(
  (Join-Path $env:SKYDIAG_REPO "build-win\bin\skydiag_helper_runtime_smoke_tests.exe"),
  (Join-Path $env:SKYDIAG_REPO "build-win\bin\skydiag_helper_false_positive_runtime_tests.exe"),
  (Join-Path $env:SKYDIAG_REPO "build-win\bin\skydiag_helper_hang_runtime_tests.exe")
); foreach ($test in $tests) { $p = Start-Process -FilePath $test -Wait -PassThru -NoNewWindow; if ($p.ExitCode -ne 0) { exit $p.ExitCode } }'
```

## Analysis Quality Corpus Gate

CTest builds `skydiag_quality_corpus_runner`, feeds its raw signal fixtures through the production `BuildCandidateConsensus()` implementation, and scores only the generated temporary summaries. This pins deterministic candidate-consensus behavior without allowing stale precomputed predictions to pass.

Synthetic fixtures do not establish real-world CTD-cause accuracy. Use reviewed, real incident summaries with `triage.ground_truth_mod` populated to measure the same `actionable_candidates` ordering that users see:

```powershell
python scripts/analyze_bucket_quality.py `
  --root <reviewed-summary-directory> `
  --out-json build/analysis-quality.json `
  --min-ground-truth <required-sample-count> `
  --min-high-confidence-predictions <required-high-sample-count> `
  --min-top1-accuracy <0-to-1> `
  --min-top3-recall <0-to-1> `
  --min-high-confidence-precision <0-to-1> `
  --max-abstention-rate <0-to-1>
```

The command exits with code `2` when a configured threshold is missed or when a requested metric has no eligible samples. Do not publish an accuracy percentage from synthetic fixtures or from an unreviewed corpus; choose release thresholds only after the corpus size and labeling policy have been recorded.

To attach a reviewed corpus to the release hard gate, configure the corpus and every threshold before running it:

```bash
export SKYDIAG_QUALITY_CORPUS=<reviewed-summary-directory>
export SKYDIAG_QUALITY_MIN_GROUND_TRUTH=<required-sample-count>
export SKYDIAG_QUALITY_MIN_HIGH_CONFIDENCE_PREDICTIONS=<required-high-sample-count>
export SKYDIAG_QUALITY_MIN_TOP1_ACCURACY=<0-to-1>
export SKYDIAG_QUALITY_MIN_TOP3_RECALL=<0-to-1>
export SKYDIAG_QUALITY_MIN_HIGH_CONFIDENCE_PRECISION=<0-to-1>
export SKYDIAG_QUALITY_MAX_ABSTENTION_RATE=<0-to-1>
bash scripts/verify_release_gate.sh
```

When `SKYDIAG_QUALITY_CORPUS` is not set, the gate reports this step as `SKIPPED (not measured)` rather than claiming that real-world accuracy passed. When a corpus is set, omitting any threshold is a hard failure.

## Issue Reporting / Troubleshooting

- Issue reporting guide: `docs/BETA_TESTING.md`
- MO2 WinUI smoke test checklist: `docs/MO2_WINUI_SMOKE_TEST_CHECKLIST.md`

## Package (zip)

After building on Windows, create an MO2-friendly zip:
```powershell
python scripts/package.py --build-dir build-win --no-pdb
```

The packager requires self-contained WinUI publish output from `build-winui` (override path with `--winui-dir`) and includes a top-level WinUI launcher plus the real WinUI app/runtime under `SKSE/Plugins/SkyrimDiagWinUI/app/`.
Because the WinUI viewer is self-contained, the zip intentionally includes many .NET and Windows App SDK sidecar files under the `app` subfolder.
It also packages `dump_tool/data` recursively (for both plugin path and WinUI path), so newly added data files do not require manual script edits.
`scripts\build-win.cmd` and `scripts\build-winui.cmd` write build provenance
manifests containing HEAD, dirty state, a source-tree fingerprint, exact
configuration, and artifact hashes. `package.py` refuses stale/mismatched
manifests and records a package manifest covering every ZIP payload file.
`build-winui.cmd` publishes into one freshly cleared staging directory, validates
that directory only, writes provenance there, and replaces `build-winui`; it
does not search older `bin/.../publish` candidates.

## Release (GitHub)

Policy:
- GitHub Release patch notes are written in **Korean (required)**.
- English is optional, but put Korean first.
- Use the canonical prerelease notes template:
  - `docs/release/PRERELEASE_NOTES_TEMPLATE.md`

Suggested checklist:
1) Update version + changelog
2) Run the local verification bundle (Linux tests + Windows build/package/gate). Do not block prerelease solely on GitHub Actions.
3) Confirm compatibility preflight is required by default (`dist/SkyrimDiagHelper.ini` has `EnableCompatibilityPreflight=1`)
4) Build + package zip on Windows (`--no-pdb`)
5) Copy the template to a versioned draft and fill it in
6) Confirm the exact ZIP passes the hard gate with `SKYDIAG_REQUIRE_CLEAN_PROVENANCE=1`
7) Tag + push, then create or edit GitHub Release with `--notes-file`
8) Upload `dist/Tullius_ctd_loger_v<version>.zip`

Suggested release-notes flow:
```bash
mkdir -p docs/release/drafts
cp docs/release/PRERELEASE_NOTES_TEMPLATE.md docs/release/drafts/v0.2.42-rcN.md
# edit docs/release/drafts/v0.2.42-rcN.md

gh release create v0.2.42-rcN dist/Tullius_ctd_loger_v0.2.42-rcN.zip \
  --prerelease \
  --title "v0.2.42-rcN — <요약 제목>" \
  --notes-file docs/release/drafts/v0.2.42-rcN.md

# or update an existing prerelease body
gh release edit v0.2.42-rcN \
  --title "v0.2.42-rcN — <요약 제목>" \
  --notes-file docs/release/drafts/v0.2.42-rcN.md
```

Release hard-gate quick checks:
```bash
# one-shot gate script (recommended)
bash scripts/verify_release_gate.sh

# 0) compatibility preflight default must stay enabled
grep -E '^EnableCompatibilityPreflight=1$' dist/SkyrimDiagHelper.ini

# 1) scripts sync (WSL repo <-> Windows mirror)
# Compare the WSL repo copy against the Windows-side mirror (adjust paths as needed)
sha256sum scripts/build-winui.cmd /mnt/c/Users/$USER/Tullius_ctd_loger/scripts/build-winui.cmd
sha256sum scripts/package.py /mnt/c/Users/$USER/Tullius_ctd_loger/scripts/package.py

# 2) required WinUI outputs
ls build-winui/{SkyrimDiagDumpToolWinUI.exe,SkyrimDiagDumpToolWinUI.pri,App.xbf,MainWindow.xbf}

# 3) zip required entries (authoritative list: scripts/release_contract.py)
python3 - <<'PY'
import subprocess
import sys

sys.path.insert(0, "scripts")
from release_contract import REQUIRED_ZIP_ENTRIES

zip_path = "dist/Tullius_ctd_loger_v<version>.zip"
entries = set(subprocess.check_output(["unzip", "-Z1", zip_path], text=True).splitlines())
missing = [entry for entry in REQUIRED_ZIP_ENTRIES if entry not in entries]
if missing:
    raise SystemExit("missing zip entries: " + ", ".join(missing))
print("zip required entries: OK")
PY

# 4) zip size guard (guide: self-contained WinUI release zips are normally tens of MB; hard gate is 100MB)
ls -lh dist/Tullius_ctd_loger_v<version>.zip

# 5) nested-path guard (must be empty; regex from scripts/release_contract.py)
python3 - <<'PY'
import re
import subprocess
import sys

sys.path.insert(0, "scripts")
from release_contract import nested_winui_path_regex

zip_path = "dist/Tullius_ctd_loger_v<version>.zip"
pattern = re.compile(nested_winui_path_regex())
bad = [line for line in subprocess.check_output(["unzip", "-Z1", zip_path], text=True).splitlines() if pattern.match(line)]
if bad:
    raise SystemExit("nested winui output detected: " + ", ".join(bad))
print("nested-path guard: OK")
PY
```

## Build (Windows)

Prereqs:
- Visual Studio 2022 (C++ Desktop)
- `vcpkg` and `VCPKG_ROOT` env var set

Configure + build:
```powershell
cmake -S . -B build --preset default
cmake --build build --preset default
```

Build modern WinUI viewer output (self-contained release publish):
```powershell
scripts\\build-winui.cmd
```

WSL entry points:
```bash
bash scripts/build-win-from-wsl.sh
bash scripts/build-winui-from-wsl.sh
```

Notes:
- The WSL wrappers convert the sibling batch path with `wslpath -w` and invoke Windows PowerShell with an absolute Windows path.
- Relative launches such as `cmd.exe /c scripts\\build-win.cmd` are not supported from WSL because `cmd.exe` falls back from the UNC current directory before it can resolve the relative script path.

Notes:
- This project uses the CommonLibSSE-NG vcpkg port. See `vcpkg-configuration.json`.
- Optional env vars for post-build copy:
  - `SKYRIM_FOLDER` copies `SkyrimDiag.dll` + `SkyrimDiag.ini` into `Data/SKSE/Plugins`
  - `SKYRIM_MODS_FOLDER` copies into `<mods>/<ProjectName>/SKSE/Plugins`

## Dependency Refresh (Recommended)

For stability on new Skyrim runtimes, run a periodic dependency refresh cycle (for example, monthly):

1) Update vcpkg baselines in `vcpkg-configuration.json` and dependency versions in `vcpkg.json`.
2) Rebuild Windows targets (`scripts\\build-win.cmd`) and run Linux tests (`ctest --test-dir build-linux-test --output-on-failure`).
3) Validate package/release gates (`bash scripts/verify_release_gate.sh`) before shipping.
