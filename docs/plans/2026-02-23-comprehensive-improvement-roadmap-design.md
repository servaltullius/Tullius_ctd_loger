# Tullius CTD Logger 종합 개선 로드맵

> 작성일: 2026-02-23
> 기준 버전: v0.2.41-rc1
> 목표: 분석 정확도 + UX + 코드 품질을 균형 있게 개선

## 배경

v0.1.0-beta.1(2026-01-30) → v0.2.41-rc1(2026-02-23) 동안 41개 릴리즈를 거치며 핵심 기능(프리징/ILS 감지, WinUI 뷰어, 블랙박스 이벤트, WCT, 적응형 로딩 임계값)이 안정화되었다. 경쟁 도구(CrashLoggerSSE, Trainwreck, Phostwood Analyzer) 대비 독보적 기능을 갖추고 있으나, 분석 결과의 활용도와 내부 품질에 개선 여지가 있다.

## 경쟁 도구 벤치마킹 요약

| 도구 | 강점 | 약점 |
|------|------|------|
| CrashLoggerSSE | 생태계 호환성 최고, PDB 심볼 해석 | 로그가 700줄 텍스트, 초보자 해석 불가 |
| Trainwreck | 버전 독립적, PDB 지원 | 자동 분석 도구 호환 미흡 |
| Phostwood Analyzer | 패턴 매칭 진단율 75-90%, 단계별 가이드 | 웹 기반, 프리징/ILS 미지원 |
| **Tullius (현재)** | **프리징/ILS 감지, Out-of-proc, WinUI, 블랙박스, WCT** | 히스토리 상관분석 미흡, 커뮤니티 공유 포맷 부재 |

## 개선 항목 — 7개 (우선순위순)

### Phase 1: Quick Wins (낮은 공수, 즉시 효과)

#### 1. C4: Dual Namespace 통합
- **문제**: `minidump::IsKnownHookFramework`와 `internal::IsKnownHookFramework`가 동일 로직을 별도 유지. 동기화 리스크.
- **해결**: `MinidumpUtil.h`에 단일 공개 API로 통합. `EvidenceBuilderInternalsUtil.cpp`에서 호출.
- **수정 파일**: `dump_tool/src/MinidumpUtil.h`, `dump_tool/src/MinidumpUtil.cpp`, `dump_tool/src/EvidenceBuilderInternalsUtil.cpp`
- **공수**: 0.5h
- **검증**: 기존 `hook_framework_guard_tests` 통과 확인

#### 2. C2: JSON 데이터 파일 스키마 검증
- **문제**: `hook_frameworks.json`, `crash_signatures.json`, `plugin_rules.json`, `graphics_injection_rules.json` 로드 시 구조 검증 없음. 잘못된 항목이 무음 무시.
- **해결**:
  - 모든 JSON 데이터 파일에 `version` 필드 필수화
  - 로드 시 필수 키 존재 + 타입 검증 (`nlohmann/json .at()` + 예외 catch)
  - 잘못된 항목 스킵 + 경고 로그 (이미 `crash_signatures.json`에 적용된 패턴 확산)
- **수정 파일**: `dump_tool/src/MinidumpUtil.cpp` (hook frameworks loader), `dump_tool/src/Analyzer.cpp` (signatures loader), `dump_tool/src/PluginRules.cpp`, `dump_tool/data/*.json`
- **공수**: 2h
- **검증**: 기존 JSON 테스트 + malformed JSON fixture 추가

#### 3. A4: Preflight 환경 검증 확장
- **문제**: Preflight에 CrashLogger 중복, BEES 체크만 있음. 가장 흔한 설치 실수(버전 불일치, 플러그인 수 초과)를 놓침.
- **해결**:
  - SKSE 버전 ↔ Skyrim 버전 호환성 확인
  - 비-ESL 플러그인 254개 초과 경고
  - `plugin_rules.json`에 알려진 비호환 모드 조합 몇 가지 추가
- **수정 파일**: `helper/src/CompatibilityPreflight.cpp`, `dump_tool/data/plugin_rules.json`
- **공수**: 3h
- **검증**: `helper_preflight_guard_tests.cpp` 확장

#### 4. B1: Discord/Reddit 커뮤니티 공유용 요약 복사
- **문제**: "Copy summary" 버튼이 플레인 텍스트. 커뮤니티에 붙여넣기하면 가독성 낮음.
- **해결**:
  - 마크다운 + 이모지 포맷의 "커뮤니티 공유용 복사" 버튼 추가
  - 유력 원인, 크래시 유형, confidence, 권장 조치를 압축 포맷으로 제공
  - 히스토리 데이터 포함 (A1 완료 후)
- **포맷 예시**:
  ```
  🔴 Skyrim CTD — SkyrimDiag v0.2.41
  📌 유력 원인: ExampleMod.dll (HIGH)
  🔍 유형: ACCESS_VIOLATION (Read @ 0x0)
  💡 조치: ExampleMod 업데이트/비활성화 후 재현 확인
  🛠️ Tullius CTD Logger
  ```
