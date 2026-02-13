# Tullius CTD Logger (SkyrimDiag)

[![Latest Release](https://img.shields.io/github/v/release/servaltullius/Tullius_ctd_loger)](https://github.com/servaltullius/Tullius_ctd_loger/releases/latest)

> [**한국어 안내 →**](docs/README_KO.md)

A best-effort diagnostics tool for **Skyrim SE / AE** that captures **CTD, freezes, and infinite loading screens**, then produces a readable report (summary + evidence + checklist) — no WinDbg required.

- **Not a crash-prevention mod.** It records signals and captures evidence; it does not swallow exceptions or attempt to keep playing.
- **No uploads / telemetry.** All output is local. Online symbol downloads are OFF by default (`AllowOnlineSymbols=0`).
- **CrashLoggerSSE integration** — auto-detects `crash-*.log` / `threaddump-*.log` and surfaces top callstack modules, C++ exception blocks, and CrashLogger version.

## Components

| Component | File | Role |
|-----------|------|------|
| SKSE Plugin | `SkyrimDiag.dll` | Black-box event/state recording, heartbeat, optional resource (.nif/.hkx/.tri) logging |
| Helper | `SkyrimDiagHelper.exe` | Out-of-proc — attaches to game, detects freeze/ILS, captures dump + WCT |
| CLI Analyzer | `SkyrimDiagDumpToolCli.exe` | Headless analysis (no window) — used by Helper for auto-analysis |
| WinUI Viewer | `SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe` | Interactive viewer: summary / evidence / events / resources / WCT |
| Native Engine | `SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll` | Native analyzer backing the WinUI viewer |

## Requirements

- **Skyrim SE / AE** (Windows)
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- WinUI viewer runtimes:
  - [.NET Desktop Runtime 8 (x64)](https://dotnet.microsoft.com/en-us/download/dotnet/8.0)
  - [Windows App Runtime 1.8 (x64)](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads)
  - [Visual C++ Redistributable 2015-2022 (x64)](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)
- Optional (recommended): [Crash Logger SSE AE VR — PDB support](https://www.nexusmods.com/skyrimspecialedition/mods/59818)

## Quick Start

1. Install the requirements above
2. Download the latest release zip and install it as a mod in **MO2 / Vortex**
3. Launch Skyrim via **SKSE**
4. When a crash/freeze/ILS happens, a dump (`.dmp`) + report are generated
5. Open the `.dmp` with `SkyrimDiagDumpToolWinUI.exe` (drag & drop)

**Manual snapshot hotkey:** `Ctrl+Shift+F12`
> Snapshots taken during normal gameplay may have low confidence. Best used when the game is already stuck (freeze / ILS) or right before a CTD.

## Output Location

- By default: MO2 `overwrite\SKSE\Plugins\`
- To redirect: set `OutputDir=` in `SkyrimDiagHelper.ini`

## Capture Methods

| Situation | Dump file pattern | Trigger |
|-----------|------------------|---------|
| CTD (game crash) | `*_Crash_*.dmp` | Automatic |
| Freeze / ILS | `*_Hang_*.dmp` | Automatic (threshold-based) |
| Manual snapshot | `*_Manual_*.dmp` | `Ctrl+Shift+F12` |

Hang detection thresholds are configurable in `SkyrimDiagHelper.ini`:
- `HangThresholdInGameSec` / `HangThresholdLoadingSec`
- `EnableAdaptiveLoadingThreshold=1` (recommended — auto-learns loading times)

## Language

The WinUI viewer follows your Windows UI language by default. Override via command line:
```
SkyrimDiagDumpToolWinUI.exe --lang ko   # Korean
SkyrimDiagDumpToolWinUI.exe --lang en   # English
```

## Performance

Designed with minimal overhead — **event recording + on-demand dump capture**, not continuous FPS measurement or constant stack walking.

Options that may have marginal impact on heavy modpacks:

| Option | Default | Note |
|--------|---------|------|
| `EnableResourceLog` | `1` | Hooks loose file opens (.nif/.hkx/.tri). Disable first if you suspect overhead. |
| `EnablePerfHitchLog` | `1` | Logs main-thread stalls (lightweight). |
| `CrashHookMode` | `1` | **Keep at 1.** Mode 2 (all exceptions) is not recommended. |
| `AllowOnlineSymbols` | `0` | Offline/local cache analysis by default. |

## Retention / Disk Cleanup

Helper auto-cleans old dumps and artifacts via `SkyrimDiagHelper.ini`:
- `MaxCrashDumps`, `MaxHangDumps`, `MaxManualDumps`, `MaxEtwTraces` (0 = unlimited)
- `MaxHelperLogBytes`, `MaxHelperLogFiles` (log rotation)

## Issue Reporting

See [`docs/BETA_TESTING.md`](docs/BETA_TESTING.md) for the full guide.

**Minimum attachments:**
- The `.dmp` file
- `*_SkyrimDiagReport.txt` and `*_SkyrimDiagSummary.json`
- `SkyrimDiag_Incident_*.json`
- (if available) `*_SkyrimDiagNativeException.log`, `*_SkyrimDiagBlackbox.jsonl`, `SkyrimDiag_WCT_*.json`, ETL traces
- (if available) CrashLogger `crash-*.log` / `threaddump-*.log`

> **Privacy:** Dumps and external logs may contain PC paths (drive letters, usernames). Review/mask before public upload.

## Development

See [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).
