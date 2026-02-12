# Changelog

## v0.2.16 (2026-02-13)

### Fixed
- Helper: fix race condition where crash event was missed if the game process terminated before the next poll cycle. On process exit, the helper now drains any pending crash event (non-blocking) before shutting down.

## v0.2.15 (2026-02-10)

### Fixed
- WinUI DumpTool: surface native analysis exceptions with actionable messages instead of a generic "External component has thrown an exception."
  - When a managed exception occurs during native interop, a `*_SkyrimDiagNativeException.log` is written to the output folder (best-effort).
- DumpTool: fix a rare analysis failure when merging existing summary triage (`[json.exception.invalid_iterator.214] cannot get value`).
- DumpTool: manual snapshot captures (`SkyrimDiag_Manual_*.dmp`) are now more reliably classified as snapshots (not CTDs) unless an exception stream is present.
- DumpTool: do not generate a misleading crash bucket key for snapshot dumps that have no exception/module/callstack information.

## v0.2.14 (2026-02-10)

### Changed
- CrashLogger integration: if CrashLogger.ini sets `Crashlog Directory`, SkyrimDiag will also search that folder when auto-detecting CrashLogger logs (best-effort).

### Added
- Internal regression tests: parse CrashLogger.ini `Crashlog Directory` (quotes/spacing/comments).

## v0.2.13 (2026-02-10)

### Changed
- Internal refactor only: split DumpTool evidence builder internals into smaller modules (no behavior changes).

### Added
- Internal regression tests: harden CrashLogger parser fixtures for v1.20 format variations (callstack rows + version header variants).

## v0.2.12 (2026-02-10)

### Changed
- Internal refactor only: split DumpTool analyzer internals into smaller modules (no behavior changes).
- Internal refactor only: split Helper main into smaller modules (no behavior changes).

## v0.2.11 (2026-02-10)

### Changed
- Avoid duplicate analysis: when Helper auto-opens the WinUI viewer for a dump, it now skips headless auto-analysis for that same dump.

### Added
- New regression test: `tests/headless_analysis_policy_tests.cpp`

## v0.2.10 (2026-02-10)

### Added
- Headless analyzer CLI: `SkyrimDiagDumpToolCli.exe` (no WinUI dependency) for post-incident analysis.
- Helper now prefers the headless CLI for auto-analysis when available, and falls back to the WinUI exe for backward compatibility.
- Packaging now ships `SkyrimDiagDumpToolCli.exe` next to `SkyrimDiagHelper.exe`.
- New tests:
  - `tests/dump_tool_cli_args_tests.cpp`
  - `tests/dump_tool_headless_resolver_tests.cpp`
  - `tests/packaging_includes_cli_tests.py`

## v0.2.9 (2026-02-10)

### Added
- Incident manifest sidecar JSON per capture (enabled by default):
  - `SkyrimDiag_Incident_Crash_*.json`
  - `SkyrimDiag_Incident_Hang_*.json`
  - `SkyrimDiag_Incident_Manual_*.json`
  - Includes `incident_id`, `capture_kind`, artifact filenames, ETW status, and an optional privacy-safe config snapshot.
- Optional crash-window ETW capture in `SkyrimDiagHelper.ini` (advanced, OFF by default):
  - `EnableEtwCaptureOnCrash`
  - `EtwCrashProfile`
  - `EtwCrashCaptureSeconds` (1..30)
- DumpTool now surfaces incident context in summary/report when a manifest is present (`summary.incident.*`).

### Changed
- Retention cleanup now prunes incident manifests alongside their corresponding dumps, and will remove `SkyrimDiag_Crash_*.etl` traces when pruning crash dumps.

## v0.2.8 (2026-02-10)

### Added
- Crash hook safety guard option in `dist/SkyrimDiag.ini`:
  - `EnableUnsafeCrashHookMode2=1` is now required to use `CrashHookMode=2`.
- Online symbol source control in `dist/SkyrimDiagHelper.ini`:
  - `AllowOnlineSymbols=0|1` with default `0` (offline/local cache).
- DumpTool privacy telemetry fields in summary/report outputs:
  - `path_redaction_applied`
  - `online_symbol_source_allowed`
  - `online_symbol_source_used`
- New regression tests:
  - `tests/crash_hook_mode_guard_tests.cpp`
  - `tests/symbol_privacy_controls_tests.cpp`
- Added vibe-kit guard workflow and doctor script scaffolding:
  - `.github/workflows/vibekit-guard.yml`
  - `.vibe/brain/agents_doctor.py`

