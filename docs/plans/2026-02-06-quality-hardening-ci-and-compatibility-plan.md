# Quality Hardening + CI + Compatibility Docs Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 테스트 커버리지와 자동 검증 경로를 보강하고, 프로토콜 버전 운영 전략을 ADR로 명시해 릴리스 안정성을 높인다.

**Architecture:** 기존 런타임 동작은 변경하지 않고, 1) Linux에서 즉시 실행 가능한 파서/억제 로직 테스트를 확장하고, 2) GitHub Actions에 기본 CI를 추가해 회귀를 조기 감지하며, 3) SharedLayout 버전 호환 정책을 ADR로 문서화한다. 코드 변경은 최소(diff-minimal) 원칙으로 유지한다.

**Tech Stack:** C++20, CMake + CTest, GitHub Actions, Markdown ADR.

---

### Task 1: CrashLogger/Hang Suppression 테스트 보강

**Files:**
- Modify: `tests/crashlogger_parser_tests.cpp`
- Modify: `tests/hang_suppression_tests.cpp`

**Step 1: CrashLogger 파서 회귀 케이스 추가**
- `C++ EXCEPTION:` 블록에서 key/value 공백이 달라도 파싱되는지 테스트 추가
- `THREAD DUMP`에서 시스템 DLL/게임 EXE 필터링이 유지되는지 테스트 추가

**Step 2: Hang suppression 경계값 케이스 추가**
- `foregroundGraceSec=0` 일 때 즉시 억제가 해제되는지 테스트 추가
- `qpcFreq=0` 일 때 비정상 suppress 상태로 고정되지 않는지 테스트 추가

**Step 3: 테스트 실행**
- Run: `cmake --build build-linux --target skydiag_hang_suppression_tests skydiag_crashlogger_parser_tests`
- Run: `ctest --test-dir build-linux --output-on-failure`
- Expected: 기존 + 신규 테스트 모두 PASS

---

### Task 2: GitHub Actions CI 추가 (Linux smoke)

**Files:**
- Create: `.github/workflows/ci.yml`
- Modify: `README.md`

**Step 1: CI 워크플로 추가**
- Ubuntu에서 configure/build/test를 수행하는 워크플로 작성
- 트리거: `push`, `pull_request`
- 검증 대상: `skydiag_hang_suppression_tests`, `skydiag_crashlogger_parser_tests`, `skydiag_i18n_core_tests`

**Step 2: README에 CI 섹션 추가**
- 로컬과 CI 동일 명령(`cmake -S . -B build-linux -G Ninja`, `cmake --build`, `ctest`)을 명시

**Step 3: YAML/문서 정적 점검**
- Run: `sed -n '1,220p' .github/workflows/ci.yml`
- Run: `rg -n "CI|ctest --test-dir build-linux|GitHub Actions" README.md`
- Expected: 워크플로/문서 경로와 명령이 일치

---

### Task 3: 프로토콜 버전 운영 전략 ADR 추가

**Files:**
- Create: `docs/adr/0004-sharedlayout-versioning-and-compatibility-policy.md`
- Modify: `docs/adr/README.md`

**Step 1: ADR 문서 작성**
- SharedLayout `kVersion` 변경 기준
- Helper의 strict mismatch fail-fast 정책 근거
- 릴리스 번들링/업데이트 가이드(플러그인+헬퍼 동시 배포)
- 향후 확장 전략(필드 추가 시 reserved/version bump 원칙)

**Step 2: ADR 인덱스 갱신**
- `docs/adr/README.md`에 ADR-0004 추가

**Step 3: 문서 점검**
- Run: `rg -n "ADR-0004|SharedLayout|kVersion|fail-fast|compat" docs/adr`
- Expected: 새 ADR과 인덱스가 연결됨

---

### Task 4: 최종 검증 + 변경 요약

**Files:**
- (No additional file edits unless fix needed)

**Step 1: 전체 테스트 재실행**
- Run: `ctest --test-dir build-linux --output-on-failure`
- Expected: PASS

**Step 2: 변경 파일 검토**
- Run: `git status --short`
- Run: `git diff -- tests/crashlogger_parser_tests.cpp tests/hang_suppression_tests.cpp .github/workflows/ci.yml README.md docs/adr/README.md docs/adr/0004-sharedlayout-versioning-and-compatibility-policy.md`

**Step 3: 결과 보고**
- 무엇을 바꿨는지
- 왜 바꿨는지
- 어떻게 검증했는지
