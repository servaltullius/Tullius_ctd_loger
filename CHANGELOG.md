# Changelog

## v0.2.25 (2026-02-14)

### 수정
- **Helper: 빠르게 종료되는 크래시에서 덤프가 0바이트로 생성되던 문제 수정.** 기존에는 크래시 이벤트 수신 후 최대 4.5초간 필터링(정상 종료/핸들된 예외 확인)을 먼저 수행한 뒤 덤프를 시도했으나, 그 사이 프로세스가 종료되면 `MiniDumpWriteDump`가 실패하여 빈 파일만 남았음. 이제 **덤프를 즉시 먼저 쓰고**, 사후에 false positive를 필터링(정상 종료 시 덤프 삭제)하는 "dump-first" 전략으로 변경.
- Helper: 덤프 실패 시 0바이트 파일을 자동 삭제하고, 실패 원인을 Helper 로그 파일에 기록하도록 개선 (기존에는 stderr에만 출력).
- DumpTool: CrashLoggerSSE/기타 훅 프레임워크가 유력 후보 1순위로 과도 노출되던 케이스 완화. 스택 후보 정렬에서 훅 프레임워크(특히 `CrashLoggerSSE.dll`)를 보수적으로 비우선화하고, 훅 프레임워크 1순위일 때 CrashLogger 근거 기반 confidence 부스트를 억제하여 오탐 안내를 줄임.
- Helper: crash event가 수동 리셋(manual-reset)인데 소비(reset)하지 않아 동일 신호를 반복 처리하던 루프를 수정. 이벤트 핸들을 `EVENT_MODIFY_STATE|SYNCHRONIZE`로 열고 처리 직후 `ResetEvent`로 소비하여 중복 처리/지연 루프를 방지.
- Helper: handled first-chance 예외 필터를 보수화. heartbeat 1회 전진만으로 덤프 삭제하지 않고, 다중 체크에서 2회 이상 전진이 확인될 때만 삭제하여 실제 크래시 누락 위험을 낮춤.

## v0.2.23 (2026-02-14)

### 수정
- Helper: `SkyrimDiagHelper.log`가 게임 세션 간에 계속 누적되던 문제 수정. 새 게임 세션(프로세스 어태치) 시 로그 파일을 초기화하여 매번 깨끗한 로그로 시작.

### 내부 개선
- Helper: 미사용 파라미터 `attachHeartbeatQpc` 제거 (내부 API 정리).
- Helper: 하트비트 초기화 경고 지연시간을 명명된 상수 `kHeartbeatInitWarnDelaySec`로 추출.

## v0.2.22 (2026-02-14)

### 수정
- Helper: 하트비트가 어태치 이후 전진하지 않으면 자동 행(hang) 캡처가 영구 비활성화되던 문제 수정. 기존의 `heartbeatEverAdvanced` 가드를 제거하고, 플러그인 하트비트 초기화 여부(`last_heartbeat_qpc != 0`)만 확인하도록 변경. 프리즈 시 하트비트가 멈추는 것이 정상 신호이므로, 데드락/무한루프/무한로딩 시나리오에서 자동 캡처가 올바르게 작동.
- Helper: 게임이 프리즈된 상태에서 Alt-Tab하면 포그라운드 억제(`SuppressHangWhenNotForeground`)로 행 덤프가 생성되지 않던 캐치-22 수정. 포그라운드가 아닐 때 윈도우 응답성(`IsWindowResponsive`)을 함께 확인하여, 윈도우가 무응답이면(진짜 프리즈) 억제하지 않고 캡처 진행.
- WinUI: 내부 리스트(증거/콜스택/이벤트 등)와 외부 페이지 스크롤이 동시에 굴러가던 문제 수정. 내부 리스트가 스크롤 경계(상단/하단)에 도달했을 때만 외부 스크롤로 전환.

## v0.2.21 (2026-02-14)

### 수정
- Helper: 핸들링된 첫 번째 기회 예외(first-chance exception)로 인한 오탐 덤프 생성 방지. 크래시 이벤트 수신 후 프로세스가 살아있을 때 하트비트 갱신 여부를 확인하여, 게임이 정상 동작 중이면 덤프를 건너뛰도록 개선.

## v0.2.20 (2026-02-13)

### 추가
- DumpTool: 알려진 훅 프레임워크 모드(EngineFixes, SSE Display Tweaks, po3_Tweaks, HDT-SMP, CrashLoggerSSE 등)가 fault module일 때 confidence를 한 단계 낮추고, "다른 모드의 메모리 오염 피해자일 수 있음" 경고를 Summary와 Recommendations에 표시. 훅 모드가 단순히 크래시 발생 위치일 뿐 진짜 원인이 아닐 수 있음을 사용자에게 안내.

## v0.2.19 (2026-02-13)

### 수정
- Helper: 정상 종료 시 크래시 덤프 생성 억제 강화. 종료 대기 시간을 500ms→3000ms로 증가하여, 모드가 많은 환경에서 DLL 정리 시간이 길어도 정상 종료로 올바르게 판단.
- Helper: 크래시 후 프로세스가 늦게 종료되는 경우 뷰어가 열리지 않던 문제 수정. 프로세스 종료 시점까지 뷰어 실행을 지연(deferred)하여, C++ 예외 등으로 프로세스가 지연 종료되어도 뷰어가 자동으로 열리도록 개선.

## v0.2.18 (2026-02-13)

### 수정
- Helper: 정상 종료 시 크래시 덤프가 생성되던 문제 수정. 종료 과정에서 DLL 정리 중 발생하는 예외를 VEH가 감지하여 덤프를 만들던 현상을, 프로세스 종료 코드(exit_code=0)를 확인해 정상 종료로 판단하면 덤프를 건너뛰도록 개선.

## v0.2.17 (2026-02-13)

### Fixed
- Build: correct MSVC runtime library generator expression in CMake.
- Build: add `/utf-8` compiler flag for MSVC to satisfy fmt v11 requirement.
- Build: handle x64 platform subfolder in WinUI output path.
- Build: explicit exit code 0 after robocopy in `build-winui.cmd`.
- CI: build all test targets instead of hardcoded list.
- CI: add tag-triggered release workflow.
- Tests: remove assertions for unimplemented features.

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