- **수정 파일**: `dump_tool_winui/MainWindow.xaml.cs`, `dump_tool_winui/MainWindow.xaml`
- **공수**: 1.5h
- **검증**: 수동 테스트 (클립보드 복사 → Discord 붙여넣기)

### Phase 2: 중간 공수, 높은 영향

#### 5. A1: 크래시 히스토리 상관 분석 강화
- **문제**: `crash_history.json`에 모듈별 카운트만 있고, 동일 패턴 반복 감지가 없음. 사용자에게 "이전 크래시와 같은 원인인지" 알려주지 않음.
- **해결**:
  - `crash_history.json` 스키마에 `bucket_key` 인덱스 추가 (발생 횟수, first_seen, last_seen)
  - 분석 시 동일 bucket_key 매칭 → confidence 1단계 상향
  - Summary JSON에 `history_correlation` 필드 추가
  - WinUI 요약에 "이전에 동일 패턴 N회 발생" 배지 표시
- **수정 파일**: `dump_tool/src/Analyzer.cpp`, `dump_tool/src/AnalyzerInternalsSummary.cpp`, `dump_tool/src/CrashHistory.cpp`(신규 또는 기존 확장), `dump_tool_winui/AnalysisSummary.cs`, `dump_tool_winui/MainWindow.xaml.cs`
- **공수**: 6h
- **검증**: `analysis_engine_runtime_tests.cpp` 확장 + WinUI 수동 검증

#### 6. B3: 단계별 트러블슈팅 가이드
- **문제**: Recommendations가 일반적. 크래시 유형별 구체적 단계가 없음. Phostwood Analyzer의 핵심 강점.
- **해결**:
  - `dump_tool/data/troubleshooting_guides.json` 신규 데이터 파일
  - 크래시 유형(ACCESS_VIOLATION, D6DDDA, Freeze/ILS, C++ Exception 등)별 단계별 체크리스트
  - 한국어/영어 이중 언어 지원
  - `EvidenceBuilderInternalsRecommendations.cpp`에서 매칭 → Summary JSON에 `troubleshooting_steps` 배열 추가
  - WinUI에 접이식 체크리스트 UI
- **수정 파일**: `dump_tool/data/troubleshooting_guides.json`(신규), `dump_tool/src/EvidenceBuilderInternalsRecommendations.cpp`, `dump_tool_winui/MainWindow.xaml.cs`, `dump_tool_winui/MainWindow.xaml`
- **공수**: 5h
- **검증**: 가이드 매칭 로직 단위 테스트 + WinUI 수동 검증

#### 7. C3: CrashLogger 파서 테스트 보완
- **문제**: `CrashLogger.cpp`(605줄)가 CrashLoggerSSE v1.17~v1.20 포맷을 파싱하는데, 직접 단위 테스트가 없음. 포맷 변형이 많아 회귀 리스크 높음.
- **해결**:
  - `tests/crashlogger_parser_tests.cpp` 신규 테스트 파일
  - 버전별 fixture 파일: v1.17, v1.18 (C++ Exception 포함), v1.19, v1.20 형식
  - 테스트 항목: 버전 감지, 콜스택 파싱, C++ Exception 블록 파싱, threaddump 파싱
  - Linux에서도 실행 가능 (Windows API 의존성 없는 텍스트 파싱 로직)
- **수정 파일**: `tests/crashlogger_parser_tests.cpp`(신규), `tests/fixtures/crashlog_*.txt`(신규), `CMakeLists.txt` 테스트 타깃 추가
- **공수**: 4h
- **검증**: `ctest` 통과

## 실행 순서 요약

```
Phase 1 (Quick Wins, ~7h):
  1. C4 Dual namespace 통합     [0.5h]
  2. C2 JSON 스키마 검증         [2h]
  3. A4 Preflight 환경 검증 확장 [3h]
  4. B1 커뮤니티 공유 포맷       [1.5h]

Phase 2 (중간 공수, ~15h):
  5. A1 히스토리 상관 분석       [6h]
  6. B3 트러블슈팅 가이드        [5h]
  7. C3 CrashLogger 파서 테스트  [4h]
```

## 채택하지 않은 항목과 이유

| 항목 | 미채택 이유 |
|------|-----------|
| A3 리소스→모드 역추적 | MO2 VFS 경로 해석이 MO2 버전/설정에 강하게 의존. 범용성 보장 어려움. 추후 재검토 |
| A5 GPU 드라이버 교차검증 | WMI/DXGI 쿼리 필요. 공수 대비 커버하는 크래시 유형이 제한적 |
| B2 크래시 통계 대시보드 | WinUI에 차트 라이브러리 의존성 추가 필요. A1 완료 후 자연스럽게 확장 가능 |
| C1 WinUI MVVM 리팩터링 | 공수 높음(8h+). 기능 추가와 동시 진행하면 충돌 리스크. B1/B3 완료 후 별도 리팩터링 세션 권장 |
| C5 Config 검증 레이어 | INI 파서 교체 수준이라 단독으로는 ROI 낮음. C2와 합쳐서 추후 검토 |
