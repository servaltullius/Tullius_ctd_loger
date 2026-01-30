# SkyrimDiag (Tullius CTD Logger) — MVP

This repository contains an MVP implementation of the design in:
- `doc/1.툴리우스_ctd_로거_개발명세서.md`
- `doc/2.코드골격참고.md`

## What’s included (MVP A)

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
  - `Ctrl+Shift+F12` → immediately writes a dump + WCT JSON (then the helper exits).
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

## Package (zip)

After building on Windows, create an MO2-friendly zip:
```powershell
python scripts/package.py --build-dir build-win --out dist/SkyrimDiag.zip
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
