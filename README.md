# Tullius CTD Logger (SkyrimDiag) — Beta

> **한국어 안내(메인)** + **베타 테스터 가이드** 포함  
> 내부 파일명/바이너리는 아직 `SkyrimDiag.*` 로 남아있을 수 있습니다(호환/개발 편의 목적).

## 한국어 안내

### 1) 무엇인가요?

Skyrim SE/AE 환경에서 **CTD(크래시) / 프리징 / 무한로딩** 상황을 best-effort로 캡처하고,
WinDbg 없이도 “왜 그런지”를 **요약/근거/체크리스트** 형태로 보여주는 진단 도구입니다.

### 2) 구성 요소

- **SKSE 플러그인**: `SKSE/Plugins/SkyrimDiag.dll`
  - 블랙박스(이벤트/상태) 기록, heartbeat, (옵션) 리소스(.nif/.hkx/.tri) 기록
- **Helper(외부 프로세스)**: `SKSE/Plugins/SkyrimDiagHelper.exe`
  - 게임 프로세스 attach → 프리징/무한로딩 감지 → 덤프 + WCT 저장
- **DumpTool(뷰어/분석기)**:
  - 기본(현대 UI): `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
  - 고급/레거시 뷰어: `SKSE/Plugins/SkyrimDiagDumpTool.exe`
  - WinUI가 내부적으로 기존 분석 파이프라인을 재사용해 `.dmp` 결과를 초보 친화적으로 표시

### 3) 설치 (MO2)

1) GitHub Releases의 zip를 **MO2에서 “모드로 설치”** 후 활성화  
2) 아래 파일들이 포함되어 있는지 확인:
   - `SKSE/Plugins/SkyrimDiag.dll`
   - `SKSE/Plugins/SkyrimDiag.ini`
   - `SKSE/Plugins/SkyrimDiagHelper.exe`
   - `SKSE/Plugins/SkyrimDiagHelper.ini`
   - `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
   - `SKSE/Plugins/SkyrimDiagDumpTool.exe`
3) MO2에서 **SKSE로 실행**

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

## 베타 테스터 가이드 (README 버전)

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
  - `Open advanced viewer` 버튼으로 기존 탭형 뷰어(요약/근거/이벤트/리소스/WCT) 실행
- DumpTool 언어:
  - 기본: 영어(넥서스 배포용). `Lang: EN/KO` 버튼으로 한국어 토글 가능
  - 영구 설정: `SkyrimDiagDumpTool.ini` → `[SkyrimDiagDumpTool] Language=en|ko`
  - 초보/고급 기본 화면 설정: `SkyrimDiagDumpTool.ini` → `[SkyrimDiagDumpTool] BeginnerMode=1|0`
  - CLI(WinUI): `SkyrimDiagDumpToolWinUI.exe --lang en|ko <dump>`
  - CLI UI 강제(호환): `SkyrimDiagDumpToolWinUI.exe --simple-ui <dump>` 또는 `--advanced-ui <dump>`
- 탭:
  - **요약**: 결론 1문장 + 신뢰도
  - **근거**: 콜스택/스택스캔/리소스 충돌/WCT 등 단서
  - **이벤트**: 직전 이벤트(상관관계 단서)
  - **리소스**: 최근 로드된 `.nif/.hkx/.tri` 및 MO2 제공자(충돌 단서)
  - **WCT**: 스레드 대기 관계(데드락/바쁜 대기 추정)

### C. 추천 설정(베타용)

- `SkyrimDiag.ini`
  - `CrashHookMode=1` 권장 (정상 동작 중 C++ 예외 throw/catch 같은 오탐을 줄이는 데 도움)
