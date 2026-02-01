# ADR-0003: DumpTool Core/UI Split

## Status
Accepted

## Context
DumpTool(`SkyrimDiagDumpTool.exe`)은 “덤프를 WinDbg 없이 분석 + GUI 뷰어 제공”을 목표로 하며, 현재는 단일 EXE 타겟 안에 다음 책임이 함께 존재합니다:
- minidump 파싱/스택 분석/휴리스틱 기반 의심 모듈 추정
- 증거/요약/체크리스트 생성(Evidence rules)
- 결과 파일(JSON/TXT) 출력
- Win32 GUI(탭/ListView/커스텀 드로우/레이아웃)

기능은 빠르게 발전했지만, 코드 결합도가 높아지며 다음 문제가 커졌습니다:
- UI 변경이 분석/출력 코드에 영향을 주기 쉬움(회귀 위험 증가)
- 큰 파일(`Analyzer.cpp`, `EvidenceBuilder.cpp`, `GuiApp.cpp`)이 비대해져 유지보수 난이도 상승
- 향후 “헤드리스 분석”, “별도 테스트”, “CLI/자동화” 확장 시 경계가 불명확

## Decision
DumpTool을 **분석 엔진(core)**과 **UI(exe)**로 분리합니다:
- `SkyrimDiagDumpToolCore` (static library)
  - 덤프 분석(`Analyzer.*`), 증거 생성(`EvidenceBuilder.*`), Crash Logger/MO2 매핑, 출력(`OutputWriter.*`) 등 “비-UI” 로직 담당
  - Windows DbgHelp/nlohmann-json 등의 의존성은 core 사용 요구사항으로 선언
- `SkyrimDiagDumpTool` (WIN32 exe)
  - entrypoint(`main.cpp`) + UI(`GuiApp.*`)만 담당
  - core 라이브러리를 링크하여 분석 결과를 UI로 표시

목표는 **동작 변경 없이** 모듈 경계를 만들고, 이후 리팩터링/테스트 도입의 기반을 마련하는 것입니다.

## Consequences

### Positive
- UI 코드 변경이 분석 엔진에 미치는 영향 감소(결합도↓)
- 분석 로직을 다른 프론트엔드(헤드리스/CLI/테스트)에서 재사용하기 쉬움
- 빌드/링크 단계에서 책임이 더 명확해짐

### Negative
- CMake 타겟이 늘어나며 초기 설정 복잡도가 약간 증가
- `nlohmann_json`처럼 UI에서도 쓰는 의존성은 core/exe 경계에서 “전파(usage requirements)”를 신경 써야 함

### Neutral
- 큰 파일 분리는 별도 단계(점진적)로 진행 가능

## Alternatives Considered
- **단일 EXE 유지 + 파일만 분리**
  - 장점: 타겟 구조는 단순
  - 단점: “UI vs 분석” 경계가 빌드/의존성 레벨에서 강제되지 않아, 결합도 감소 효과가 제한적

## References
- `docs/plans/2026-01-30-architecture-review-and-refactor-plan.md`
- `docs/plans/2026-01-31-quality-pass-plan.md`

