# Tullius CTD Logger — 한국어 안내 (Beta)

> 이 프로젝트는 **Skyrim SE/AE** 환경에서 CTD(크래시) / 프리징 / 무한로딩을 **best-effort**로 진단하기 위한 도구입니다.  
> 내부 파일명/바이너리는 아직 `SkyrimDiag.*` 로 남아있을 수 있습니다(호환/개발 편의 목적).

## 구성 요소

- **SKSE 플러그인**: `SKSE/Plugins/SkyrimDiag.dll`
  - 게임 내부 이벤트/상태(블랙박스), heartbeat, (옵션) 리소스(.nif/.hkx/.tri) 기록
- **Helper(외부 프로세스)**: `SKSE/Plugins/SkyrimDiagHelper.exe`
  - 게임 프로세스에 attach → CTD/프리징/무한로딩 감지 → 덤프 + WCT(Wait Chain) 저장
- **DumpTool(뷰어/분석기)**: `SKSE/Plugins/SkyrimDiagDumpTool.exe`
  - `.dmp`를 WinDbg 없이 읽고 **요약/근거/이벤트/리소스/WCT** 형태로 보기 쉽게 표시

## 설치 (MO2)

1) GitHub Release의 zip를 **MO2에서 “모드로 설치”** 합니다.  
2) 설치된 모드에 아래 파일이 포함되어 있는지 확인합니다:
   - `SKSE/Plugins/SkyrimDiag.dll`
   - `SKSE/Plugins/SkyrimDiag.ini`
   - `SKSE/Plugins/SkyrimDiagHelper.exe`
   - `SKSE/Plugins/SkyrimDiagHelper.ini`
   - `SKSE/Plugins/SkyrimDiagDumpTool.exe`
3) 모드를 활성화한 뒤 **SKSE로 게임을 실행**합니다.

## 기본 동작/출력 위치

- 기본 설정에서 SKSE로 실행하면 Helper가 자동으로 붙습니다.
- 결과(덤프/보고서)는 보통 MO2:
  - `overwrite\\SKSE\\Plugins\\` 에 생성됩니다.
- 한 곳으로 모으고 싶으면 `SkyrimDiagHelper.ini`의 `OutputDir=`를 설정하세요.

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

- `.dmp`를 `SkyrimDiagDumpTool.exe`에 드래그&드롭하거나, 실행 후 파일을 선택합니다.
- 탭:
  - **요약**: 결론 1문장 + 신뢰도
  - **근거**: 콜스택/스택스캔/리소스 충돌/WCT 등 단서
  - **이벤트**: 직전 이벤트(상관관계 단서)
  - **리소스**: 최근 로드된 `.nif/.hkx/.tri` 및 MO2 제공자(충돌 단서)
  - **WCT**: 스레드 대기 관계(데드락/바쁜 대기 추정)

## 베타 테스트 / 제보

- 자세한 제보 양식: `docs/BETA_TESTING.md`
- 원인 판정은 “확정”이 아니라 **best-effort 추정 + 신뢰도 표기**입니다.