- `SkyrimDiagHelper.ini`
  - `DumpMode=1` 기본 권장 (FullMemory는 파일이 매우 커질 수 있음)
  - `DumpToolExe` 기본값은 `SkyrimDiagWinUI\SkyrimDiagDumpToolWinUI.exe`이며, 파일이 없으면 helper가 `SkyrimDiagDumpTool.exe`로 자동 폴백
  - DumpTool 자동 열기 정책(초보 기본값)
    - `AutoOpenViewerOnCrash=1` : CTD 덤프 생성 직후 뷰어 자동 표시
    - `AutoOpenViewerOnHang=1` + `AutoOpenHangAfterProcessExit=1` : 프리징 덤프는 게임 종료 후 자동 표시
    - `AutoOpenHangDelayMs=2000` : 종료 후 2초 지연
    - `AutoOpenViewerOnManualCapture=0` : 수동 캡처는 자동 팝업 안 함
    - `AutoOpenViewerBeginnerMode=1` : 자동 오픈 시 초보 화면으로 시작
  - Alt-Tab/백그라운드 일시정지 오탐 방지(기본값 권장)
    - `SuppressHangWhenNotForeground=1`
    - `ForegroundGraceSec=5` (포그라운드로 돌아온 직후 잠깐 기다렸다가 캡처)
    - 포그라운드 복귀 후에도 창이 정상 응답 중이면 행 덤프를 계속 억제(Alt-Tab 오탐 추가 감소)
  - 고급 옵션(기본 OFF): `EnableEtwCaptureOnHang=1`
    - `wpr.exe`로 hang 캡처 전후 ETW를 짧게 수집해 추가 단서(`*.etl`)를 남김
    - ETW 수집 실패해도 dump/WCT는 계속 생성됨(best-effort)
  - “fault module을 특정하지 못함”이 반복되면 **해당 문제 상황에서만** `DumpMode=2`로 올려 재캡처

### D. “빠른 재현” 테스트(가능한 경우)

CTD가 잘 안 나는 모드팩에서는, 베타 검증을 위해 “기능이 동작하는지”만 빠르게 확인할 수 있습니다.

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
- (있다면) Crash Logger SSE/AE의 `crash-*.log` 또는 `threaddump-*.log`
  - v1.18.0+의 `C++ EXCEPTION:` 블록(throw 타입/정보/위치/모듈)이 있으면 DumpTool에서 함께 표시됩니다.

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
- Crash Logger crash-*.log / threaddump-*.log: (있으면)

[추가 메모]
- 최근 설치/업데이트한 모드/플러그인:
- 추정 원인 또는 의심 모드:
```

### G. 개인정보/보안 주의

- 덤프/로그에는 PC 경로(드라이브 문자/유저명)가 포함될 수 있습니다.
- 공개 업로드가 부담되면, 경로가 보이는 부분은 마스킹 후 공유해주세요.

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
- **DumpTool (EXE)**: reads `.dmp`, extracts SkyrimDiag user streams, writes a human-friendly summary + blackbox timeline
  - Modern UI shell: `SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
  - Legacy analyzer/viewer backend: `SkyrimDiagDumpTool.exe`

## Install (MO2)

