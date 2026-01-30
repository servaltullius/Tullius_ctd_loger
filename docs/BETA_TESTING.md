# Beta Testing Guide (Tullius CTD Logger)

> 이 문서는 **베타 테스터용** 가이드입니다.  
> 결과는 “확정”이 아니라 **best-effort 추정 + 신뢰도(높음/중간/낮음)** 로 제공됩니다.

## 1) 설치 (MO2)

- GitHub Release의 zip를 MO2에서 “모드로 설치” 후 활성화합니다.
- 설치 결과에 아래 파일들이 포함되어야 합니다:
  - `SKSE/Plugins/SkyrimDiag.dll`
  - `SKSE/Plugins/SkyrimDiag.ini`
  - `SKSE/Plugins/SkyrimDiagHelper.exe`
  - `SKSE/Plugins/SkyrimDiagHelper.ini`
  - `SKSE/Plugins/SkyrimDiagDumpTool.exe`

## 2) 기본 동작

- SKSE로 게임을 실행하면(기본 설정 기준) Helper가 자동으로 붙어서 모니터링합니다.
- 덤프/분석 결과는 보통 MO2 `overwrite\SKSE\Plugins\`에 생성됩니다.
  - 다른 폴더로 모으고 싶으면 `SkyrimDiagHelper.ini`의 `OutputDir=`를 설정하세요.

## 2-1) 추천 설정(베타용)

- `SkyrimDiag.ini`
  - `CrashHookMode=1` 권장 (정상 동작 중 C++ 예외 throw/catch 같은 오탐을 줄이는 데 도움)
- `SkyrimDiagHelper.ini`
  - `DumpMode=1` 기본 권장 (FullMemory는 파일이 매우 커질 수 있음)
  - “fault module을 특정하지 못함”이 반복되면 **해당 문제 상황에서만** `DumpMode=2`로 올려 재캡처

## 3) 캡처 방식

### A. 실제 CTD(게임이 튕김)

- 게임이 튕긴 직후 생성된 `*_Crash_*.dmp`를 기준으로 확인합니다.

### B. 프리징/무한로딩 (자동 감지)

- 프리징/무한로딩이 일정 시간 이상 지속되면 `*_Hang_*.dmp`가 생성됩니다.
- 모드팩마다 로딩 시간이 크게 다르기 때문에, `SkyrimDiagHelper.ini`의 임계값이 중요합니다:
  - `HangThresholdInGameSec` : 인게임 프리징 기준(기본 10초)
  - `HangThresholdLoadingSec` : 로딩 화면 기준(기본 600초)
  - `EnableAdaptiveLoadingThreshold=1` 이면 최근 로딩 시간을 학습해 자동 보정합니다(추천).

### C. 수동 스냅샷(핫키)

- `Ctrl+Shift+F12` : 현재 상태를 “스냅샷”으로 캡처합니다.
- 정상 상태에서 찍은 수동 스냅샷은 “문제가 있다”는 의미가 아닐 수 있습니다.
  - 진단은 **문제 상황(프리징/무한로딩/CTD 직전)** 에서 캡처했을 때 가장 유효합니다.

## 3-1) “빠른 재현” 테스트(가능한 경우)

CTD가 잘 안 나는 모드팩에서는, 베타 검증을 위해 “기능이 동작하는지”만 빠르게 확인할 수 있습니다.

- `SkyrimDiag.ini`에서 `EnableTestHotkeys=1`
  - `Ctrl+Shift+F10` : 의도적 크래시(CTD 덤프 생성 확인)
  - `Ctrl+Shift+F11` : 의도적 행(프리징) 유발(행 감지/WCT/덤프 생성 확인)

> 주의: 테스트 후에는 `EnableTestHotkeys=0` 으로 되돌리는 것을 권장합니다.

## 4) DumpTool로 보는 법(유저 기준)

- `.dmp`를 `SkyrimDiagDumpTool.exe`에 드래그 앤 드롭하거나 실행 후 파일을 선택합니다.
- 탭 가이드:
  - **요약**: “결론”을 한 문장으로 표시(신뢰도 포함)
  - **근거**: 왜 그렇게 판단했는지(콜스택/스택 스캔/리소스 충돌/WCT 등)
  - **이벤트**: 크래시/행 직전 어떤 이벤트가 있었는지(상관관계 단서)
  - **리소스**: 최근 로드된 `.nif/.hkx/.tri` 및 MO2 제공자(충돌 단서)
  - **WCT**: 프리징 시 스레드가 무엇을 기다리는지(데드락/바쁜 대기 추정)

## 4-1) 자주 하는 오해(중요)

- “유력 후보”는 **확정 원인**이 아니라 “가능성 높은 후보”입니다.
- “수동 캡처(스냅샷)”은 정상 상태에서도 찍힐 수 있으며, 이것만으로 문제를 단정하지 않습니다.
- 로딩이 매우 긴 모드팩은 자동 프리징 감지 임계값이 높아야 오탐이 줄어듭니다(Adaptive 추천).

## 5) 이슈 리포트(필수 정보)

원인 분석 정확도를 높이려면 아래 정보가 필요합니다.

### 필수 첨부 파일

- 문제 상황의 `*.dmp` 1개
- 같은 이름의:
  - `*_SkyrimDiagReport.txt`
  - `*_SkyrimDiagSummary.json`
  - `*_SkyrimDiagBlackbox.jsonl` (있다면)
  - `SkyrimDiag_WCT_*.json` (있다면)
- (있다면) Crash Logger SSE/AE의 `crash-*.log`

### 함께 적어주세요

- 게임 버전(SE/AE/VR) + 실행 파일 버전(가능하면)
- SKSE 버전
- MO2 사용 여부(대부분 yes) + “단일 프로필인지”
- 문제 유형: CTD / 무한로딩 / 프리징 / 프레임드랍(히치)
- 재현 조건(어디서, 무엇을 하면, 로딩 시간 등)

### 이슈 템플릿(복사해서 사용)

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
- Crash Logger crash-*.log: (있으면)

[추가 메모]
- 최근 설치/업데이트한 모드/플러그인:
- 추정 원인 또는 의심 모드:
```

## 6) 보안/개인정보 주의

- 덤프/로그에는 **PC 경로(드라이브 문자/유저명)** 가 포함될 수 있습니다.
- 공개 업로드가 부담되면, 경로가 보이는 스크린샷/로그는 일부 마스킹 후 공유해주세요.
