# Tullius CTD Logger (SkyrimDiag)

> **한국어 안내(메인)** + **이슈 리포팅 가이드** 포함  
> 내부 파일명/바이너리는 아직 `SkyrimDiag.*` 로 남아있을 수 있습니다(호환/개발 편의 목적).
>
> Latest release: `v0.2.10`  
> https://github.com/servaltullius/Tullius_ctd_loger/releases/tag/v0.2.10

## Quick Intro (English)

Tullius CTD Logger (SkyrimDiag) is a best-effort diagnostics tool for Skyrim SE/AE that captures **CTD / hang / infinite loading** and produces a readable report (summary + evidence + checklist) without requiring WinDbg.

- Important: this is **not** a crash-prevention mod. It records signals and captures evidence, but it does not swallow exceptions or try to “recover and keep playing”.
- No uploads/telemetry: outputs are written locally. Online symbol downloads are **OFF** by default (`AllowOnlineSymbols=0`).
- Components: SKSE plugin + out-of-proc helper + headless analyzer CLI + WinUI DumpTool (viewer) backed by a native analyzer.
- CrashLoggerSSE integration: auto-detects `crash-*.log` / `threaddump-*.log` and surfaces top callstack modules, C++ exception blocks, and the CrashLogger version string.
- Extra evidence: interprets minidump exception parameters for common codes (e.g. access violation read/write/execute + address).
- Notes: some exceptions are handled and the game may keep running. To reduce “viewer popups while the game continues”, keep `AutoOpenCrashOnlyIfProcessExited=1` (default) or disable crash auto-open with `AutoOpenViewerOnCrash=0`.
- Retention: helper can auto-clean old dumps and derived artifacts (`MaxCrashDumps`, `MaxHangDumps`, `MaxManualDumps`, `MaxEtwTraces`) and rotate its own log (`MaxHelperLogBytes`, `MaxHelperLogFiles`).

## 한국어 안내

### 1) 무엇인가요?

Skyrim SE/AE 환경에서 **CTD(크래시) / 프리징 / 무한로딩** 상황을 best-effort로 캡처하고,
WinDbg 없이도 “왜 그런지”를 **요약/근거/체크리스트** 형태로 보여주는 진단 도구입니다.

- **크래시를 막아주는 모드가 아닙니다.** 예외를 삼키거나(access violation을 무시하고) 실행을 계속하려고 하지 않습니다.
- **업로드/텔레메트리 없음.** 결과물은 로컬에 저장되며, 온라인 심볼 다운로드는 기본 OFF(`AllowOnlineSymbols=0`) 입니다.
- CrashLoggerSSE 로그도 자동으로 찾아서 함께 표시합니다(상위 모듈/ C++ 예외 블록/ CrashLogger 버전).
- 예외 파라미터 분석(예: 접근 위반 read/write/execute + 주소)을 근거로 추가합니다.
- 덤프/아티팩트가 쌓이지 않도록 Helper에서 보관(정리) + 로그 로테이션을 지원합니다.

### 2) 구성 요소

- **SKSE 플러그인**: `SKSE/Plugins/SkyrimDiag.dll`
  - 블랙박스(이벤트/상태) 기록, heartbeat, (옵션) 리소스(.nif/.hkx/.tri) 기록
- **Helper(외부 프로세스)**: `SKSE/Plugins/SkyrimDiagHelper.exe`
  - 게임 프로세스 attach → 프리징/무한로딩 감지 → 덤프 + WCT 저장
- **DumpTool(뷰어/분석기)**:
  - 헤드리스 CLI: `SKSE/Plugins/SkyrimDiagDumpToolCli.exe` (UI 없이 분석만 실행, Helper 자동 분석에 사용)
  - WinUI 앱: `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
  - 네이티브 분석 엔진 DLL: `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll`
  - 초보/고급 분석을 모두 WinUI 단일 창에서 제공

### 3) 설치 (MO2)

1) GitHub Releases의 zip를 **MO2에서 “모드로 설치”** 후 활성화  
2) 아래 파일들이 포함되어 있는지 확인:
   - `SKSE/Plugins/SkyrimDiag.dll`
   - `SKSE/Plugins/SkyrimDiag.ini`
   - `SKSE/Plugins/SkyrimDiagHelper.exe`
   - `SKSE/Plugins/SkyrimDiagHelper.ini`
   - `SKSE/Plugins/SkyrimDiagDumpToolCli.exe`
   - `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
   - `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll`
