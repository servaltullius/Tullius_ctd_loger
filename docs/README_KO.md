# Tullius CTD Logger — 한국어 안내

> 이 프로젝트는 **Skyrim SE/AE** 환경에서 CTD(크래시) / 프리징 / 무한로딩을 **best-effort**로 진단하기 위한 도구입니다.  
> 내부 파일명/바이너리는 아직 `SkyrimDiag.*` 로 남아있을 수 있습니다(호환/개발 편의 목적).
>
> 최신 릴리즈: `v0.2.15`  
> https://github.com/servaltullius/Tullius_ctd_loger/releases/latest

## 필수 선행(요구사항)

- Skyrim SE/AE (Windows)
- SKSE64: https://skse.silverlock.org/
- Address Library for SKSE Plugins: https://www.nexusmods.com/skyrimspecialedition/mods/32444
- (선택/권장) Crash Logger SSE AE VR - PDB support: https://www.nexusmods.com/skyrimspecialedition/mods/59818

## 구성 요소

- **SKSE 플러그인**: `SKSE/Plugins/SkyrimDiag.dll`
  - 게임 내부 이벤트/상태(블랙박스), heartbeat, (옵션) 리소스(.nif/.hkx/.tri) 기록
- **Helper(외부 프로세스)**: `SKSE/Plugins/SkyrimDiagHelper.exe`
  - 게임 프로세스에 attach → CTD/프리징/무한로딩 감지 → 덤프 + WCT(Wait Chain) 저장
- **DumpTool(분석기/뷰어)**:
  - **헤드리스 CLI(창 없음)**: `SKSE/Plugins/SkyrimDiagDumpToolCli.exe`
    - Helper가 자동 분석에 사용합니다(사용자가 직접 창을 볼 필요 없음)
  - **WinUI 뷰어(UI)**: `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
    - 네이티브 분석 엔진 DLL `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll`을 직접 호출해
      `.dmp`를 WinDbg 없이 읽고 **요약/근거/이벤트/리소스/WCT**를 표시합니다.

## "창이 하나 더 뜨나요?" (CLI vs WinUI)

- CLI(`SkyrimDiagDumpToolCli.exe`)는 **헤드리스**라서 창이 뜨지 않습니다.
- WinUI(`SkyrimDiagDumpToolWinUI.exe`)가 실제로 “결과를 보여주는 창” 입니다.
- `v0.2.11`부터는 Helper가 덤프를 자동으로 WinUI로 열 때, 같은 덤프에 대해 CLI 자동 분석을 건너뜁니다(중복 실행 방지).

## 설치 (MO2)

1) GitHub Release의 zip를 **MO2에서 “모드로 설치”** 합니다.  
2) 설치된 모드에 아래 파일이 포함되어 있는지 확인합니다:
   - `SKSE/Plugins/SkyrimDiag.dll`
   - `SKSE/Plugins/SkyrimDiag.ini`
   - `SKSE/Plugins/SkyrimDiagHelper.exe`
   - `SKSE/Plugins/SkyrimDiagHelper.ini`
   - `SKSE/Plugins/SkyrimDiagDumpToolCli.exe`
   - `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
   - `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll`
3) 모드를 활성화한 뒤 **SKSE로 게임을 실행**합니다.

## 필수 런타임 (경량 WinUI 배포)

- 이 배포본은 WinUI를 **framework-dependent(경량)** 로 배포합니다.
- 사용자 PC에 아래 런타임이 설치되어 있어야 합니다:
  - .NET Desktop Runtime 8 (x64): https://dotnet.microsoft.com/en-us/download/dotnet/8.0
  - Windows App Runtime (1.8, x64): https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads
  - Microsoft Visual C++ Redistributable 2015-2022 (x64): https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist

## 언어 (한국어/영어)

- WinUI DumpTool UI는 기본적으로 Windows UI 언어를 따릅니다.
- 강제(명령행):
  - 한국어: `SkyrimDiagDumpToolWinUI.exe --lang ko`
  - 영어: `SkyrimDiagDumpToolWinUI.exe --lang en`

## 기본 동작/출력 위치

- 기본 설정에서 SKSE로 실행하면 Helper가 자동으로 붙습니다.
- 결과(덤프/보고서)는 보통 MO2:
  - `overwrite\\SKSE\\Plugins\\` 에 생성됩니다.
- 한 곳으로 모으고 싶으면 `SkyrimDiagHelper.ini`의 `OutputDir=`를 설정하세요.

## “저장/로드 중 창이 뜨는데 게임은 계속됨” 케이스