### Changed
- DumpTool symbolization now defaults to offline/local cache unless explicitly opted in.
- Helper now passes explicit symbol policy flags (`--allow-online-symbols` / `--no-online-symbols`) to WinUI analyzer path.
- Path redaction is applied more consistently in outputs, including resource path lines.
- Test runner wiring now uses `Python3_EXECUTABLE` and `sys.executable` for cross-platform Python invocation.
- Vibe-kit seed/config scripts and docs were refreshed:
  - `.vibe/config.json`
  - `.vibe/README.md`
  - `.vibe/brain/*`
  - `scripts/setup_vibe_env.py`
  - `scripts/vibe.py`

### Fixed
- Windows `ctest` compatibility issue caused by hardcoded `python3` in bucket quality script tests.

## v0.2.6 (2026-02-07)

### Added
- Helper retention/disk cleanup options in `SkyrimDiagHelper.ini`:
  - `MaxCrashDumps`, `MaxHangDumps`, `MaxManualDumps`, `MaxEtwTraces`
  - `MaxHelperLogBytes`, `MaxHelperLogFiles`
- Crash viewer popup suppression options in `SkyrimDiagHelper.ini`:
  - `AutoOpenCrashOnlyIfProcessExited`, `AutoOpenCrashWaitForExitMs`
- DumpTool evidence: exception parameter analysis for common codes (e.g., access violation read/write/execute + address).
- CrashLogger integration: detect and report CrashLogger version string (e.g., `CrashLoggerSSE v1.19.0`) when a log is auto-detected.
- WinUI: added a "Copy summary" action for quick sharing.

### Fixed
- CI Linux workflow now builds all unit test targets before running `ctest`.
- CI Windows manual workflow builds the WinUI shell before packaging.

## v0.2.5 (2026-02-06)

### Fixed
- Packaging bug in `scripts/package.py`: WinUI publish output is now copied recursively, preventing runtime file loss when publish layouts include nested files/directories.
- WinUI packaging crash fix: `scripts/build-winui.cmd` now stages from WinUI build output (includes required `.pri/.xbf` assets) instead of stripped publish output.
- WinUI visual quality improvements: enabled Per-Monitor V2 DPI awareness via app manifest for sharper rendering on high-DPI displays.
- WinUI scrolling reliability: when nested controls consume mouse wheel input, wheel events are chained to the root scroll viewer for smoother page scrolling.
- WinUI localization polish: static UI labels/buttons now switch between English/Korean (`--lang ko` or system UI language Korean).

### Added
- Native analyzer bridge DLL for WinUI (`SkyrimDiagDumpToolNative.dll`) with exported C ABI (`SkyrimDiagAnalyzeDumpW`) so WinUI can analyze dumps directly without launching legacy UI executable.
- Built-in advanced analysis panels in WinUI (callstack, evidence, resources, blackbox events, WCT JSON, report text) in the same window as beginner summary.

### Changed
- WinUI headless mode now runs native analysis directly (no process delegation to `SkyrimDiagDumpTool.exe`).
- Helper dump-tool resolution no longer falls back to legacy executable.
- CMake build no longer defines the legacy `SkyrimDiagDumpTool` Win32 executable target (native DLL + WinUI only).
- WinUI publish switched to framework-dependent/lightweight output (`scripts/build-winui.cmd`), reducing package size but requiring user runtimes.
- WinUI viewer visuals refreshed (typography, spacing, card styling, and list readability) while preserving existing dump-analysis workflow.
- WinUI viewer theme refreshed with a Skyrim-inspired parchment + dark stone look.
- WinUI viewer redesigned again using current Fluent/observability UI patterns:
  - fixed left navigation pane visibility (always expanded labels, no icon-only collapse)
  - added quick triage strip (primary suspect/confidence/actions/events)
  - added explicit 3-step workflow cards in Analyze panel
  - increased visual depth with layered surface tokens (`Window/Pane/Hero/Section/Elevated`)
