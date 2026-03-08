# CTD Logger 우선순위 개선 구현 계획

> 작성일: 2026-03-08
> 기준 버전: v0.2.42
> 목표: CTD 로거의 분석 정확도, 사용자 피드백 루프, 예방 진단, 지원 UX를 우선순위 기반으로 순차 개선

## 배경

현재 파이프라인은 `plugin -> helper -> dump_tool -> WinUI` 구조가 안정화되어 있고, Windows 빌드/패키징/릴리스 게이트도 통과 가능한 상태다. 다음 단계의 핵심은 기능 폭을 더 넓히는 것보다 다음 3가지를 강화하는 데 있다.

1. 분석 결과가 실제로 맞았는지 제품 안에서 학습 가능한 피드백 루프 만들기
2. 덤프 품질과 분석 품질을 분리해 오탐 체감을 줄이기
3. 사용자가 재현/제보/예방을 더 쉽게 하도록 UX를 정리하기

## 우선순위

### 1. WinUI triage 입력 + bucket quality 피드백 루프

- 목표:
  - 사용자가 WinUI에서 `review_status`, `verdict`, `actual_cause`, `ground_truth_mod`, `notes`를 직접 남길 수 있게 한다.
  - 요약 JSON의 `triage` 블록에 저장해 재분석 시에도 유지한다.
  - `scripts/analyze_bucket_quality.py`가 읽는 ground truth 데이터를 실제 제품 UX와 연결한다.
- 수정 범위:
  - `dump_tool_winui/MainWindow.xaml`
  - `dump_tool_winui/MainWindow.xaml.cs`
  - `dump_tool_winui/AnalysisSummary.cs`
  - 관련 source-guard / schema 테스트
- 완료 기준:
  - WinUI에서 triage 저장 가능
  - 재오픈 시 저장 값이 다시 로드됨
  - bucket-quality 스크립트가 기대하는 필드가 계속 유지됨

### 2. capture quality / analysis quality 분리 표시

- 목표:
  - 같은 low confidence라도 “캡처 자체가 애매한 경우”와 “분석 증거가 약한 경우”를 분리해 보여준다.
  - first-chance, snapshot/manual capture, symbol 부족, blackbox 부재 같은 사유를 설명 가능한 필드로 만든다.
- 수정 범위:
  - `dump_tool/src/Analyzer.cpp`
  - `dump_tool/src/OutputWriter.cpp`
  - `dump_tool_winui/AnalysisSummary.cs`
  - `dump_tool_winui/MainWindow.xaml(.cs)`
- 완료 기준:
  - summary JSON에 두 품질 축이 별도 노출
  - WinUI에 각각의 등급과 이유가 표시됨

### 3. 리소스 로그 + 모드 인덱스 기반 상관분석

- 목표:
  - 크래시 직전 리소스 접근을 모드 소유 정보와 묶어 suspect score 보조 신호로 사용한다.
  - 콜스택이 애매한 덤프에서도 “방금 건드린 모드”를 더 잘 잡는다.
- 수정 범위:
  - `helper`의 리소스/블랙박스 출력
  - `dump_tool`의 리소스 집계 및 점수 반영
  - MO2/Vortex 경로 해석 계층
- 완료 기준:
  - 최근 리소스 이벤트가 모드 단위로 집계됨
  - suspect ranking에 보조 가중치가 반영됨

### 4. Compatibility Preflight 확장

- 목표:
  - 충돌 후 분석보다 먼저, 흔한 설치/버전 실수를 더 많이 잡는다.
  - SKSE/게임 버전 불일치, 플러그인 슬롯 과다, 중복 크래시 핸들러, 알려진 비호환 조합을 사전 경고한다.
- 수정 범위:
  - `helper/src/CompatibilityPreflight.cpp`
  - 관련 룰 데이터
  - guard tests
- 완료 기준:
  - 대표 설치 오류 시나리오가 경고로 노출
  - 릴리즈/회귀 테스트에 케이스가 고정됨

### 5. 지원용 번들(export) 기능

- 목표:
  - 사용자가 지원 요청 시 필요한 파일을 한 번에 묶어 보낼 수 있게 한다.
  - summary/report/incident/WCT/blackbox/log를 privacy-safe 방식으로 export한다.
- 수정 범위:
  - `dump_tool_winui`
  - `scripts/package.py` 또는 별도 export 유틸
  - redaction / manifest 로직
- 완료 기준:
  - WinUI에서 선택한 분석 결과 기준 지원용 zip 생성 가능
  - 민감 경로가 기본적으로 정리됨

### 6. JSON 룰/로더 공통 검증 레이어

- 목표:
  - signatures, hook frameworks, plugin rules, graphics rules, troubleshooting guide가 제각각 로드되는 문제를 줄인다.
  - malformed 항목을 무음 무시하지 않고 공통 경고/검증 흐름을 갖춘다.
- 수정 범위:
  - `dump_tool/src` 공통 로더 유틸
  - 데이터 파일 및 malformed fixture tests
- 완료 기준:
  - 공통 version / required-key 검증 적용
  - 데이터 포맷 회귀가 테스트에서 즉시 드러남

## 구현 순서

### Phase 1

1. 우선순위 1 구현
2. triage 결과가 실제 bucket quality 리포트와 이어지는지 검증
3. WinUI 저장 UX와 summary 스키마를 안정화

### Phase 2

1. 우선순위 2 구현
2. quality reason 텍스트와 UI 배지 추가

### Phase 3

1. 우선순위 3, 4 병행 설계
2. 먼저 Preflight quick win을 넣고, 이후 리소스-모드 상관분석으로 확장

### Phase 4

1. 우선순위 5 지원 번들 기능 추가
2. 우선순위 6 공통 로더/검증 레이어로 데이터 안전성 정리

## 이번 턴 구현 범위

이번 작업에서는 우선순위 1만 실제 코드로 구현한다.

- WinUI triage 편집 UI 추가
- triage 값을 summary JSON에 저장
- 재로딩 시 편집 값 복원
- 관련 테스트 보강

## 검증 계획

- Linux 빠른 검증:
  - `cmake -S . -B build-linux-test -G Ninja`
  - `cmake --build build-linux-test`
  - `ctest --test-dir build-linux-test --output-on-failure`
- Windows 확인이 필요할 경우:
  - `scripts\\build-win.cmd`
  - `scripts\\build-winui.cmd`

