# Symbolized Stack + Crash Bucketing + ETW Hang Capture Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** DumpTool의 원인 후보 정확도를 높이기 위해 심볼 기반 스택 프레임 표기를 추가하고, 반복 CTD를 자동 묶는 버킷 키를 출력하며, Helper에 선택적 ETW(Hang 구간) 캡처를 추가한다.

**Architecture:** 기존 진단 파이프라인(Plugin → Helper dump/WCT → DumpTool)을 유지한 채, 분석 산출물에 `bucket`/`symbolized callstack`을 확장한다. ETW는 기본 OFF의 옵션 기능으로 Helper에서 외부 `wpr.exe`를 best-effort 호출한다. 실패 시 진단 파이프라인은 계속 동작한다.

**Tech Stack:** C++20, DbgHelp(Sym*), Win32 CreateProcess, CMake, CTest, GitHub Actions.

---

### Task 1: 버킷 키 생성 로직을 테스트 우선으로 추가

**Files:**
- Create: `dump_tool/src/Bucket.h`
- Create: `tests/bucket_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: failing test 작성**
- 동일 입력이면 같은 bucket key
- top frame/module/exception 조합이 바뀌면 key가 달라짐

**Step 2: RED 검증**
- `cmake --build build-linux --target skydiag_bucket_tests` (실패 예상)

**Step 3: 최소 구현**
- 안정적 문자열 직렬화 + FNV1a64 hex key 생성

**Step 4: GREEN 검증**
- `cmake --build build-linux --target skydiag_bucket_tests`
- `ctest --test-dir build-linux --output-on-failure`

---

### Task 2: DumpTool 분석 결과에 bucket + 심볼화 프레임 반영

**Files:**
- Modify: `dump_tool/src/Analyzer.h`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/src/OutputWriter.cpp`

**Step 1: AnalysisResult 확장**
- `crash_bucket_key` 필드 추가

**Step 2: symbolized frame formatter 추가**
- 기존 주소 기반 프레임 포맷팅 경로에 `SymFromAddrW`/`SymGetLineFromAddrW64` best-effort 적용
- 심볼 실패 시 기존 `module+offset` 폴백 유지

**Step 3: bucket 산출 적용**
- exception code + fault module + top frames 기반 bucket 계산

**Step 4: output 반영**
- Summary JSON / Report 텍스트에 `crash_bucket_key` 출력

**Step 5: 빌드 검증**
- Windows: `scripts\build-win.cmd`

---

### Task 3: Helper에 ETW 옵션 캡처(기본 OFF) 추가

**Files:**
- Modify: `helper/include/SkyrimDiagHelper/Config.h`
- Modify: `helper/src/Config.cpp`
- Modify: `helper/src/main.cpp`
- Modify: `dist/SkyrimDiagHelper.ini`
- Modify: `README.md`

**Step 1: config 확장**
- `EnableEtwCaptureOnHang=0`
- `EtwWprExe=wpr.exe`
- `EtwProfile=`(기본 GeneralProfile)
- `EtwMaxDurationSec`(짧은 버퍼 캡처 용도)

**Step 2: hang capture 시점 연동**
- hang dump 직전에 best-effort `wpr -start`
- dump 직후 `wpr -stop <out.etl>` 실행
- 실패해도 dump/WCT 흐름 유지

**Step 3: 문서/ini 갱신**
- 옵션은 고급 기능, 기본 OFF 명시

**Step 4: 빌드/패키징 검증**
- Windows: `scripts\build-win.cmd`
- Windows: `python scripts\package.py --build-dir build-win --out dist\Tullius_ctd_loger.zip`

---

### Task 4: 최종 검증

**Files:**
- (No additional file edits unless required)

**Step 1: Linux tests**
- `ctest --test-dir build-linux --output-on-failure`

**Step 2: 변경 점검**
- `git status --short`
- `git diff -- dump_tool/src/Analyzer.h dump_tool/src/Analyzer.cpp dump_tool/src/OutputWriter.cpp dump_tool/src/Bucket.h helper/include/SkyrimDiagHelper/Config.h helper/src/Config.cpp helper/src/main.cpp dist/SkyrimDiagHelper.ini README.md tests/CMakeLists.txt tests/bucket_tests.cpp docs/plans/2026-02-06-symbol-bucketing-and-etw-hang-plan.md`

**Step 3: 결과 보고**
- 기능 변화
- 검증 결과
- 제한 사항(ETW 환경 의존)
