# Tullius CTD Logger — 한국어 안내

[![Latest Release](https://img.shields.io/github/v/release/servaltullius/Tullius_ctd_loger)](https://github.com/servaltullius/Tullius_ctd_loger/releases/latest)

> 내부 파일명/바이너리는 `SkyrimDiag.*`로 남아있을 수 있습니다 (호환/개발 편의 목적).

Skyrim SE/AE 환경에서 **CTD(크래시) / 프리징 / 무한로딩**을 best-effort로 캡처하고,
WinDbg 없이 **요약 / 근거 / 체크리스트** 형태로 보여주는 진단 도구입니다.

- **크래시를 막아주는 모드가 아닙니다.** 예외를 삼키거나 실행을 계속하지 않습니다.
- **업로드/텔레메트리 없음.** 결과물은 로컬에 저장되며, 온라인 심볼 다운로드는 기본 OFF (`AllowOnlineSymbols=0`).
- **CrashLoggerSSE 연동** — `crash-*.log` / `threaddump-*.log`를 자동으로 찾아서 함께 표시합니다.

---

## 필수 요구사항

- **Skyrim SE / AE** (Windows)
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- WinUI 뷰어 런타임:
  - [.NET Desktop Runtime 8 (x64)](https://dotnet.microsoft.com/en-us/download/dotnet/8.0)
  - [Windows App Runtime 1.8 (x64)](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads)
  - [Visual C++ Redistributable 2015-2022 (x64)](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)
- (선택/권장) [Crash Logger SSE AE VR — PDB support](https://www.nexusmods.com/skyrimspecialedition/mods/59818)

## 구성 요소

| 구성 요소 | 파일 | 역할 |
|-----------|------|------|
| SKSE 플러그인 | `SkyrimDiag.dll` | 블랙박스(이벤트/상태) 기록, heartbeat, (옵션) 리소스 기록 |
| Helper | `SkyrimDiagHelper.exe` | 외부 프로세스 — 게임에 attach, 프리징/무한로딩 감지, 덤프 + WCT 저장 |
| CLI 분석기 | `SkyrimDiagDumpToolCli.exe` | 헤드리스(창 없음) 분석 — Helper 자동 분석에 사용 |
| WinUI 뷰어 | `SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe` | 대화형 뷰어: 요약 / 근거 / 이벤트 / 리소스 / WCT |
| 네이티브 엔진 | `SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll` | WinUI 뷰어의 네이티브 분석 엔진 |

> **"창이 하나 더 뜨나요?"** — CLI는 헤드리스(창 없음)이고, WinUI만 결과를 보여주는 창입니다.
> v0.2.11부터 Helper가 WinUI를 자동으로 열면 같은 덤프에 대해 CLI 분석을 건너뜁니다.

## 설치 (MO2)

1. GitHub Release의 zip을 **MO2에서 "모드로 설치"** 후 활성화
2. 아래 파일이 포함되어 있는지 확인:
   - `SKSE/Plugins/SkyrimDiag.dll` + `.ini`
   - `SKSE/Plugins/SkyrimDiagHelper.exe` + `.ini`
   - `SKSE/Plugins/SkyrimDiagDumpToolCli.exe`
   - `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
   - `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll`
3. **SKSE로 게임 실행**

## 사용법

1. SKSE로 실행하면 Helper가 자동으로 붙습니다
2. CTD / 프리징 / 무한로딩 발생 시 덤프(`.dmp`) + 리포트가 자동 생성
3. `SkyrimDiagDumpToolWinUI.exe`에 `.dmp`를 **드래그 & 드롭** → "지금 분석"

**수동 스냅샷:** `Ctrl+Shift+F12`
> 정상 플레이 중 찍은 스냅샷은 신뢰도가 낮을 수 있습니다. 프리징/무한로딩/CTD 직전에 찍는 것이 가장 유효합니다.

## 출력 위치

- 기본: MO2 `overwrite\SKSE\Plugins\`
- 변경: `SkyrimDiagHelper.ini`에서 `OutputDir=` 설정
- 시작 시 호환성 점검 결과: `SkyrimDiag_Preflight.json` (설정 `EnableCompatibilityPreflight=1`)

## 캡처 방식

| 상황 | 덤프 파일 패턴 | 트리거 |
|------|---------------|--------|
| CTD (게임 튕김) | `*_Crash_*.dmp` | 자동 |
| 프리징 / 무한로딩 | `*_Hang_*.dmp` | 자동 (임계값 기반) |
| 수동 스냅샷 | `*_Manual_*.dmp` | `Ctrl+Shift+F12` |

프리징 감지 임계값 (`SkyrimDiagHelper.ini`):
- `HangThresholdInGameSec` / `HangThresholdLoadingSec`
- `EnableAdaptiveLoadingThreshold=1` (권장 — 로딩 시간 자동 학습)

## 언어

WinUI DumpTool UI는 기본적으로 Windows UI 언어를 따릅니다.
```
SkyrimDiagDumpToolWinUI.exe --lang ko   # 한국어 강제
SkyrimDiagDumpToolWinUI.exe --lang en   # 영어 강제
```

## "저장/로드 중 창이 뜨는데 게임은 계속됨"

- 저장/코세이브 과정에서 발생한 예외가 내부에서 처리(handled)되면 게임이 계속될 수 있습니다.
- 이 경우 "유력 후보"는 확정 원인이 아니라 **위험 신호**로 해석하세요.
- 팝업을 줄이려면 `SkyrimDiagHelper.ini`에서:
  - `AutoOpenCrashOnlyIfProcessExited=1` (기본) — 게임 종료 시에만 뷰어 자동 오픈
  - `AutoOpenViewerOnCrash=0` — 자동 오픈 완전 끄기

## 성능 영향

**최소 신호 기록 + 필요 시 덤프 캡처** 구조로, 보통 체감 성능 저하는 크지 않습니다.

| 옵션 | 기본값 | 참고 |
|------|--------|------|
| `EnableResourceLog` | `1` | 리소스 후킹 (.nif/.hkx/.tri). 의심 시 가장 먼저 `0`으로 테스트. |
| `EnableAdaptiveResourceLogThrottle` | `1` | 대량 리소스 burst 시 샘플링으로 오버헤드 완화. |
| `EnablePerfHitchLog` | `1` | 메인 스레드 스톨 기록 (가벼움). |
| `CrashHookMode` | `1` | **1 유지 권장.** 2 = 모든 예외 기록 (비권장). |
| `AllowOnlineSymbols` | `0` | 로컬/오프라인 캐시 우선 분석. |

## 덤프 보관 / 디스크 정리

`SkyrimDiagHelper.ini`에서 자동 정리:
- `MaxCrashDumps`, `MaxHangDumps`, `MaxManualDumps`, `MaxEtwTraces` (0 = 무제한)
- `MaxHelperLogBytes`, `MaxHelperLogFiles` (로그 로테이션)

## 이슈 제보

자세한 가이드: [`docs/BETA_TESTING.md`](BETA_TESTING.md)

**최소 첨부 파일:**
- `.dmp` 파일
- `*_SkyrimDiagReport.txt`, `*_SkyrimDiagSummary.json`
- `SkyrimDiag_Incident_*.json`
- (있다면) `*_SkyrimDiagNativeException.log`, `*_SkyrimDiagBlackbox.jsonl`, `SkyrimDiag_WCT_*.json`, ETL 트레이스
- (있다면) CrashLogger `crash-*.log` / `threaddump-*.log`

> **개인정보 주의:** 덤프와 외부 로그에는 PC 경로(유저명 등)가 포함될 수 있습니다. 공개 업로드 전 확인/마스킹을 권장합니다.

## 개발 / 기여

[`docs/DEVELOPMENT.md`](DEVELOPMENT.md) 참고.