3) MO2에서 **SKSE로 실행**

### 3-1) 필수 런타임 (경량 WinUI 배포)

- 이 배포본은 WinUI를 **framework-dependent(경량)** 로 배포합니다.
- 사용자 PC에 아래 런타임이 설치되어 있어야 합니다:
  - .NET Desktop Runtime 8 (x64): https://dotnet.microsoft.com/en-us/download/dotnet/8.0
  - Windows App Runtime (1.8, x64): https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads
  - Microsoft Visual C++ Redistributable 2015-2022 (x64): https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist

### 4) 출력 위치

- 기본적으로 결과(덤프/리포트)는 보통 MO2 `overwrite\\SKSE\\Plugins\\`에 생성됩니다.
- 한 곳으로 모으고 싶으면 `SkyrimDiagHelper.ini`의 `OutputDir=`를 설정하세요.

### 5) 성능 영향(오버헤드) 안내

- 기본 설정은 “상시 FPS 측정/상시 스택워크” 같은 무거운 방식이 아니라, **최소 신호 기록 + 필요 시 덤프 캡처** 구조라서 보통 체감 성능 저하는 크지 않습니다.
- 다만 아래 옵션은 환경에 따라 **미세한 오버헤드/스턱** 가능성이 있습니다:
  - `EnableResourceLog=1` : loose file 오픈 후킹(필터: `.nif/.hkx/.tri`) → 로딩/스트리밍이 많은 모드팩에서 미세 영향 가능  
    → 의심되면 **가장 먼저 `EnableResourceLog=0`** 로 테스트해주세요.
  - `EnablePerfHitchLog=1` : 메인 스레드 스톨(히치) 단서 기록(가벼움) → 필요 없으면 끌 수 있습니다.
  - `CrashHookMode=2` : 모든 예외 기록(권장하지 않음) → **기본값 `CrashHookMode=1` 유지 권장**

## 이슈 리포팅 가이드 (README 버전)

> 자세한 버전: `docs/BETA_TESTING.md`

### A. 캡처 종류

- **CTD(게임이 튕김)**: `*_Crash_*.dmp`
- **프리징/무한로딩(자동 감지)**: 기준 시간 이상 멈추면 `*_Hang_*.dmp`
  - 기준 시간은 `SkyrimDiagHelper.ini`에서 조절:
    - `HangThresholdInGameSec` (인게임)
    - `HangThresholdLoadingSec` (로딩 화면)
    - `EnableAdaptiveLoadingThreshold=1` (로딩 시간 자동 학습/보정, 추천)
- **수동 스냅샷(핫키)**: `Ctrl+Shift+F12`
  - 정상 상태에서 찍은 수동 캡처는 “문제”가 아닐 수 있습니다.
  - **문제 상황(프리징/무한로딩/CTD 직전)** 에서 찍는 것이 진단에 가장 유효합니다.

### B. DumpTool(뷰어)로 보는 법

- 기본 권장: `SkyrimDiagDumpToolWinUI.exe`를 실행해 `.dmp`를 선택합니다.
- 또는 `.dmp`를 `SkyrimDiagDumpToolWinUI.exe`에 드래그&드롭합니다.
- 기본 화면은 **초보 보기**입니다.
  - 핵심 CTA: Top 원인 후보 + 추천 조치
  - 같은 창에서 고급 분석(콜스택/근거/리소스/이벤트/WCT/리포트)까지 확인
