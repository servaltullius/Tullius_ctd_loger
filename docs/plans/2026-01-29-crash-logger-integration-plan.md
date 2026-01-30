# Crash Logger SSE/AE Integration + Suspect Scoring Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** DumpTool이 Crash Logger SSE/AE 로그를 자동으로 찾아/파싱하고, minidump 스택 스캔 기반으로 “유력 후보 모드/DLL”을 더 정확하게 제시한다.

**Architecture:** DumpTool 분석 단계에서 (1) Crash Logger 로그 자동 탐색/파싱(선택) (2) minidump ThreadListStream 스택 스캔으로 모듈 점수화 (3) 결과를 Evidence/Report/JSON/UI에 반영한다.

**Tech Stack:** C++20(Win32), DbgHelp(minidump), nlohmann/json, std::filesystem, SHGetKnownFolderPath(KnownFolders)

---

## Task 1: Crash Logger 로그 자동 탐색

**Files:**
- Modify: `dump_tool/src/Analyzer.cpp`

**Steps:**
1. `SHGetKnownFolderPath(FOLDERID_Documents)`로 Documents 경로를 얻는다.
2. 후보 디렉터리를 구성한다(존재하는 것만):
   - `Documents\\My Games\\Skyrim Special Edition\\SKSE\\`
   - `Documents\\My Games\\Skyrim VR\\SKSE\\`
   - 위 경로의 `CrashLogger/CrashLogs/Crashlogs` 서브폴더(있을 때)
3. 덤프 파일명에서 `_YYYYMMDD_HHMMSS` 타임스탬프를 파싱(실패 시 last_write_time 사용).
4. 후보 디렉터리의 `.log/.txt` 파일들을 열어 앞부분에서 `CrashLogger` + `PROBABLE CALL STACK` 포함 여부로 필터링하고, 덤프 시간과 가장 가까운 로그 1개를 선택한다.

---

## Task 2: Crash Logger 로그 파싱 → 모듈/모드 후보 생성

**Files:**
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/src/Analyzer.h`

**Steps:**
1. 로그에서 `PROBABLE CALL STACK:` 섹션을 찾아 다음 섹션(빈 줄 또는 `REGISTERS:`) 전까지 라인을 파싱한다.
2. 각 라인에서 `Something.dll+0123ABCD` / `SkyrimSE.exe+...` 패턴을 추출하고 모듈 파일명 빈도수를 집계한다.
3. minidump의 `ModuleListStream`에서 동일 파일명을 가진 모듈의 full path를 찾아 `mods\\<ModName>` 기반으로 모드명을 추정한다.
4. 상위 N개의 모듈/모드를 Evidence/Recommendations에 “추가 근거”로 표시한다.

---

## Task 3: minidump 스택 스캔 기반 “유력 후보” 점수화

**Files:**
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/src/Analyzer.h`

**Steps:**
1. `ModuleListStream`를 벡터로 로드( base, size, path, filename ).
2. `ThreadListStream`에서 대상 스레드(우선순위):
   - `ExceptionStream.ThreadId` (CTD)
   - WCT JSON에서 `isCycle=true` 스레드들(행/프리징)
   - (fallback) 마지막 Heartbeat 이벤트의 tid
3. 각 스레드 스택 메모리에서 `Rsp` 이후 영역을 포인터 단위로 스캔하여 “모듈 주소로 보이는 값”을 카운트한다.
4. 시스템 DLL/게임 EXE는 점수 계산에서 감산/제외(표시는 가능).
5. 상위 후보(Top 3~5)를 “유력 후보”로 Evidence/summary_sentence에 반영한다.

---

## Task 4: 출력/뷰어 반영

**Files:**
- Modify: `dump_tool/src/Analyzer.cpp` (WriteOutputs JSON/Report 확장)
- Modify: `dump_tool/src/GuiApp.cpp` (요약/근거 탭에 추가 표시)

**Steps:**
1. `*_SkyrimDiagSummary.json`에 `crash_logger`(log_path, top_modules) 및 `suspects`(모듈/모드/점수)를 추가한다.
2. `*_SkyrimDiagReport.txt`에도 동일 내용을 사람이 읽기 쉬운 형태로 추가한다.
3. GUI에서 Evidence 탭에 후보들이 자연스럽게 보이도록(신뢰도 배지 컬럼 유지) 항목 순서를 조정한다.

---

## Task 5: 빌드/패키징/검증

**Files:**
- N/A (scripts 사용)

**Steps:**
1. Windows 빌드: `C:\\Users\\kdw73\\SkyrimDiag\\scripts\\build-win.cmd`
2. 패키징: `python3 scripts/package.py --build-dir build-win --out dist/SkyrimDiag.zip`
3. 샘플 덤프/로그로 DumpTool을 실행해:
   - 로그 자동 발견/파싱이 동작하는지
   - Evidence/Report/JSON에 후보가 출력되는지 확인한다.

