# Tullius CTD Logger Quality Pass Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task.

**Goal:** “기능 변경 없이” DumpTool 중심으로 코드리뷰/리팩터링/디커플링을 진행해 유지보수성과 확장성을 올리고, 릴리즈 리스크를 낮춘다.

**Architecture:** `dump_tool`을 `core(분석 엔진)`과 `ui(뷰어)`로 나누고, 큰 파일(`Analyzer.cpp`, `EvidenceBuilder.cpp`, `GuiApp.cpp`)의 책임을 분리한다. 동작을 바꾸지 않는 범위에서 모듈 경계/인터페이스를 명확히 한다.

**Tech Stack:** C++20, CMake, Windows API, DbgHelp, nlohmann_json

---

## Context / Constraints

- 테스트 프레임워크가 현재 CMake에 없음 → “리팩터링=동작 유지”를 최우선으로 하고, 매 단계 Windows 빌드로 회귀를 방지한다.
- MO2 환경/일반 유저 UX가 목표 → 경로/권한/오탐(정상 상태 스냅샷) 안내 메시지는 “정직한 신뢰도”로 유지한다.
- Helper/Plugin은 이미 “정상 종료시 hang dump 생성 방지”가 반영되어 있으므로, 이번 패스는 **DumpTool 유지보수성**에 집중한다.

---

## Code Review (요약)

### 강점
- Plugin/Helper/DumpTool 3-계층 구조가 명확하고, out-of-proc 덤프 생성은 안정성/오버헤드 측면에서 적절.
- SharedLayout + seqlock(odd/even) 패턴은 락 없이도 Reader 일관성을 확보하는 합리적 선택.

### 리스크 / 개선 포인트(우선순위)
1. DumpTool 단일 EXE에 “분석/규칙/보고서/UI”가 과도하게 결합되어 있고, 큰 파일이 많아 회귀 위험이 큼.
2. `Analyzer.cpp` 내부에 “덤프 읽기 + 스택워크/스택스캔 + 의심 모듈 스코어링”이 함께 있어 변경이 어렵다.
3. UI(`GuiApp.cpp`)는 빠르게 진화했지만, 컨트롤 생성/레이아웃/커스텀 드로우가 한 파일에 집중되어 유지보수성이 떨어진다.

---

## Tasks

### Task 1: DumpTool을 `core`와 `ui`로 분리 (저위험/구조)

**Files:**
- Modify: `dump_tool/CMakeLists.txt`
- Create: `dump_tool/src/Core.h`
- Create: `dump_tool/src/Core.cpp`
- Modify: `dump_tool/src/main.cpp`

**Step 1:** `dump_tool_core`(static library) 타겟을 추가하고, 기존 소스 중 “분석/출력” 계층을 core로 이동한다.  
**Step 2:** `SkyrimDiagDumpTool`(WIN32 exe)은 UI + entrypoint만 유지하고 `dump_tool_core`를 링크한다.

**Verify:** Windows에서 `scripts\\build-win.cmd`가 성공한다.

---

### Task 2: `Analyzer.cpp` 책임 분리 (저위험/파일 분리)

**Files:**
- Create: `dump_tool/src/MinidumpRead.h`
- Create: `dump_tool/src/MinidumpRead.cpp`
- Create: `dump_tool/src/Stackwalk.h`
- Create: `dump_tool/src/Stackwalk.cpp`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/CMakeLists.txt`

**Step 1:** `MinidumpRead.*`에 “minidump 스트림/메모리 범위 읽기” 유틸을 이동한다.  
**Step 2:** `Stackwalk.*`에 StackWalk64 + 메모리 read callback 관련 코드를 이동한다.

**Verify:** 빌드 성공 + 뷰어 실행/덤프 로드 동작이 동일.

---

### Task 3: `EvidenceBuilder.cpp` 규칙 분리 (중간/리스크 낮춤)

**Files:**
- Create: `dump_tool/src/EvidenceRules.h`
- Create: `dump_tool/src/EvidenceRules.cpp`
- Modify: `dump_tool/src/EvidenceBuilder.cpp`
- Modify: `dump_tool/CMakeLists.txt`

**Step 1:** “규칙/휴리스틱”을 `EvidenceRules.*`로 이동하고, `EvidenceBuilder`는 orchestration만 담당한다.

**Verify:** 동일 덤프 입력에 대해 요약/근거/권장조치 출력 형식이 유지된다(동작 변경 없음).

---

### Task 4: UI 파일 분리(선택) + 리소스 관리 개선(저위험)

**Files:**
- (선택) Create: `dump_tool/src/GuiLayout.*`, `dump_tool/src/GuiDraw.*`
- Modify: `dump_tool/src/GuiApp.cpp`

**Step 1:** 컨트롤 생성/레이아웃/커스텀 드로우를 파일 단위로 분리한다(동작 유지).  
**Step 2:** 반복 생성되는 GDI 리소스(brush/pen/font 등)는 RAII/캐시로 누수/오버헤드 위험을 줄인다.

---

### Task 5: 패키징/검증(필수)

**Files:**
- Modify: (필요시) `scripts/package.py` 또는 `dist/*`

**Step 1:** Windows mirror로 동기화 후 `scripts\\build-win.cmd` 실행  
**Step 2:** `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger.zip`  
**Step 3:** `--no-pdb` zip도 생성하고 해시를 기록한다.

---

## Non-goals (이번 패스에서 안 함)
- “원인 모드 확정” 같은 새로운 분석 기능 추가(정확도 실험은 별도 이슈/플랜에서).
- 테스트 프레임워크 도입(프로젝트 합의 후 별도 작업).