- Install as a mod with:
  - `SKSE/Plugins/SkyrimDiag.dll`
  - `SKSE/Plugins/SkyrimDiag.ini`
  - `SKSE/Plugins/SkyrimDiagHelper.exe`
  - `SKSE/Plugins/SkyrimDiagHelper.ini`
  - `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
  - `SKSE/Plugins/SkyrimDiagDumpTool.exe`
- Default behavior: launching SKSE will auto-start the helper (`AutoStartHelper=1` in `SkyrimDiag.ini`).

## Use

- Run Skyrim via SKSE as usual (MO2).
- Outputs:
  - Dumps/WCT/stats are written by the helper. Set `OutputDir` in `SkyrimDiagHelper.ini` for an easy-to-find folder.
- Hang detection (in `SkyrimDiagHelper.ini`):
  - `SuppressHangWhenNotForeground=1` avoids false hang dumps while Skyrim is paused in the background (Alt-Tab).
  - `ForegroundGraceSec=5` waits briefly after returning to foreground before capturing a hang, so momentary resume delays don’t spam dumps.
  - After returning to foreground, hang dumps stay suppressed while the game window is responsive (and not in a loading screen), until the heartbeat advances.
  - Advanced (default OFF): `EnableEtwCaptureOnHang=1` captures a short ETW trace (`*.etl`) around hang dump generation via `wpr.exe` (best-effort; dump/WCT flow continues even if ETW fails).
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
    - Default executable: `DumpToolExe=SkyrimDiagWinUI\SkyrimDiagDumpToolWinUI.exe` (auto-fallback to `SkyrimDiagDumpTool.exe` if missing)
  - Viewer auto-open policy (beginner-friendly defaults):
    - `AutoOpenViewerOnCrash=1`: open viewer immediately for crash dumps.
    - `AutoOpenViewerOnHang=1` + `AutoOpenHangAfterProcessExit=1`: queue latest hang dump and open viewer after Skyrim exits.
    - `AutoOpenHangDelayMs=2000`: delay opening for 2 seconds after process exit.
    - `AutoOpenViewerOnManualCapture=0`: manual hotkey captures stay headless by default.
    - `AutoOpenViewerBeginnerMode=1`: auto-open starts in beginner view.
  - Manual:
    - Drag-and-drop a `.dmp` onto `SkyrimDiagDumpToolWinUI.exe`, or double-click it to pick a dump file.
    - WinUI opens beginner-first results and can open legacy advanced tabs on demand.
  - Language (DumpTool):
    - Default: English (for Nexus). Toggle in-app via the `Lang: EN/KO` button.
    - Persist via `SkyrimDiagDumpTool.ini`: `[SkyrimDiagDumpTool] Language=en|ko`
    - CLI override: `SkyrimDiagDumpToolWinUI.exe --lang en|ko <dump>`
    - Default UI mode: `SkyrimDiagDumpTool.ini` `[SkyrimDiagDumpTool] BeginnerMode=1|0`
    - CLI UI override: `SkyrimDiagDumpToolWinUI.exe --simple-ui <dump>` or `--advanced-ui <dump>`
  - Output files:
    - `<stem>_SkyrimDiagSummary.json` (exception + module+offset, flags, etc.)
    - `<stem>_SkyrimDiagReport.txt` (quick human-readable report)
    - `<stem>_SkyrimDiagBlackbox.jsonl` (timeline of recent in-game events)

## Validate (optional)

For in-game validation without waiting:
- In `SkyrimDiag.ini`, set `EnableTestHotkeys=1`
  - `Ctrl+Shift+F10` → intentional crash (tests crash capture)
  - `Ctrl+Shift+F11` → intentional hang on the main thread (tests hang detection + WCT/dump)

## CI (GitHub Actions)

- Workflow: `.github/workflows/ci.yml`
- Scope: Linux smoke/unit tests for parser + hang suppression + i18n core
- Trigger: `push`, `pull_request`
- Manual Windows packaging job: `workflow_dispatch` (build + package zip artifact upload on `windows-2022`)

Equivalent local commands:
```bash
cmake -S . -B build-linux -G Ninja
cmake --build build-linux --target skydiag_hang_suppression_tests skydiag_crashlogger_parser_tests skydiag_i18n_core_tests
ctest --test-dir build-linux --output-on-failure
```

## Beta testing

베타 배포 목적은 “원인 모드 특정”을 **최대한 유저 친화적으로** 돕는 것입니다. 다만 덤프 기반 추정은 본질적으로 한계가 있으므로,
리포트의 **신뢰도(높음/중간/낮음)** 표기를 참고해주세요.

- 베타 테스터 가이드: `docs/BETA_TESTING.md`

## Package (zip)

After building on Windows, create an MO2-friendly zip:
```powershell
python scripts/package.py --build-dir build-win --out dist/Tullius_ctd_loger.zip
```
The packager auto-includes WinUI files when found in `build-winui` (override with `--winui-dir`, disable with `--no-winui`).

## Build (Windows)

Prereqs:
- Visual Studio 2022 (C++ Desktop)
- `vcpkg` and `VCPKG_ROOT` env var set

Configure + build:
```powershell
cmake -S . -B build --preset default
cmake --build build --preset default
```

Publish modern WinUI viewer (self-contained, recommended for release zips):
```powershell
scripts\build-winui.cmd
```

Notes:
- This project uses the **CommonLibSSE-NG vcpkg port**. See `vcpkg-configuration.json`.
- Optional env vars for post-build copy:
  - `SKYRIM_FOLDER` → copies `SkyrimDiag.dll` + `SkyrimDiag.ini` into `Data/SKSE/Plugins`
  - `SKYRIM_MODS_FOLDER` → copies into `<mods>/<ProjectName>/SKSE/Plugins`
