# Tullius CTD Logger (SkyrimDiag)

[![Latest Release](https://img.shields.io/github/v/release/servaltullius/Tullius_ctd_loger)](https://github.com/servaltullius/Tullius_ctd_loger/releases/latest)

> [**한국어 안내 →**](docs/README_KO.md)

A best-effort diagnostics tool for **Skyrim SE / AE** that captures **CTD, freezes, and infinite loading screens**, then produces a readable report (summary + evidence + checklist) — no WinDbg required.

- **Not a crash-prevention mod.** It records signals and captures evidence; it does not swallow exceptions or attempt to keep playing.
- **No uploads / telemetry.** All output is local. Online symbol downloads are OFF by default (`AllowOnlineSymbols=0`).
- **CrashLoggerSSE integration** — auto-detects timestamped `crash-*.log` / `threaddump-*.log` artifacts, including the current v1.24 format, and surfaces top callstack modules, C++ exception blocks, and CrashLogger version. A normal runtime `CrashLogger.log` without a `Callstack:` section is not treated as crash evidence.

## Components

| Component | File | Role |
|-----------|------|------|
| SKSE Plugin | `SkyrimDiag.dll` | Black-box event/state recording, heartbeat, optional resource (.nif/.hkx/.tri) logging |
| Helper | `SkyrimDiagHelper.exe` | Out-of-proc — attaches to game, detects freeze/ILS, captures dump + WCT |
| CLI Analyzer | `SkyrimDiagDumpToolCli.exe` | Headless analysis (no window) — used by Helper for auto-analysis |
| WinUI Viewer | `SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe` | Easy-to-find launcher for the interactive viewer |
| WinUI App Runtime | `SkyrimDiagWinUI/app/` | Self-contained viewer app, runtime files, native analyzer, and data |

## Requirements

- **Skyrim SE / AE** (Windows)
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- WinUI viewer runtime:
  - The release zip includes a self-contained WinUI viewer (`v0.2.52+`), so users do **not** need to install .NET Desktop Runtime 8 or Windows App Runtime 1.8 separately.
  - The many files under `SkyrimDiagWinUI\\app` are the bundled .NET / Windows App SDK runtime. Do **not** delete individual files or copy only the viewer EXE. When updating, remove the old `SkyrimDiagWinUI` folder and install the complete release zip.
  - [Visual C++ Redistributable 2015-2022 (x64)](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)
- Optional (recommended): [Crash Logger SSE AE VR — PDB support](https://www.nexusmods.com/skyrimspecialedition/mods/59818)

## Quick Start

1. Install the requirements above
2. Download the latest release zip and install it as a mod in **MO2 / Vortex**
3. Launch Skyrim via **SKSE**
4. When a crash/freeze/ILS happens, a dump (`.dmp`) + report are generated
5. Open `SkyrimDiagDumpToolWinUI.exe`, click `Select dump`, and analyze the `.dmp`
   - Or launch it from a command line: `SkyrimDiagDumpToolWinUI.exe "path\\to\\dump.dmp"`
   - In `v0.2.53+`, use the top-level launcher in `SkyrimDiagWinUI`; the many self-contained runtime files are kept under `SkyrimDiagWinUI\\app`.

**Manual snapshot hotkey:** `Ctrl+Shift+F12`
> Snapshots taken during normal gameplay may have low confidence. Best used when the game is already stuck (freeze / ILS) or right before a CTD.

## Reading Results Safely

Tullius complements CrashLogger rather than replacing it. There is no reviewed real-incident corpus large enough to claim higher measured root-cause accuracy than another crash logger.

- **Stronger evidence:** the dump fault module and an eligible CrashLogger frame point to the same non-system DLL. This is a priority for source review or isolation, not automatic proof of root cause.
- **Possible victim location:** `CrashLogger.dll`, a hook framework, the game EXE, or a system DLL can be where corrupted state is finally observed. `v0.2.57+` preserves an already captured original exception from later CrashLogger-internal faults and cautiously prefers an eligible non-hook frame candidate when the paired log supports it.
- **Context only:** referenced ESP/ESM records, recently loaded resources, and resource providers show correlation around the incident. They must not be treated as the culprit without an independent stack, reproduction, or source-level match.
- **Pairing quality matters:** ambiguous or distant CrashLogger log matches are recorded with reduced confidence and cannot independently promote a candidate to High confidence.

## Output Location

- By default (`OutputDir=` left blank): MO2 `overwrite\SKSE\Plugins\Tullius Ctd Logs\`
- To redirect: set `OutputDir=` in `SkyrimDiagHelper.ini`
  Leaving it blank uses the default `Tullius Ctd Logs` subfolder. Explicit values should be plain paths without quotes. Absolute paths work; relative paths are resolved from the helper folder.
- Startup compatibility snapshot: `SkyrimDiag_Preflight.json` (`EnableCompatibilityPreflight=1`)
- Dump failure fallback hint: `SkyrimDiag_WER_LocalDumps_Hint.txt` (`EnableWerDumpFallbackHint=1`)

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
| `EnableAdaptiveResourceLogThrottle` | `1` | Samples resource events during heavy loose-file bursts to reduce hook overhead. |
| `ResourceLogThrottleHighWatermarkPerSec` | `1500` | Per-second event threshold that starts adaptive sampling. |
| `ResourceLogThrottleMaxSampleDivisor` | `8` | Maximum sampling divisor under burst load (higher = lower overhead, less detail). |
| `EnablePerfHitchLog` | `1` | Logs main-thread stalls (lightweight). |
| `CrashHookMode` | `1` | **Keep at 1.** Mode 2 (all exceptions) is not recommended. |
| `AllowOnlineSymbols` | `0` | Offline/local cache analysis by default. |

Helper diagnostics options (`SkyrimDiagHelper.ini`):

| Option | Default | Note |
|--------|---------|------|
| `EnableCompatibilityPreflight` | `1` | Writes startup compatibility checks to `SkyrimDiag_Preflight.json`. |
| `EnableWerDumpFallbackHint` | `1` | Writes WER LocalDumps setup hint when dump capture fails. |

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

You do not need to attach every file in the output directory. Start with the files from the same incident timestamp; add Blackbox, WCT, ETL, and external CrashLogger logs only when they exist and are relevant.

> **Privacy:** Dumps and external logs may contain PC paths (drive letters, usernames). Review/mask before public upload.

## Development

See [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).