- DumpTool 언어:
  - 기본: 영어(넥서스 배포용)
  - 헤드리스 CLI: `SkyrimDiagDumpToolCli.exe --lang en|ko <dump> [--out-dir <dir>]`
  - WinUI: `SkyrimDiagDumpToolWinUI.exe --lang en|ko <dump>`
  - 호환 플래그: `--simple-ui` / `--advanced-ui` (입력 호환용)
- 탭:
  - **요약**: 결론 1문장 + 신뢰도
  - **근거**: 콜스택/스택스캔/리소스 충돌/WCT 등 단서
  - **이벤트**: 직전 이벤트(상관관계 단서)
  - **리소스**: 최근 로드된 `.nif/.hkx/.tri` 및 MO2 제공자(충돌 단서)
  - **WCT**: 스레드 대기 관계(데드락/바쁜 대기 추정)

### C. 추천 설정(정식 릴리즈 기본값 기준)

- `SkyrimDiag.ini`
  - `CrashHookMode=1` 권장 (정상 동작 중 C++ 예외 throw/catch 같은 오탐을 줄이는 데 도움)
  - `CrashHookMode=2`는 기본 비활성 보호가 적용됩니다.
    - `EnableUnsafeCrashHookMode2=0` (기본)
    - `EnableUnsafeCrashHookMode2=1`일 때만 mode 2 허용
- `SkyrimDiagHelper.ini`
  - `DumpMode=1` 기본 권장 (FullMemory는 파일이 매우 커질 수 있음)
  - `AllowOnlineSymbols=0` 기본 권장 (오프라인/로컬 캐시 기반 분석)
  - `DumpToolExe` 기본값: `SkyrimDiagWinUI\SkyrimDiagDumpToolWinUI.exe`
  - DumpTool 자동 열기 정책(초보 기본값)
    - `AutoOpenViewerOnCrash=1` : CTD 덤프 생성 직후 뷰어 자동 표시
    - `AutoOpenCrashOnlyIfProcessExited=1` : 게임이 곧바로 종료될 때만 크래시 뷰어 자동 오픈(저장/로드 중 팝업 감소)
    - `AutoOpenCrashWaitForExitMs=2000` : 위 정책의 “종료로 판정” 대기 시간(ms)
    - `AutoOpenViewerOnHang=1` + `AutoOpenHangAfterProcessExit=1` : 프리징 덤프는 게임 종료 후 자동 표시
    - `AutoOpenHangDelayMs=2000` : 종료 후 2초 지연
    - `AutoOpenViewerOnManualCapture=0` : 수동 캡처는 자동 팝업 안 함
    - `AutoOpenViewerBeginnerMode=1` : 자동 오픈 시 초보 화면으로 시작
  - 덤프/아티팩트 보관(0=무제한): `MaxCrashDumps`, `MaxHangDumps`, `MaxManualDumps`, `MaxEtwTraces`
  - Helper 로그 로테이션(0=무제한): `MaxHelperLogBytes`, `MaxHelperLogFiles`
  - Alt-Tab/백그라운드 일시정지 오탐 방지(기본값 권장)
    - `SuppressHangWhenNotForeground=1`
    - `ForegroundGraceSec=5` (포그라운드로 돌아온 직후 잠깐 기다렸다가 캡처)
    - 포그라운드 복귀 후에도 창이 정상 응답 중이면 행 덤프를 계속 억제(Alt-Tab 오탐 추가 감소)
  - 고급 옵션(기본 OFF): `EnableEtwCaptureOnHang=1`
    - `EtwHangProfile`(기본) → 실패 시 `EtwHangFallbackProfile`(선택) 순으로 ETW 시작 재시도
    - `wpr.exe`로 hang 캡처 전후 ETW를 짧게 수집해 추가 단서(`*.etl`)를 남김
    - ETW 수집 실패해도 dump/WCT는 계속 생성됨(best-effort)
  - 고급 옵션(기본 OFF): `EnableEtwCaptureOnCrash=1`
    - 크래시 이벤트 감지 시 `wpr.exe`로 짧은 ETW를 수집해 `SkyrimDiag_Crash_*.etl`을 남깁니다(best-effort).
    - `EtwCrashProfile`(프로파일), `EtwCrashCaptureSeconds`(1..30초)로 동작을 조절합니다.
  - Incident manifest (기본 ON)
    - `EnableIncidentManifest=1`일 때 `SkyrimDiag_Incident_*.json`(작은 사이드카)을 생성해 덤프/아티팩트 관계를 연결합니다.
    - `IncidentManifestIncludeConfigSnapshot=1`은 **절대경로를 제외한**(privacy-safe) 설정 스냅샷을 포함합니다.
  - 반복 버킷 자동 재캡처(기본 OFF)
    - `EnableAutoRecaptureOnUnknownCrash=1`
    - `AutoRecaptureUnknownBucketThreshold=2` (같은 bucket에서 fault module 미확정이 2회 연속이면)
    - `AutoRecaptureAnalysisTimeoutSec=20` (동기 분석 대기)
    - 조건 충족 시 프로세스가 살아있는 경우에 한해 FullMemory crash dump 1회 추가 캡처(best-effort)
  - “fault module을 특정하지 못함”이 반복되면 **해당 문제 상황에서만** `DumpMode=2`로 올려 재캡처