- Packaging now ships full-replacement WinUI set:
  - includes `SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
  - includes `SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll`
  - no longer requires or packages `SkyrimDiagDumpTool.exe` / `SkyrimDiagDumpTool.ini`

## v0.2.4 (2026-02-06)

### Added
- New modern WinUI 3 viewer shell (`SkyrimDiagDumpToolWinUI.exe`) with beginner-first layout:
  - dump picker + one-click analysis
  - crash snapshot card (summary/bucket/module/mod hint)
  - top cause candidates list
  - recommended next-step checklist
  - quick action to open legacy advanced viewer
- Windows helper script `scripts/build-winui.cmd` to publish WinUI viewer in self-contained mode.
- Packaging enhancement: `scripts/package.py` now auto-includes WinUI publish artifacts when found (configurable with `--winui-dir` and `--no-winui`).

### Changed
- Helper default dump viewer executable changed to `SkyrimDiagWinUI\SkyrimDiagDumpToolWinUI.exe`.
- Helper executable resolution now safely falls back to legacy `SkyrimDiagDumpTool.exe` if WinUI executable is missing.
- README and default ini guidance updated for WinUI-first workflow.

## v0.2.3 (2026-02-06)

### Added
- Crash bucketing key (`crash_bucket_key`) output in Summary JSON/Report, plus callstack symbolization improvements to better group repeated CTDs by signature.
- Beginner-first DumpTool UX:
  - Default beginner view with primary CTA (`Check Cause Candidates` / `원인 후보 확인하기`)
  - Top-5 candidate + evidence presentation
  - Explicit `Advanced analysis` toggle to access full tabs.
- DumpTool single-window reuse path: when already open, new dump opens in the same window via inter-process message handoff (`WM_COPYDATA`) instead of creating extra windows.
- New helper viewer auto-open policy options in `SkyrimDiagHelper.ini`:
  - `AutoOpenViewerOnCrash`
  - `AutoOpenViewerOnHang`
  - `AutoOpenViewerOnManualCapture`
  - `AutoOpenHangAfterProcessExit`
  - `AutoOpenHangDelayMs`
  - `AutoOpenViewerBeginnerMode`
- Optional ETW capture around hang dumps (`EnableEtwCaptureOnHang`, `EtwWprExe`, `EtwProfile`, `EtwMaxDurationSec`) as best-effort diagnostics.
- New bucket unit test target (`skydiag_bucket_tests`) and test source (`tests/bucket_tests.cpp`).

### Changed
- Helper dump flow now separates headless analysis from viewer launch:
  - Crash: viewer can open immediately
  - Hang: latest hang dump can be queued and auto-opened after process exit (with configurable delay)
  - Manual capture: viewer auto-open remains off by default.
- DumpTool now persists beginner/advanced default mode in `SkyrimDiagDumpTool.ini` (`BeginnerMode=1|0`) and supports CLI overrides (`--simple-ui`, `--advanced-ui`).

## v0.2.2 (2026-02-03)

### Fixed
- Further reduced Alt-Tab false hang dumps: after returning to foreground, keep suppressing hang dumps while the game window is responsive (and not in a loading screen), until the heartbeat advances.

### Added
- CrashLogger SSE/AE v1.18.0 support: parse and surface the new `C++ EXCEPTION:` details (Type / Info / Throw Location / Module) in evidence, reports, and JSON output.
- DumpTool i18n (EN/KO): English-first UI/output for Nexus + in-app language toggle (persists via `SkyrimDiagDumpTool.ini` and supports CLI `--lang en|ko`).
- DumpTool UI polish: modern owner-draw buttons, better padding (Summary/WCT), WCT mono font, evidence row striping, and Windows 11 rounded corners (best-effort).

## v0.2.1 (2026-02-01)

### Fixed
- Further reduced false hang dumps around Alt-Tab / background pause by keeping suppression “sticky” until the heartbeat advances, and adding a short foreground grace window (`ForegroundGraceSec`).
- Improved CrashLogger SSE/AE log compatibility (v1.17.0+): better detection and parsing for thread dump logs (`threaddump-*.log`) and stack-trace edge cases.

### Added
- Lightweight cross-platform unit tests for hang suppression logic and CrashLogger log parsing core (Linux-friendly, no Win32 deps).

## v0.2.0 (2026-02-01)

### Fixed
- Prevented false hang dumps when the user Alt-Tabs: by default, hang capture is suppressed while Skyrim is not the foreground window (`SuppressHangWhenNotForeground=1`).
- Reduced false positives around menus/shutdown by using a more conservative menu threshold (`HangThresholdInMenuSec`) and a short re-check grace period before writing hang dumps.

### Changed
- DumpTool internal architecture: split into `SkyrimDiagDumpToolCore` (analysis/output) + `SkyrimDiagDumpTool` (UI) to reduce coupling and make future maintenance safer.
- Improved documentation for beta testing and common misinterpretations (manual snapshot vs. real CTD/hang).

## v0.1.0-beta.1 (2026-01-30)

- Initial public beta release.
