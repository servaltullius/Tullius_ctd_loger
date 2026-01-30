# Tullius CTD Logger (SkyrimDiag) — Beta

This repository contains an MVP implementation of the design in:
- `doc/1.툴리우스_ctd_로거_개발명세서.md`
- `doc/2.코드골격참고.md`

## What’s included

- **SKSE plugin (DLL)**: shared-memory blackbox ringbuffer, main-thread heartbeat, passive crash mark (VEH)
  - Optional: recent resource load log (e.g. `.nif/.hkx/.tri`)
  - Optional: best-effort hitch/stutter signal (PerfHitch)
- **Helper (EXE)**: attach/monitor, hang detection, WCT capture, MiniDumpWriteDump with user streams (blackbox + WCT JSON)
- **DumpTool (EXE)**: reads `.dmp`, extracts SkyrimDiag user streams, writes a human-friendly summary + blackbox timeline

## Install (MO2)

- Install as a mod with:
  - `SKSE/Plugins/SkyrimDiag.dll`
  - `SKSE/Plugins/SkyrimDiag.ini`
  - `SKSE/Plugins/SkyrimDiagHelper.exe`
  - `SKSE/Plugins/SkyrimDiagHelper.ini`
  - `SKSE/Plugins/SkyrimDiagDumpTool.exe`
- Default behavior: launching SKSE will auto-start the helper (`AutoStartHelper=1` in `SkyrimDiag.ini`).

## Use

- Run Skyrim via SKSE as usual (MO2).
- Outputs:
  - Dumps/WCT/stats are written by the helper. Set `OutputDir` in `SkyrimDiagHelper.ini` for an easy-to-find folder.
- Crash hook behavior (in `SkyrimDiag.ini`):
  - `CrashHookMode=0` Off
  - `CrashHookMode=1` Fatal exceptions only (recommended; reduces false “Crash_*.dmp” during normal play/loading)
  - `CrashHookMode=2` All exceptions (can false-trigger; only if you understand the trade-off)
- Resource tracking (in `SkyrimDiag.ini`):
  - `EnableResourceLog=1` logs recent loads (e.g. `.nif/.hkx/.tri`) into the dump so the viewer can show “recent assets” and MO2 provider conflicts (best-effort).
- Performance hitch tracking (in `SkyrimDiag.ini`):
  - `EnablePerfHitchLog=1` records a `PerfHitch` event when the main-thread scheduled heartbeat runs late (best-effort).
  - Tuning: `PerfHitchThresholdMs`, `PerfHitchCooldownMs`
- Manual capture:
  - `Ctrl+Shift+F12` → immediately writes a dump + WCT JSON (스냅샷/문제상황 캡처용).
- Dump analysis (no WinDbg required):
  - Easiest (default): after a dump is written, the helper auto-runs the DumpTool and creates human-friendly files next to the dump.
    - Toggle in `SkyrimDiagHelper.ini`: `AutoAnalyzeDump=1`
  - Manual:
    - Drag-and-drop a `.dmp` onto `SkyrimDiagDumpTool.exe`, or double-click it to pick a dump file.
    - The DumpTool opens a viewer UI (tabs: summary/evidence/events/resources/WCT).
  - Output files:
    - `<stem>_SkyrimDiagSummary.json` (exception + module+offset, flags, etc.)
    - `<stem>_SkyrimDiagReport.txt` (quick human-readable report)
    - `<stem>_SkyrimDiagBlackbox.jsonl` (timeline of recent in-game events)

## Validate (optional)

For in-game validation without waiting:
- In `SkyrimDiag.ini`, set `EnableTestHotkeys=1`
  - `Ctrl+Shift+F10` → intentional crash (tests crash capture)
  - `Ctrl+Shift+F11` → intentional hang on the main thread (tests hang detection + WCT/dump)

## Beta testing

베타 배포 목적은 “원인 모드 특정”을 **최대한 유저 친화적으로** 돕는 것입니다. 다만 덤프 기반 추정은 본질적으로 한계가 있으므로,
리포트의 **신뢰도(높음/중간/낮음)** 표기를 참고해주세요.

- 베타 테스터 가이드: `docs/BETA_TESTING.md`

## Package (zip)

After building on Windows, create an MO2-friendly zip:
```powershell
python scripts/package.py --build-dir build-win --out dist/Tullius_ctd_loger.zip
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

Notes:
- This project uses the **CommonLibSSE-NG vcpkg port**. See `vcpkg-configuration.json`.
- Optional env vars for post-build copy:
  - `SKYRIM_FOLDER` → copies `SkyrimDiag.dll` + `SkyrimDiag.ini` into `Data/SKSE/Plugins`
  - `SKYRIM_MODS_FOLDER` → copies into `<mods>/<ProjectName>/SKSE/Plugins`