### D. “빠른 재현” 테스트(가능한 경우)

CTD가 잘 안 나는 모드팩에서는, “기능이 동작하는지”만 빠르게 확인할 수 있습니다.

- `SkyrimDiag.ini`에서 `EnableTestHotkeys=1`
  - `Ctrl+Shift+F10` : 의도적 크래시(CTD 덤프 생성 확인)
  - `Ctrl+Shift+F11` : 의도적 행(프리징) 유발(행 감지/WCT/덤프 생성 확인)

### E. 이슈 제보 시 필수 첨부

- 문제 상황의 `*.dmp` 1개
- 같은 이름의:
  - `*_SkyrimDiagReport.txt`
  - `*_SkyrimDiagSummary.json`
  - `*_SkyrimDiagBlackbox.jsonl` (있다면)
  - `SkyrimDiag_WCT_*.json` (있다면)
  - `SkyrimDiag_Incident_*.json` (기본 생성)
  - `SkyrimDiag_Crash_*.etl` / `SkyrimDiag_Hang_*.etl` (ETW를 켠 경우)
- (있다면) Crash Logger SSE/AE의 `crash-*.log` 또는 `threaddump-*.log`
  - v1.18.0+의 `C++ EXCEPTION:` 블록(throw 타입/정보/위치/모듈)이 있으면 DumpTool에서 함께 표시됩니다.
  - `CrashLoggerSSE vX.Y.Z` 버전 문자열도 함께 표시됩니다.

### F. 이슈 템플릿(복사해서 사용)

```text
[환경]
- 게임: SE/AE/VR, 버전:
- SKSE 버전:
- MO2 사용: 예/아니오
- 단일 프로필: 예/아니오

[문제 유형]
- CTD / 프리징 / 무한로딩 / 히치

[재현 방법]
- (가능하면 단계별로)

[첨부]
- *.dmp:
- *_SkyrimDiagReport.txt:
- *_SkyrimDiagSummary.json:
- *_SkyrimDiagBlackbox.jsonl: (있으면)
- SkyrimDiag_WCT_*.json: (있으면)
- SkyrimDiag_Incident_*.json: (기본 생성)
- SkyrimDiag_Crash_*.etl / SkyrimDiag_Hang_*.etl: (ETW를 켠 경우)
- Crash Logger crash-*.log / threaddump-*.log: (있으면)

[추가 메모]
- 최근 설치/업데이트한 모드/플러그인:
- 추정 원인 또는 의심 모드:
```

### G. 개인정보/보안 주의

- Summary/Report 출력은 기본적으로 경로 마스킹이 적용됩니다.
  - `privacy.path_redaction_applied=1` 여부로 확인 가능
- 다만 원본 덤프(`*.dmp`)와 외부 로그(CrashLogger 등)에는 PC 경로(드라이브 문자/유저명)가 포함될 수 있습니다.
- 공개 업로드 시에는 원본 파일 공유 범위를 최소화하고, 필요 시 경로/식별자 마스킹 후 공유해주세요.

