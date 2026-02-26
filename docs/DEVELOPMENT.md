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
  - WinUI shell: `SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
  - Native analyzer: `SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll`

## Install (MO2) (Local Testing)

Install as a mod with:
- `SKSE/Plugins/SkyrimDiag.dll`
- `SKSE/Plugins/SkyrimDiag.ini`
- `SKSE/Plugins/SkyrimDiagHelper.exe`
- `SKSE/Plugins/SkyrimDiagHelper.ini`
- `SKSE/Plugins/SkyrimDiagDumpToolCli.exe`
- `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
- `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll`

Default behavior: launching SKSE will auto-start the helper (`AutoStartHelper=1` in `SkyrimDiag.ini`).

Runtime prerequisites for lightweight WinUI distribution:
- .NET Desktop Runtime 8 (x64): https://dotnet.microsoft.com/en-us/download/dotnet/8.0
- Windows App Runtime (1.8, x64): https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads
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

- Workflow: `.github/workflows/ci.yml`
- Scope: Linux smoke/unit tests for parser + hang suppression + i18n core + bucket + retention/config checks + XAML sanity
- Trigger: `push`, `pull_request`
- Manual Windows packaging job: `workflow_dispatch` (build + package zip artifact upload on `windows-2022`)

Equivalent local commands:
```bash
cmake -S . -B build-linux -G Ninja
cmake --build build-linux
ctest --test-dir build-linux --output-on-failure
```

## Issue Reporting / Troubleshooting

- Issue reporting guide: `docs/BETA_TESTING.md`
- MO2 WinUI smoke test checklist: `docs/MO2_WINUI_SMOKE_TEST_CHECKLIST.md`

## Package (zip)

After building on Windows, create an MO2-friendly zip:
```powershell
python scripts/package.py --build-dir build-win --out dist/Tullius_ctd_loger.zip --no-pdb
```

The packager requires WinUI publish output from `build-winui` (override path with `--winui-dir`) and includes both `SkyrimDiagDumpToolWinUI.exe` and `SkyrimDiagDumpToolNative.dll`.
It also packages `dump_tool/data` recursively (for both plugin path and WinUI path), so newly added data files do not require manual script edits.

## Release (GitHub)

Policy:
- GitHub Release patch notes are written in **Korean (required)**.
- English is optional, but put Korean first.

Suggested checklist:
1) Update version + changelog
2) Run tests (Linux + Windows)
3) Confirm compatibility preflight is required by default (`dist/SkyrimDiagHelper.ini` has `EnableCompatibilityPreflight=1`)
4) Build + package zip on Windows (`--no-pdb`)
5) Tag + push, then create GitHub Release and upload `dist/Tullius_ctd_loger.zip`

Release hard-gate quick checks:
```bash
# one-shot gate script (recommended)
bash scripts/verify_release_gate.sh

# 0) compatibility preflight default must stay enabled
grep -E '^EnableCompatibilityPreflight=1$' /home/kdw73/Tullius_ctd_loger/dist/SkyrimDiagHelper.ini

# 1) scripts sync (WSL repo <-> Windows mirror)
sha256sum /home/kdw73/Tullius_ctd_loger/scripts/build-winui.cmd /mnt/c/Users/kdw73/Tullius_ctd_loger/scripts/build-winui.cmd
sha256sum /home/kdw73/Tullius_ctd_loger/scripts/package.py /mnt/c/Users/kdw73/Tullius_ctd_loger/scripts/package.py

# 2) required WinUI outputs
ls /mnt/c/Users/kdw73/Tullius_ctd_loger/build-winui/{SkyrimDiagDumpToolWinUI.exe,SkyrimDiagDumpToolWinUI.pri,App.xbf,MainWindow.xbf}

# 3) zip required entries (authoritative list: scripts/release_contract.py)
python3 - <<'PY'
import subprocess
import sys

sys.path.insert(0, "/home/kdw73/Tullius_ctd_loger/scripts")
from release_contract import REQUIRED_ZIP_ENTRIES

zip_path = "/mnt/c/Users/kdw73/Tullius_ctd_loger/dist/Tullius_ctd_loger.zip"
entries = set(subprocess.check_output(["unzip", "-Z1", zip_path], text=True).splitlines())
missing = [entry for entry in REQUIRED_ZIP_ENTRIES if entry not in entries]
if missing:
    raise SystemExit("missing zip entries: " + ", ".join(missing))
print("zip required entries: OK")
PY

# 4) zip size guard (guide: 8MB ~ 25MB)
ls -lh /mnt/c/Users/kdw73/Tullius_ctd_loger/dist/Tullius_ctd_loger.zip

# 5) nested-path guard (must be empty; regex from scripts/release_contract.py)
python3 - <<'PY'
import re
import subprocess
import sys

sys.path.insert(0, "/home/kdw73/Tullius_ctd_loger/scripts")
from release_contract import nested_winui_path_regex

zip_path = "/mnt/c/Users/kdw73/Tullius_ctd_loger/dist/Tullius_ctd_loger.zip"
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

Build modern WinUI viewer output (framework-dependent / lightweight):
```powershell
scripts\\build-winui.cmd
```

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
