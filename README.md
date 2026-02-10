# Tullius CTD Logger (SkyrimDiag)

> **한국어 안내(메인)** (플레이어/사용자용)  
> 내부 파일명/바이너리는 아직 `SkyrimDiag.*` 로 남아있을 수 있습니다(호환/개발 편의 목적).
>
> Latest release: `v0.2.12` (stable)  
> Download: https://github.com/servaltullius/Tullius_ctd_loger/releases/latest  
> 참고: GitHub Releases 화면 왼쪽의 "tags N"은 **태그 개수 표시**이며, 릴리즈 목록/안정판 여부와는 별개입니다.

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

### 2-1) "창이 하나 더 뜨나요?" (CLI vs WinUI)

- `SkyrimDiagDumpToolCli.exe`는 **헤드리스(창 없음)** 분석기입니다. Helper가 백그라운드에서 실행해 Summary/Report 같은 결과 파일을 생성합니다.
- `SkyrimDiagDumpToolWinUI.exe`는 **뷰어(UI)** 입니다. 사용자가 직접 보는 창은 보통 이 WinUI 1개입니다.
- `v0.2.11`부터: Helper가 덤프를 자동으로 WinUI로 열 때는, 같은 덤프에 대해 CLI 자동 분석을 건너뜁니다(중복 실행 방지).

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

## 이슈 리포팅 (요약)

자세한 가이드: `docs/BETA_TESTING.md`

필수 첨부(가능한 한 같이):
- 문제 상황의 `*.dmp` 1개
- 같은 이름의:
  - `*_SkyrimDiagReport.txt`
  - `*_SkyrimDiagSummary.json`
  - `SkyrimDiag_Incident_*.json`
  - (있다면) `*_SkyrimDiagBlackbox.jsonl`, `SkyrimDiag_WCT_*.json`, `SkyrimDiag_Crash_*.etl` / `SkyrimDiag_Hang_*.etl`
- (있다면) Crash Logger SSE/AE의 `crash-*.log` 또는 `threaddump-*.log`

개인정보 주의:
- 덤프/외부 로그에는 PC 경로(유저명 등)가 포함될 수 있어요. 공개 업로드 전 점검/마스킹을 권장합니다.

---

## Development / Contributing (English)

See `docs/DEVELOPMENT.md`.