---

## English (for contributors)

This repository contains an MVP implementation of the design in:
- `doc/1.툴리우스_ctd_로거_개발명세서.md`
- `doc/2.코드골격참고.md`

## What’s included

- **SKSE plugin (DLL)**: shared-memory blackbox ringbuffer, main-thread heartbeat, passive crash mark (VEH)
  - Optional: recent resource load log (e.g. `.nif/.hkx/.tri`)
  - Optional: best-effort hitch/stutter signal (PerfHitch)
- **Helper (EXE)**: attach/monitor, hang detection, WCT capture, MiniDumpWriteDump with user streams (blackbox + WCT JSON)
- **DumpTool (WinUI + Native DLL)**:
  - WinUI shell: `SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
  - Native analyzer: `SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll`

## Install (MO2)

- Install as a mod with:
  - `SKSE/Plugins/SkyrimDiag.dll`
  - `SKSE/Plugins/SkyrimDiag.ini`
  - `SKSE/Plugins/SkyrimDiagHelper.exe`
  - `SKSE/Plugins/SkyrimDiagHelper.ini`
  - `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
  - `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll`
- Default behavior: launching SKSE will auto-start the helper (`AutoStartHelper=1` in `SkyrimDiag.ini`).
- Runtime prerequisites for lightweight WinUI distribution:
  - .NET Desktop Runtime 8 (x64): https://dotnet.microsoft.com/en-us/download/dotnet/8.0
  - Windows App Runtime (1.8, x64): https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads
  - Microsoft Visual C++ Redistributable 2015-2022 (x64): https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist

## Use

- Run Skyrim via SKSE as usual (MO2).
- Outputs:
  - Dumps/WCT/stats are written by the helper. Set `OutputDir` in `SkyrimDiagHelper.ini` for an easy-to-find folder.
- Hang detection (in `SkyrimDiagHelper.ini`):
  - `SuppressHangWhenNotForeground=1` avoids false hang dumps while Skyrim is paused in the background (Alt-Tab).
  - `ForegroundGraceSec=5` waits briefly after returning to foreground before capturing a hang, so momentary resume delays don’t spam dumps.
  - After returning to foreground, hang dumps stay suppressed while the game window is responsive (and not in a loading screen), until the heartbeat advances.
  - Advanced (default OFF): `EnableEtwCaptureOnHang=1` captures a short ETW trace (`*.etl`) around hang dump generation via `wpr.exe` (best-effort; dump/WCT flow continues even if ETW fails).
  - Advanced (default OFF): `EnableEtwCaptureOnCrash=1` captures a short crash-window ETW trace (`SkyrimDiag_Crash_*.etl`) via `wpr.exe` (best-effort; dump capture does not depend on ETW).
  - Incident manifest (default ON): `EnableIncidentManifest=1` writes `SkyrimDiag_Incident_*.json` to link artifacts (dump/WCT/ETW) with privacy-safe defaults (filenames only; no absolute paths).