- SkyrimDiag는 CTD(프로세스 종료)뿐 아니라 저장/코세이브 과정에서 발생한 예외가 훅에 잡히면 덤프/리포트를 만들 수 있습니다.
- 예외가 내부에서 처리(handled)되면 게임이 계속 진행될 수 있습니다. 이 경우 “유력 후보”는 **확정 원인**이라기보다, 해당 시점에 예외를 유발한 **위험 신호**로 보는 게 안전합니다.
- 팝업을 줄이려면 `SkyrimDiagHelper.ini`에서:
  - `AutoOpenCrashOnlyIfProcessExited=1` (기본): 게임이 곧바로 종료될 때만 크래시 뷰어 자동 오픈
  - `AutoOpenViewerOnCrash=0`: 크래시에서도 자동 오픈 완전 끄기

## 덤프/로그 보관(디스크 정리)

- `SkyrimDiagHelper.ini`의 아래 값으로 오래된 덤프/아티팩트(WCT JSON, ETL, Summary/Report/Blackbox 등)를 자동 정리합니다. (`0` = 무제한)
  - `MaxCrashDumps`, `MaxHangDumps`, `MaxManualDumps`, `MaxEtwTraces`
  - `MaxHelperLogBytes`, `MaxHelperLogFiles` (Helper 로그 로테이션)

## 성능 영향(오버헤드) 안내

- 이 도구는 “상시 FPS 측정/상시 스택워크” 같은 무거운 방식이 아니라, **최소 신호 기록 + 필요 시 덤프 캡처** 구조를 목표로 합니다.
- 그래도 환경에 따라 아래 옵션은 미세한 오버헤드/스턱 가능성이 있습니다:
  - `EnableResourceLog=1` : loose file 오픈 후킹(필터: `.nif/.hkx/.tri`) → 로딩/스트리밍이 많은 모드팩에서 미세 영향 가능  
    → 의심되면 **가장 먼저 `EnableResourceLog=0`** 로 테스트하세요.
  - `EnablePerfHitchLog=1` : 메인 스레드 스톨(히치) 단서 기록(가벼움) → 필요 없으면 끌 수 있습니다.
  - `CrashHookMode=2` : 모든 예외 기록(권장하지 않음) → **기본값 `CrashHookMode=1` 유지 권장**
  - `CrashHookMode=2`는 보호 옵션이 켜져야만 동작합니다:
    - `EnableUnsafeCrashHookMode2=0` (기본)
    - `EnableUnsafeCrashHookMode2=1`일 때만 mode 2 허용
  - `AllowOnlineSymbols=0` (기본): 온라인 심볼 서버 사용 없이 로컬/오프라인 캐시 우선 분석

## 캡처 방식

- **실제 CTD(게임이 튕김)**: `*_Crash_*.dmp` 생성
- **프리징/무한로딩(자동 감지)**: 기준 시간 이상 멈추면 `*_Hang_*.dmp` 생성
  - 기준 시간은 `SkyrimDiagHelper.ini`에서 조절:
    - `HangThresholdInGameSec` (인게임)
    - `HangThresholdLoadingSec` (로딩 화면)
    - `EnableAdaptiveLoadingThreshold=1` (로딩 시간 자동 학습/보정, 추천)
- **수동 캡처(스냅샷)**: `Ctrl+Shift+F12`
  - 정상 상태에서 찍은 수동 캡처는 “문제”가 아닐 수 있습니다.
  - **문제 상황(프리징/무한로딩/CTD 직전)** 에서 찍는 것이 진단에 가장 유효합니다.

## DumpTool(뷰어) 사용법

- `.dmp`를 `SkyrimDiagDumpToolWinUI.exe`에 드래그&드롭하거나, 실행 후 파일을 선택합니다.
- 탭:
  - **요약**: 결론 1문장 + 신뢰도
  - **근거**: 콜스택/스택스캔/리소스 충돌/WCT 등 단서
  - **이벤트**: 직전 이벤트(상관관계 단서)
  - **리소스**: 최근 로드된 `.nif/.hkx/.tri` 및 MO2 제공자(충돌 단서)
  - **WCT**: 스레드 대기 관계(데드락/바쁜 대기 추정)

## 이슈 제보 / 트러블슈팅

- 자세한 제보 양식: `docs/BETA_TESTING.md`
- 원인 판정은 “확정”이 아니라 **best-effort 추정 + 신뢰도 표기**입니다.

## 개인정보/보안 주의

- Summary/Report 출력은 기본적으로 경로 마스킹이 적용됩니다.
  - `privacy.path_redaction_applied=1` 여부로 확인 가능
- 다만 원본 덤프(`*.dmp`)와 외부 로그(CrashLogger 등)에는 PC 경로(드라이브 문자/유저명)가 포함될 수 있습니다.
- 공개 업로드 시에는 원본 파일 공유 범위를 최소화하고, 필요 시 경로/식별자 마스킹 후 공유하세요.