- Crash hook behavior (in `SkyrimDiag.ini`):
  - `CrashHookMode=0` Off
  - `CrashHookMode=1` Fatal exceptions only (recommended; reduces false “Crash_*.dmp” during normal play/loading)
  - `CrashHookMode=2` All exceptions (can false-trigger; only if you understand the trade-off)
  - Safety guard: mode 2 is ignored unless `EnableUnsafeCrashHookMode2=1` (default `0`)
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
    - Online symbol source policy: `AllowOnlineSymbols=0` (default, local/offline cache only)
    - Default executable: `DumpToolExe=SkyrimDiagWinUI\SkyrimDiagDumpToolWinUI.exe`
  - Viewer auto-open policy (beginner-friendly defaults):
    - `AutoOpenViewerOnCrash=1`: open viewer immediately for crash dumps.
    - `AutoOpenCrashOnlyIfProcessExited=1`: only auto-open crash viewer if Skyrim exits quickly (reduces popups for handled exceptions).
    - `AutoOpenCrashWaitForExitMs=2000`: “exit soon” wait window in milliseconds.
    - `AutoOpenViewerOnHang=1` + `AutoOpenHangAfterProcessExit=1`: queue latest hang dump and open viewer after Skyrim exits.
    - `AutoOpenHangDelayMs=2000`: delay opening for 2 seconds after process exit.
    - `AutoOpenViewerOnManualCapture=0`: manual hotkey captures stay headless by default.
    - `AutoOpenViewerBeginnerMode=1`: auto-open starts in beginner view.
  - Retention (0 = unlimited):
    - `MaxCrashDumps`, `MaxHangDumps`, `MaxManualDumps`, `MaxEtwTraces`
    - Helper log rotation: `MaxHelperLogBytes`, `MaxHelperLogFiles`
  - Manual:
    - Drag-and-drop a `.dmp` onto `SkyrimDiagDumpToolWinUI.exe`, or double-click it to pick a dump file.
    - WinUI shows beginner and advanced diagnostics in one app window.
  - Language (DumpTool):
    - Default: English (for Nexus).
    - CLI override: `SkyrimDiagDumpToolWinUI.exe --lang en|ko <dump>`
    - Compatibility flags accepted: `--simple-ui`, `--advanced-ui`
  - Output files:
    - `<stem>_SkyrimDiagSummary.json` (exception + module+offset, flags, triage/symbolization metadata 포함)
      - `schema.name/version` 메타데이터 포함(현재 version 2)
      - 재분석 시 기존 `triage` 라벨(리뷰 상태/ground truth)를 보존
    - `<stem>_SkyrimDiagReport.txt` (quick human-readable report)
    - `<stem>_SkyrimDiagBlackbox.jsonl` (timeline of recent in-game events)
  - Triage/quality workflow:
    - `Summary.json`의 `triage` 필드(`review_status`, `ground_truth_mod` 등)에 사후 확정 정보를 기입
    - `python scripts/analyze_bucket_quality.py --root <output-dir> --out-json <report.json>`
    - bucket별 unknown-rate, 리뷰된 케이스 기준 top1 precision을 집계
      - `ground_truth_mod`는 `suspects[0].inferred_mod_name` 우선, 없으면 `module_filename`과 매칭

## Validate (optional)

For in-game validation without waiting:
- In `SkyrimDiag.ini`, set `EnableTestHotkeys=1`
  - `Ctrl+Shift+F10` → intentional crash (tests crash capture)
  - `Ctrl+Shift+F11` → intentional hang on the main thread (tests hang detection + WCT/dump)

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

릴리즈 목적은 “원인 모드 특정”을 **최대한 유저 친화적으로** 돕는 것입니다. 다만 덤프 기반 추정은 본질적으로 한계가 있으므로,
리포트의 **신뢰도(높음/중간/낮음)** 표기를 참고해주세요.

- 이슈 제보 가이드: `docs/BETA_TESTING.md`
- MO2 WinUI 스모크 테스트 체크리스트: `docs/MO2_WINUI_SMOKE_TEST_CHECKLIST.md`

## Package (zip)

After building on Windows, create an MO2-friendly zip:
```powershell
python scripts/package.py --build-dir build-win --out dist/Tullius_ctd_loger.zip
```
The packager requires WinUI publish output from `build-winui` (override path with `--winui-dir`) and includes both `SkyrimDiagDumpToolWinUI.exe` and `SkyrimDiagDumpToolNative.dll`.

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
scripts\build-winui.cmd
```

Notes:
- This project uses the **CommonLibSSE-NG vcpkg port**. See `vcpkg-configuration.json`.
- Optional env vars for post-build copy:
  - `SKYRIM_FOLDER` → copies `SkyrimDiag.dll` + `SkyrimDiag.ini` into `Data/SKSE/Plugins`
  - `SKYRIM_MODS_FOLDER` → copies into `<mods>/<ProjectName>/SKSE/Plugins`
