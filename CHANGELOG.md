# Changelog

## v0.2.41-rc3 (2026-02-24)

### 추가 (이벤트 로그 가독성 개선 — 베타 테스터 피드백 반영)
- DumpTool: `EventRow`에 `detail` 필드 추가 — 이벤트별 사람이 읽을 수 있는 요약 텍스트 자동 생성.
- DumpTool: `FormatEventDetail` 구현 — PerfHitch(`hitch=105.8s flags=Loading interval=1000ms`), MenuOpen/Close(FNV-1a 해시 → `Loading Menu` 등 32개 알려진 메뉴 이름 역해석), Heartbeat/CellChange 포맷.
- DumpTool: 텍스트 리포트 및 JSONL 블랙박스 출력에 `detail` 필드 포함.
- DumpTool: 프리징/큰 히치 직전 이벤트 컨텍스트 요약을 Evidence에 추가 — 10초 이내 주요 이벤트 흐름(`MenuOpen(Loading Menu) → PerfHitch(hitch=2.3s) → ...`) 표시.
- WinUI: 이벤트 탭에서 JSONL 파싱 후 `detail` 필드 기반 가독성 높은 포맷으로 표시 (기존 raw JSON 대체).

### 테스트
- 이벤트 가독성 가드 테스트 8개 추가 (`event_detail_guard_tests`).
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`41/41`).

## v0.2.41-rc2 (2026-02-23)

### 추가 (Phase 1 — Quick Wins)
- DumpTool: `internal::` 래퍼 함수 4개를 제거하고 `minidump::` 단일 네임스페이스로 통합해 동기화 리스크 제거 (C4).
- DumpTool: `hook_frameworks.json`, `crash_signatures.json`, `plugin_rules.json` 로드 시 `version` 필드 필수화 + 잘못된 항목 스킵/경고 로그 (C2).
- Helper: Preflight에 비-ESL 플러그인 240개 초과 경고(`FULL_PLUGIN_SLOT_LIMIT`)와 알려진 비호환 모드 조합(`KNOWN_INCOMPATIBLE_COMBO`) 체크 추가 (A4).
- WinUI: Discord/Reddit 커뮤니티 공유용 이모지+마크다운 포맷 복사 버튼(`CopyShareButton`) 추가 (B1).

### 추가 (Phase 2 — 중간 공수)
- DumpTool: 크래시 히스토리 bucket-key 상관 분석 — 동일 `crash_bucket_key` 반복 발생 시 Evidence에 "반복 크래시 패턴" 표시 + Summary JSON에 `history_correlation` 필드 출력 (A1).
- WinUI: 동일 패턴 반복 시 "⚠ 동일 패턴 N회 반복 발생" 배지 표시 (A1).
- DumpTool: `troubleshooting_guides.json` 신규 데이터 파일 — 크래시 유형별(ACCESS_VIOLATION, D6DDDA, C++ Exception, 프리징, 로딩 중 크래시, 스냅샷) 단계별 트러블슈팅 가이드 6개 (B3).
- DumpTool: Analyzer에서 트러블슈팅 가이드 자동 매칭 → Summary JSON에 `troubleshooting_steps` 출력 (B3).
- WinUI: 접이식 트러블슈팅 체크리스트 UI(`TroubleshootingExpander`) 추가 (B3).

### 테스트
- CrashLogger 파서 엣지케이스 테스트 20개 추가 — 기존 18개 → 38개로 커버리지 대폭 보강 (C3).
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`40/40`).

## v0.2.41-rc1 (2026-02-23)

### 수정
- DumpTool: `MissingMasters` 판정의 암묵 런타임 마스터 예외 목록에 `_ResourcePack.esl`/`ResourcePack.esl`를 추가해 false positive를 완화.
- Diagnostics: 프리징 덤프에서 `_ResourcePack.esl` 단독 누락으로 `MISSING_MASTER`가 과도하게 트리거되던 사용자 피드백 케이스를 재현 기준으로 보정.

### 테스트
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).

## v0.2.40 (2026-02-23)

### 수정
- `v0.2.40-rc3`의 누락 마스터(false positive) 완화와 `v0.2.40-rc4`의 Helper/WinUI 안정화 개선을 정식 반영.
- Helper: crash event 재연결 재시도, hang-only 모드 가시화 로그, 프로세스별 singleton mutex 및 plugin watchdog 기반 재기동 경로를 적용.
- WinUI: 분석 취소 경로(out-of-proc headless 포함)와 대용량 아티팩트 비동기 로딩을 적용해 프리징 체감 개선.
- Helper: retention 정리를 백그라운드 워커로 분리해 캡처 핫패스 블로킹을 완화.

### 테스트
- Linux: `cmake --build build-linux-test -j` 성공.
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).
- Windows: `scripts\\build-win.cmd` 성공.
- Packaging/Release gate: `scripts\\build-winui.cmd` + `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger.zip --no-pdb` + `bash scripts/verify_release_gate.sh /home/kdw73/Tullius_ctd_loger /mnt/c/Users/kdw73/Tullius_ctd_loger` 통과.

## v0.2.40-rc4 (2026-02-23)

### 수정
- Helper: crash event 핸들 열기 실패를 상태로 보존하고, 런타임에서 주기적으로 재연결을 시도하도록 보강. crash event 부재 시 hang-only 모드 경고를 로그에 명확히 표기.
- Helper: 프로세스별 singleton mutex를 도입해 중복 helper 실행을 억제하고, plugin/watchdog와의 생명주기 동기화를 강화.
- Plugin: helper auto-start 경로를 재사용 가능한 함수로 정리하고, helper가 내려갔을 때 지수 백오프로 재기동하는 watchdog을 추가.
- Helper: retention 정리를 캡처 핫패스에서 분리해 백그라운드 워커(큐)로 비동기 처리하도록 변경.
- WinUI: 분석 취소 버튼/취소 토큰 경로를 추가하고, 블랙박스·리포트·WCT 로딩을 백그라운드로 이동해 대용량 아티팩트에서 UI 프리징을 완화.
- WinUI: 분석 실행을 out-of-proc headless 경로로 확장해 취소 시 분석 프로세스를 종료할 수 있도록 개선.

### 테스트
- Linux: `cmake --build build-linux-test -j` 성공.
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).
- Windows: `scripts\\build-win.cmd` 성공.
- Packaging/Release gate: `scripts\\build-winui.cmd` + `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger.zip --no-pdb` + `bash scripts/verify_release_gate.sh /home/kdw73/Tullius_ctd_loger /mnt/c/Users/kdw73/Tullius_ctd_loger` 통과.

## v0.2.40-rc3 (2026-02-23)

### 수정
- DumpTool: `MissingMasters` 계산에서 런타임/매니저 상태에 따라 `plugins.txt`에 명시되지 않을 수 있는 기본 마스터(`Skyrim.esm`, `Update.esm`, DLC 3종, 무료 CC 4종)를 암묵 로드 예외로 처리해 false positive를 완화.
- Diagnostics: 프리징 리포트에서 기본 마스터가 대량 누락으로 표시되며 `MISSING_MASTER`가 과도하게 트리거되던 사례를 재현 기준으로 교정.

### 테스트
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).
- Windows: `scripts\\build-win.cmd` 성공.
- Packaging/Release gate: `scripts\\build-winui.cmd` + `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger.zip --no-pdb` + `bash scripts/verify_release_gate.sh` 통과.

## v0.2.40-rc2 (2026-02-23)

### 수정
- Helper(Hang): foreground/not-foreground 억제 판단과 로그 경로를 공통 헬퍼로 정리해 감지/확정 단계 중복 코드를 제거.
- Helper(Process Exit): 종료 처리 분기를 `Drain/Cleanup/Launch` 보조 함수로 분해해 CTD/정상종료 경계 로직의 가독성과 유지보수성을 개선.
- Retention: 출력 디렉터리를 1회 스캔한 결과를 재사용하고 timestamp refcount로 incident manifest 삭제 조건을 계산해 불필요한 재스캔을 제거.
- Tests: 소스 가드 테스트 공통 유틸(`SourceGuardTestUtils.h`)을 도입하고 구조 기반(assert order/body) 검증으로 문자열 취약 가드를 보강.

### 테스트
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).
- Windows: `scripts\\build-win.cmd` 성공.
- Packaging/Release gate: `scripts\\build-winui.cmd` + `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger.zip --no-pdb` + `bash scripts/verify_release_gate.sh` 통과.

## v0.2.39 (2026-02-22)

### 수정
- Helper/WinUI: CTD/프리징 이후 DumpTool 뷰어 auto-open 경로를 보강(실패/즉시 종료 시에도 headless 분석이 스킵되지 않도록)하고, 뷰어가 열릴 때 기존 분석 산출물을 우선 로드해 중복 분석을 줄임.

### 테스트
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).

## v0.2.39-rc4 (2026-02-22)

### 수정
- Helper: DumpTool 뷰어 auto-open이 실패하거나 즉시 종료되는 경우에도, headless 분석을 스킵하지 않도록 `viewerNow` 판단을 "실제 런치 성공" 기준으로 보강.
- Helper: 프로세스 종료 후 Hang 뷰어 auto-open 로그가 실제 런치 결과를 반영하도록 수정(실패 케이스에서 오해 방지).
- WinUI: Helper가 headless 분석 산출물(Summary/Report 등)을 생성한 직후 뷰어를 auto-open하는 경우, 뷰어가 재분석을 중복 수행하지 않고 기존 산출물을 먼저 로드하도록 개선(필요 시 "지금 분석"으로 재실행 가능).

### 테스트
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).

## v0.2.39-rc3 (2026-02-21)

### 수정
- CI: 릴리즈 파이프라인(Linux Unit Tests)에서 `nlohmann/json.hpp`가 없는 환경에서도 빌드가 되도록, `skydiag_plugin_rules_logic_tests`를 조건부로 활성화하도록 수정.
- Helper: 크래시 이벤트 후 프로세스가 `exit_code=0`으로 종료된 경우에도, 강한 예외 코드가 감지되면 덤프/자동 뷰어 오픈을 억제하지 않도록 보강(CTD인데 뷰어가 안 뜨는 체감 완화).
- Helper: DumpTool 뷰어 실행이 즉시 종료되는 케이스를 감지해 `SkyrimDiagHelper.log`에 런타임/시작 크래시 힌트를 남기도록 진단 로그를 보강.
- Release: `-rc` 태그는 GitHub Release를 pre-release로 생성하도록 워크플로우를 보강.

### 테스트
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).

## v0.2.39-rc2 (2026-02-21)

### 수정
- Helper: 크래시 이벤트 후 프로세스가 `exit_code=0`으로 종료된 경우에도, 강한 예외 코드가 감지되면 덤프/자동 뷰어 오픈을 억제하지 않도록 보강(CTD인데 뷰어가 안 뜨는 체감 완화).
- Helper: DumpTool 뷰어 실행이 즉시 종료되는 케이스를 감지해 `SkyrimDiagHelper.log`에 런타임/시작 크래시 힌트를 남기도록 진단 로그를 보강.
- Release: `-rc` 태그는 GitHub Release를 pre-release로 생성하도록 워크플로우를 보강.

### 테스트
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).

## v0.2.39-rc1 (2026-02-21)

### 개선
- Helper: 시작 시 `SkyrimDiag_Preflight.json`을 생성하는 호환성 프리플라이트 추가. Crash Logger 중복, BEES 필요 조건, 플러그인 스캔 상태를 사전 점검.
- Helper/WCT: COM wait-chain 콜백 등록을 best-effort로 추가해 프리징 분석 맥락을 확장.
- Helper: 덤프 생성 실패 시 `SkyrimDiag_WER_LocalDumps_Hint.txt`를 자동 생성해 WER LocalDumps fallback 가이드를 제공.
- Plugin: 리소스 로깅에 적응형 스로틀 추가(`EnableAdaptiveResourceLogThrottle`)로 대량 loose-file burst 환경에서 오버헤드 완화.
- Plugin/Helper 설정: 신규 옵션(`EnableCompatibilityPreflight`, `EnableWerDumpFallbackHint`, 리소스 스로틀 키) 추가 및 manifest snapshot 반영.
- Release tooling: `scripts/verify_release_gate.sh` 추가로 릴리즈 하드게이트(스크립트 해시/필수 파일/ZIP 엔트리/용량/중첩 경로)를 원샷 검증 가능하게 개선.

### 테스트
- 신규 가드 테스트 추가:
  - `tests/helper_preflight_guard_tests.cpp`
- 기존 가드 테스트 확장:
  - `tests/helper_crash_autopen_config_tests.cpp`
  - `tests/crash_hook_mode_guard_tests.cpp`
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`39/39`).
- Windows: `scripts\\build-win.cmd` 성공, 신규 가드 exe 3종 수동 실행 통과.
- Packaging/Release gate:
  - `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger.zip --no-pdb` 성공
  - `bash scripts/verify_release_gate.sh` 통과

## v0.2.38-rc3 (2026-02-20)

### 수정
- Helper: pending crash 분석 태스크 정리 시 analyzer 프로세스가 살아 있으면 종료 후 핸들을 닫도록 보강하여, 잔존 프로세스로 인한 재진입/충돌 가능성을 완화.
- Plugin: UI 작업 큐(`AddUITask`) enqueue 실패 예외 가드를 추가해 pending 플래그가 고착되는 런타임 데드락 가능성을 완화.
- Plugin: 리소스 훅에서 관심 확장자(.nif/.hkx/.tri) 선필터를 추가해 불필요한 경로 조합/문자열 처리 오버헤드를 줄임.
- DumpTool: 시그니처 DB 로더를 항목 단위 내결함으로 개선(잘못된 hex/regex/구조 항목 스킵)하고, regex 사전 컴파일을 도입해 매칭 경로 안정성/성능을 보강.
- DumpTool: missing masters 계산 시 비활성 플러그인으로 인한 false positive를 제거.
- Packaging: `dump_tool/data` 하위 파일을 재귀 수집하도록 변경해 신규 데이터 파일이 패키지에서 누락되지 않도록 개선.

### 테스트
- 신규 가드 테스트 추가:
  - `tests/pending_crash_analysis_guard_tests.cpp`
  - `tests/plugin_runtime_guard_tests.cpp`
- 런타임/로직/패키징 회귀 테스트 확장:
  - `tests/analysis_engine_runtime_tests.cpp`
  - `tests/plugin_rules_logic_tests.cpp`
  - `tests/packaging_includes_cli_tests.py`
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(38/38).
- Windows 빌드/패키징 + 릴리즈 하드게이트(WinUI 필수 파일, ZIP 필수 엔트리, 용량/중첩 경로 가드) 통과.

## v0.2.37 (2026-02-17)

### 수정
- Packaging: WinUI 폴더 복사 시 중첩 빌드 산출물(`publish/`, `win-x64/`, `x64/`)이 함께 ZIP에 들어가던 문제 수정. `scripts/package.py`에서 중첩 산출물을 제외하도록 보강해 릴리즈 ZIP 용량 급증(파일 중복 포함) 회귀를 해결.

### 테스트
- `tests/packaging_includes_cli_tests.py`에 중첩 WinUI 산출물(`publish`, `win-x64`) 미포함 검증 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(29/29).

## v0.2.36 (2026-02-17)

### 수정
- DumpTool: `usvfs_x64.dll` / `uvsfs64.dll`(MO2 VFS 훅 계층)을 훅 프레임워크 목록으로 분류하도록 보강. 해당 모듈이 크래시 원인으로 과도 지목되던 오탐 가능성을 완화.
- DumpTool: 콜스택/스택 스캔 후보 승격 로직에서 MO2 VFS 훅 모듈(`usvfs_x64.dll`, `uvsfs64.dll`)을 CrashLogger/SKSE 런타임과 동일한 특별 처리 대상으로 추가. 비-훅 후보가 있을 때 피해 프레임 소유자를 1순위 원인으로 과도 지목하지 않도록 조정.

### 테스트
- 훅 프레임워크 JSON 테스트에 `usvfs_x64.dll`, `uvsfs64.dll` 항목 검증 추가.
- 훅 프레임워크 가드 테스트에 MO2 VFS 특별 처리(`topIsMo2Vfs`) 회귀 방지 검증 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(29/29).

## v0.2.35 (2026-02-17)

### 수정
- Packaging/WinUI: `scripts/build-winui.cmd`의 출력 폴더 선택 로직을 보강해, `App.xbf` / `MainWindow.xbf` / `SkyrimDiagDumpToolWinUI.pri`가 포함된 경로만 패키징 대상으로 채택하도록 수정. 일부 환경에서 `x64` 경로가 우선 선택되며 XBF 자산이 빠져 WinUI가 실행 직후 종료되던 회귀를 수정.
- Packaging: `scripts/package.py`에 WinUI 필수 자산 사전 검증을 추가. `App.xbf` / `MainWindow.xbf` / `.pri` 누락 시 ZIP 생성을 실패시켜 깨진 릴리즈 산출물이 배포되지 않도록 가드.

### 테스트
- `tests/packaging_includes_cli_tests.py`를 확장해 WinUI 필수 자산(`App.xbf`, `MainWindow.xbf`, `.pri`)이 ZIP에 포함되는지 검증 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(29/29).

## v0.2.34 (2026-02-16)

### 수정
- Helper: 크래시 이벤트로 덤프를 생성한 뒤에도 대상 프로세스가 `exit_code=0`으로 정상 종료하면, 해당 크래시 산출물(`.dmp`, `*_SkyrimDiagSummary.json`, `*_SkyrimDiagReport.txt`, incident manifest 등)을 종료 직전에 정리하도록 보강. 이제 "게임은 정상 종료했는데 CTD 리포트/뷰어가 뜨는" 오탐 체감을 줄임.
- Helper: 정상 종료(`exit_code=0`) 경로에서 deferred crash viewer 자동 오픈을 차단하여, 종료 경계 예외로 남은 크래시 덤프 팝업이 뜨지 않도록 조정.
- Helper: 정상 종료 오탐 정리 경로에서 Crash ETW stop을 산출물 삭제보다 먼저 수행하도록 순서를 보정. ETW 파일 생성/manifest 갱신 타이밍 경합으로 `.etl` 잔존 가능성을 완화.
- Helper: 크래시 산출물 정리 시 파일별 삭제 실패(잠금/권한 등)를 에러코드와 함께 Helper 로그에 기록하도록 보강.

### 테스트
- 크래시 오탐 가드 테스트에 정상 종료 후 산출물 정리 로직 문자열 가드 추가 (`tests/crash_capture_false_positive_guard_tests.cpp`).
- 크래시 오탐 가드 테스트에 ETW stop 선행 보장 및 삭제 실패 로그 가드를 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(29/29).

## v0.2.33 (2026-02-15)

### 수정
- DumpTool: `sl.interposer.dll`(Streamline/DLSS interposer)을 훅 프레임워크 목록으로 분류하도록 보강. 이제 `sl.interposer.dll`을 단독 원인으로 과도 지목하는 오탐을 줄이고, 비-훅 후보/리소스 충돌 단서를 우선 보도록 유도.
- Helper: 크래시 이벤트 직후 3초 내 프로세스가 종료되고, 크래시 시점 상태가 메뉴(`kState_InMenu`)였던 경우를 종료 경계 케이스로 간주하여 자동 액션을 억제. 덤프는 보존하되 자동 뷰어 팝업/자동 headless 분석을 건너뛰어 "게임 종료했는데 크래시 창이 뜨는" 피드백을 완화.
- Helper: incident manifest의 `state_flags`를 크래시 시점 스냅샷으로 고정해 종료 직후 상태 변동으로 인한 맥락 왜곡을 줄임.

### 테스트
- 훅 프레임워크 JSON/가드 테스트에 `sl.interposer.dll` 회귀 방지 검증 추가.
- 크래시 오탐 가드 테스트에 메뉴 경계 억제 플래그(`suppressCrashAutomationForLikelyShutdownException`) 검증 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(29/29).

## v0.2.32 (2026-02-15)

### 수정
- Hang 분석 오탐 완화: `win32u.dll`을 시스템 DLL 목록에 추가하고, Windows 시스템 경로(`...\Windows\System32\...` 등) 기반 분류를 도입해 스택 후보에서 시스템 DLL이 유력 후보로 과도하게 노출되는 케이스를 줄임.
- 요약 문구 보수화: hang 캡처에서 스택 1순위가 Windows 시스템 DLL일 경우, `유력 원인`으로 단정하지 않고 "대기/피해 위치 가능성, 덤프 단독으로 원인 단정 어려움"으로 안내하도록 조정.
- 권장 조치 보수화: 스택 1순위가 시스템 DLL이면 모드 재설치/비활성화 단정 안내 대신 비-시스템 후보/리소스/충돌 단서 우선 점검을 유도.
- `InferredMod` 안전장치: fault module이 시스템/게임 모듈이거나 추정명이 DLL/EXE 이름 형태일 때는 `inferred_mod_name`을 비워 잘못된 `InferredMod: win32u.dll` 출력 가능성을 차단.
- CrashLogger 파서/후처리의 시스템 DLL 필터에도 `win32u.dll`을 반영해 결과 일관성을 개선.

### 테스트
- 시스템 DLL 오탐 회귀 방지 가드 테스트 추가(`tests/system_module_guard_tests.cpp`).
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과(29/29).

## v0.2.31 (2026-02-15)

### 수정
- DumpTool: `skse64_loader.dll`/`skse64_steam_loader.dll`뿐 아니라 `skse64_1_6_1170.dll` 형태의 SKSE 런타임 DLL(`skse64_*.dll`)도 훅 프레임워크로 판별하도록 보완. 기존에는 런타임 DLL이 일반 원인 후보로 승격되는 오탐이 남아있을 수 있었음.
- DumpTool: 콜스택/스택스캔의 훅 프레임워크 우선순위 완화 로직에서 SKSE 로더 별칭이 아니라 SKSE 런타임 패턴 공통 판별(`IsSkseModule`)을 사용하도록 변경.
- 데이터: `hook_frameworks.json` 기본 목록에 `skse64.dll` 항목 추가.

### 테스트
- 훅 프레임워크 가드 테스트를 SKSE 런타임 공통 판별(`topIsSkseRuntime`, `IsSkseModule`) 기준으로 갱신.
- `hook_frameworks.json` 테스트에 `skse64.dll` 항목 검증 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과.

## v0.2.30 (2026-02-15)

### 수정
- DumpTool: 스택 후보 승격 로직에서 `skse64_loader.dll` / `skse64_steam_loader.dll`을 CrashLogger 계열과 동일한 훅 프레임워크 특수 케이스로 처리하도록 보완. 비-훅 후보가 있을 때 로더 DLL이 과도하게 1순위로 지목되는 오탐을 완화.
- 요약 문구: 훅 프레임워크 모듈(예: SKSE 로더)만 남는 경우 `유력 원인`으로 단정하지 않고 "피해 위치 가능성 / 단독 원인 단정 어려움"으로 보수화.
- 권장 조치: 훅 프레임워크가 fault module인 상황에서 비-훅 후보가 없으면, 해당 DLL 자체를 단독 원인으로 안내하지 않고 리소스/충돌/비-훅 단서 우선 점검을 유도하도록 조정.

### 테스트
- 훅 프레임워크 가드 테스트에 SKSE 로더 별칭 처리(`topIsSkseLoader`) 검증을 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과.

## v0.2.29 (2026-02-15)

### 수정
- DumpTool: `CrashLogger.dll`(구/별칭 파일명)도 `crashloggersse.dll`과 동일하게 훅 프레임워크 목록으로 분류하도록 보완. 기존에는 별칭이 목록에 없어 스택 후보 1순위로 과도 지목되는 오탐 케이스가 발생할 수 있었음.
- DumpTool: 스택 기반 후보 승격 로직에서 CrashLogger 특수 처리에 `CrashLogger.dll` 별칭을 추가하여, 비-훅 후보가 있을 때 피해 프레임 소유자를 원인으로 과도 지목하지 않도록 개선.
- 요약/권장 문구: `SkyrimSE.exe` 또는 시스템 모듈 크래시에서 스택 1순위가 훅 프레임워크(`CrashLogger.dll` 포함)인 경우, 단독 원인으로 단정하지 않고 "피해 위치 가능성"을 명시하도록 보수화.
- 권장 조치: 비-훅 스택 후보가 존재하면 해당 후보를 우선 안내하고, 훅 프레임워크 후보만 남을 때는 리소스/충돌/비-훅 단서 우선 점검 가이드를 제공.

### 테스트
- `hook_frameworks.json`에 `crashlogger.dll` 항목 존재를 검증하는 테스트 추가.
- 훅 프레임워크 가드 테스트에 `CrashLogger.dll` 별칭 처리/요약 보수화 가드 케이스 추가.
- 전체 Linux 테스트 재실행: `ctest --test-dir build-linux-test --output-on-failure` 통과.

## v0.2.28 (2026-02-15)

### 수정
- Retention: `Crash`/`Crash_Full` 덤프가 같은 timestamp를 공유할 때, 한쪽 덤프만 정리되어도 incident manifest(`SkyrimDiag_Incident_Crash_<ts>.json`)가 같이 삭제되던 문제 수정. 동일 timestamp의 다른 덤프가 남아 있으면 manifest를 유지.
- Helper(Hang): ETW 파일 저장 전에 retention이 먼저 실행되어 ETW 개수 제한이 즉시 반영되지 않던 순서 문제 수정. ETW stop/write 이후 retention을 적용하도록 조정.
- Helper(Crash): 비동기 Crash ETW stop 완료 시점과 자동 Full 재캡처 생성 시점에 retention을 즉시 재적용하도록 보완하여, 세션 중에도 덤프/ETW 보관 개수 제한을 더 일관되게 유지.

### 테스트
- `skydiag_retention_tests` 보강:
  - 동일 timestamp sibling crash dump(`Crash` + `Crash_Full`) 시 manifest 유지 회귀 테스트 추가.
  - Crash/Hang ETW trace를 통합 대상으로 개수 제한 prune 동작을 검증하는 테스트 추가.

## v0.2.27 (2026-02-15)

### 수정
- 릴리즈 CI(Linux Unit Tests)에서 `nlohmann/json.hpp`가 없는 환경에서 `skydiag_analysis_engine_runtime_tests` 빌드가 실패하던 문제 수정. 이제 헤더가 있을 때만 해당 런타임 테스트 타깃을 활성화하여 태그 릴리즈 파이프라인이 안정적으로 동작.

### 포함
- 분석 신뢰성 개선(시그니처 DB/주소 해석/크래시 이력/스코어링 교정/오탐 완화) 변경을 그대로 포함.

## v0.2.26 (2026-02-15)

### 추가
- DumpTool: 크래시 시그니처 데이터베이스 도입 (`dump_tool/data/crash_signatures.json`) 및 분석 파이프라인 통합. 예외 코드/모듈/오프셋/콜스택 패턴을 기반으로 알려진 크래시 패턴을 우선 진단.
- DumpTool: 게임 EXE 오프셋 해석기(Address Resolver) 도입 (`dump_tool/data/address_db/skyrimse_functions.json`). 알려진 함수와 매칭되면 증거/요약 JSON에 함수명을 출력.
- DumpTool: 크래시 이력 저장/통계 엔진 도입 (`crash_history.json`). 최근 반복 발생 모듈 통계를 증거와 요약 JSON에 포함.
- 테스트: 핵심 분석 엔진 런타임 테스트 추가 (`tests/analysis_engine_runtime_tests.cpp`) 및 스코어링/시그니처/주소해석/이력 관련 가드 테스트 확장.

### 개선
- 훅 프레임워크 목록을 JSON으로 외부화하고 분석기/패키징 경로를 통합하여 하드코딩 중복 제거.
- 스택 스캔 점수 계산에 RSP 근접 가중치(8/4/2/1)를 적용하고 임계값을 재보정하여 오탐을 완화.
- 결과 JSON(`*_SkyrimDiagSummary.json`)에 `signature_match`, `resolved_functions`, `crash_history_stats`, triage 확장 필드를 추가.

### 수정
- Fallback 모듈 탐지 경로에서 `fault_module_offset`가 누락되던 문제 수정(시그니처 매칭/주소 해석 정확도 개선).
- 시그니처 `callstack_contains` 매칭 입력을 실제 콜스택 프레임 기반으로 보강(기존 suspect 모듈명 중심 입력의 한계 보완).

## v0.2.25 (2026-02-14)

### 수정
- **Helper: 빠르게 종료되는 크래시에서 덤프가 0바이트로 생성되던 문제 수정.** 기존에는 크래시 이벤트 수신 후 최대 4.5초간 필터링(정상 종료/핸들된 예외 확인)을 먼저 수행한 뒤 덤프를 시도했으나, 그 사이 프로세스가 종료되면 `MiniDumpWriteDump`가 실패하여 빈 파일만 남았음. 이제 **덤프를 즉시 먼저 쓰고**, 사후에 false positive를 필터링(정상 종료 시 덤프 삭제)하는 "dump-first" 전략으로 변경.
- Helper: 덤프 실패 시 0바이트 파일을 자동 삭제하고, 실패 원인을 Helper 로그 파일에 기록하도록 개선 (기존에는 stderr에만 출력).
- DumpTool: CrashLoggerSSE/기타 훅 프레임워크가 유력 후보 1순위로 과도 노출되던 케이스 완화. 스택 후보 정렬에서 훅 프레임워크(특히 `CrashLoggerSSE.dll`)를 보수적으로 비우선화하고, 훅 프레임워크 1순위일 때 CrashLogger 근거 기반 confidence 부스트를 억제하여 오탐 안내를 줄임.
- Helper: crash event가 수동 리셋(manual-reset)인데 소비(reset)하지 않아 동일 신호를 반복 처리하던 루프를 수정. 이벤트 핸들을 `EVENT_MODIFY_STATE|SYNCHRONIZE`로 열고 처리 직후 `ResetEvent`로 소비하여 중복 처리/지연 루프를 방지.
- Helper: handled first-chance 예외 필터를 보수화. heartbeat 1회 전진만으로 덤프 삭제하지 않고, 다중 체크에서 2회 이상 전진이 확인될 때만 삭제하여 실제 크래시 누락 위험을 낮춤.

## v0.2.23 (2026-02-14)

### 수정
- Helper: `SkyrimDiagHelper.log`가 게임 세션 간에 계속 누적되던 문제 수정. 새 게임 세션(프로세스 어태치) 시 로그 파일을 초기화하여 매번 깨끗한 로그로 시작.

### 내부 개선
- Helper: 미사용 파라미터 `attachHeartbeatQpc` 제거 (내부 API 정리).
- Helper: 하트비트 초기화 경고 지연시간을 명명된 상수 `kHeartbeatInitWarnDelaySec`로 추출.

## v0.2.22 (2026-02-14)

### 수정
- Helper: 하트비트가 어태치 이후 전진하지 않으면 자동 행(hang) 캡처가 영구 비활성화되던 문제 수정. 기존의 `heartbeatEverAdvanced` 가드를 제거하고, 플러그인 하트비트 초기화 여부(`last_heartbeat_qpc != 0`)만 확인하도록 변경. 프리즈 시 하트비트가 멈추는 것이 정상 신호이므로, 데드락/무한루프/무한로딩 시나리오에서 자동 캡처가 올바르게 작동.
- Helper: 게임이 프리즈된 상태에서 Alt-Tab하면 포그라운드 억제(`SuppressHangWhenNotForeground`)로 행 덤프가 생성되지 않던 캐치-22 수정. 포그라운드가 아닐 때 윈도우 응답성(`IsWindowResponsive`)을 함께 확인하여, 윈도우가 무응답이면(진짜 프리즈) 억제하지 않고 캡처 진행.
- WinUI: 내부 리스트(증거/콜스택/이벤트 등)와 외부 페이지 스크롤이 동시에 굴러가던 문제 수정. 내부 리스트가 스크롤 경계(상단/하단)에 도달했을 때만 외부 스크롤로 전환.

## v0.2.21 (2026-02-14)

### 수정
- Helper: 핸들링된 첫 번째 기회 예외(first-chance exception)로 인한 오탐 덤프 생성 방지. 크래시 이벤트 수신 후 프로세스가 살아있을 때 하트비트 갱신 여부를 확인하여, 게임이 정상 동작 중이면 덤프를 건너뛰도록 개선.

## v0.2.20 (2026-02-13)

### 추가
- DumpTool: 알려진 훅 프레임워크 모드(EngineFixes, SSE Display Tweaks, po3_Tweaks, HDT-SMP, CrashLoggerSSE 등)가 fault module일 때 confidence를 한 단계 낮추고, "다른 모드의 메모리 오염 피해자일 수 있음" 경고를 Summary와 Recommendations에 표시. 훅 모드가 단순히 크래시 발생 위치일 뿐 진짜 원인이 아닐 수 있음을 사용자에게 안내.

## v0.2.19 (2026-02-13)

### 수정
- Helper: 정상 종료 시 크래시 덤프 생성 억제 강화. 종료 대기 시간을 500ms→3000ms로 증가하여, 모드가 많은 환경에서 DLL 정리 시간이 길어도 정상 종료로 올바르게 판단.
- Helper: 크래시 후 프로세스가 늦게 종료되는 경우 뷰어가 열리지 않던 문제 수정. 프로세스 종료 시점까지 뷰어 실행을 지연(deferred)하여, C++ 예외 등으로 프로세스가 지연 종료되어도 뷰어가 자동으로 열리도록 개선.

## v0.2.18 (2026-02-13)

### 수정
- Helper: 정상 종료 시 크래시 덤프가 생성되던 문제 수정. 종료 과정에서 DLL 정리 중 발생하는 예외를 VEH가 감지하여 덤프를 만들던 현상을, 프로세스 종료 코드(exit_code=0)를 확인해 정상 종료로 판단하면 덤프를 건너뛰도록 개선.

## v0.2.17 (2026-02-13)

### Fixed
- Build: correct MSVC runtime library generator expression in CMake.
- Build: add `/utf-8` compiler flag for MSVC to satisfy fmt v11 requirement.
- Build: handle x64 platform subfolder in WinUI output path.
- Build: explicit exit code 0 after robocopy in `build-winui.cmd`.
- CI: build all test targets instead of hardcoded list.
- CI: add tag-triggered release workflow.
- Tests: remove assertions for unimplemented features.

## v0.2.16 (2026-02-13)

### Fixed
- Helper: fix race condition where crash event was missed if the game process terminated before the next poll cycle. On process exit, the helper now drains any pending crash event (non-blocking) before shutting down.

## v0.2.15 (2026-02-10)

### Fixed
- WinUI DumpTool: surface native analysis exceptions with actionable messages instead of a generic "External component has thrown an exception."
  - When a managed exception occurs during native interop, a `*_SkyrimDiagNativeException.log` is written to the output folder (best-effort).
- DumpTool: fix a rare analysis failure when merging existing summary triage (`[json.exception.invalid_iterator.214] cannot get value`).
- DumpTool: manual snapshot captures (`SkyrimDiag_Manual_*.dmp`) are now more reliably classified as snapshots (not CTDs) unless an exception stream is present.
- DumpTool: do not generate a misleading crash bucket key for snapshot dumps that have no exception/module/callstack information.

## v0.2.14 (2026-02-10)

### Changed
- CrashLogger integration: if CrashLogger.ini sets `Crashlog Directory`, SkyrimDiag will also search that folder when auto-detecting CrashLogger logs (best-effort).

### Added
- Internal regression tests: parse CrashLogger.ini `Crashlog Directory` (quotes/spacing/comments).

## v0.2.13 (2026-02-10)

### Changed
- Internal refactor only: split DumpTool evidence builder internals into smaller modules (no behavior changes).

### Added
- Internal regression tests: harden CrashLogger parser fixtures for v1.20 format variations (callstack rows + version header variants).

## v0.2.12 (2026-02-10)

### Changed
- Internal refactor only: split DumpTool analyzer internals into smaller modules (no behavior changes).
- Internal refactor only: split Helper main into smaller modules (no behavior changes).

## v0.2.11 (2026-02-10)

### Changed
- Avoid duplicate analysis: when Helper auto-opens the WinUI viewer for a dump, it now skips headless auto-analysis for that same dump.

### Added
- New regression test: `tests/headless_analysis_policy_tests.cpp`

## v0.2.10 (2026-02-10)

### Added
- Headless analyzer CLI: `SkyrimDiagDumpToolCli.exe` (no WinUI dependency) for post-incident analysis.
- Helper now prefers the headless CLI for auto-analysis when available, and falls back to the WinUI exe for backward compatibility.
- Packaging now ships `SkyrimDiagDumpToolCli.exe` next to `SkyrimDiagHelper.exe`.
- New tests:
  - `tests/dump_tool_cli_args_tests.cpp`
  - `tests/dump_tool_headless_resolver_tests.cpp`
  - `tests/packaging_includes_cli_tests.py`

## v0.2.9 (2026-02-10)

### Added
- Incident manifest sidecar JSON per capture (enabled by default):
  - `SkyrimDiag_Incident_Crash_*.json`
  - `SkyrimDiag_Incident_Hang_*.json`
  - `SkyrimDiag_Incident_Manual_*.json`
  - Includes `incident_id`, `capture_kind`, artifact filenames, ETW status, and an optional privacy-safe config snapshot.
- Optional crash-window ETW capture in `SkyrimDiagHelper.ini` (advanced, OFF by default):
  - `EnableEtwCaptureOnCrash`
  - `EtwCrashProfile`
  - `EtwCrashCaptureSeconds` (1..30)
- DumpTool now surfaces incident context in summary/report when a manifest is present (`summary.incident.*`).

### Changed
- Retention cleanup now prunes incident manifests alongside their corresponding dumps, and will remove `SkyrimDiag_Crash_*.etl` traces when pruning crash dumps.

## v0.2.8 (2026-02-10)

### Added
- Crash hook safety guard option in `dist/SkyrimDiag.ini`:
  - `EnableUnsafeCrashHookMode2=1` is now required to use `CrashHookMode=2`.
- Online symbol source control in `dist/SkyrimDiagHelper.ini`:
  - `AllowOnlineSymbols=0|1` with default `0` (offline/local cache).
- DumpTool privacy telemetry fields in summary/report outputs:
  - `path_redaction_applied`
  - `online_symbol_source_allowed`
  - `online_symbol_source_used`
- New regression tests:
  - `tests/crash_hook_mode_guard_tests.cpp`
  - `tests/symbol_privacy_controls_tests.cpp`
- Added vibe-kit guard workflow and doctor script scaffolding:
  - `.github/workflows/vibekit-guard.yml`
  - `.vibe/brain/agents_doctor.py`

### Changed
- DumpTool symbolization now defaults to offline/local cache unless explicitly opted in.
- Helper now passes explicit symbol policy flags (`--allow-online-symbols` / `--no-online-symbols`) to WinUI analyzer path.
- Path redaction is applied more consistently in outputs, including resource path lines.
- Test runner wiring now uses `Python3_EXECUTABLE` and `sys.executable` for cross-platform Python invocation.
- Vibe-kit seed/config scripts and docs were refreshed:
  - `.vibe/config.json`
  - `.vibe/README.md`
  - `.vibe/brain/*`
  - `scripts/setup_vibe_env.py`
  - `scripts/vibe.py`

### Fixed
- Windows `ctest` compatibility issue caused by hardcoded `python3` in bucket quality script tests.

## v0.2.6 (2026-02-07)

### Added
- Helper retention/disk cleanup options in `SkyrimDiagHelper.ini`:
  - `MaxCrashDumps`, `MaxHangDumps`, `MaxManualDumps`, `MaxEtwTraces`
  - `MaxHelperLogBytes`, `MaxHelperLogFiles`
- Crash viewer popup suppression options in `SkyrimDiagHelper.ini`:
  - `AutoOpenCrashOnlyIfProcessExited`, `AutoOpenCrashWaitForExitMs`
- DumpTool evidence: exception parameter analysis for common codes (e.g., access violation read/write/execute + address).
- CrashLogger integration: detect and report CrashLogger version string (e.g., `CrashLoggerSSE v1.19.0`) when a log is auto-detected.
- WinUI: added a "Copy summary" action for quick sharing.

### Fixed
- CI Linux workflow now builds all unit test targets before running `ctest`.
- CI Windows manual workflow builds the WinUI shell before packaging.

## v0.2.5 (2026-02-06)

### Fixed
- Packaging bug in `scripts/package.py`: WinUI publish output is now copied recursively, preventing runtime file loss when publish layouts include nested files/directories.
- WinUI packaging crash fix: `scripts/build-winui.cmd` now stages from WinUI build output (includes required `.pri/.xbf` assets) instead of stripped publish output.
- WinUI visual quality improvements: enabled Per-Monitor V2 DPI awareness via app manifest for sharper rendering on high-DPI displays.
- WinUI scrolling reliability: when nested controls consume mouse wheel input, wheel events are chained to the root scroll viewer for smoother page scrolling.
- WinUI localization polish: static UI labels/buttons now switch between English/Korean (`--lang ko` or system UI language Korean).

### Added
- Native analyzer bridge DLL for WinUI (`SkyrimDiagDumpToolNative.dll`) with exported C ABI (`SkyrimDiagAnalyzeDumpW`) so WinUI can analyze dumps directly without launching legacy UI executable.
- Built-in advanced analysis panels in WinUI (callstack, evidence, resources, blackbox events, WCT JSON, report text) in the same window as beginner summary.

### Changed
- WinUI headless mode now runs native analysis directly (no process delegation to `SkyrimDiagDumpTool.exe`).
- Helper dump-tool resolution no longer falls back to legacy executable.
- CMake build no longer defines the legacy `SkyrimDiagDumpTool` Win32 executable target (native DLL + WinUI only).
- WinUI publish switched to framework-dependent/lightweight output (`scripts/build-winui.cmd`), reducing package size but requiring user runtimes.
- WinUI viewer visuals refreshed (typography, spacing, card styling, and list readability) while preserving existing dump-analysis workflow.
- WinUI viewer theme refreshed with a Skyrim-inspired parchment + dark stone look.
- WinUI viewer redesigned again using current Fluent/observability UI patterns:
  - fixed left navigation pane visibility (always expanded labels, no icon-only collapse)
  - added quick triage strip (primary suspect/confidence/actions/events)
  - added explicit 3-step workflow cards in Analyze panel
  - increased visual depth with layered surface tokens (`Window/Pane/Hero/Section/Elevated`)
- Packaging now ships full-replacement WinUI set:
  - includes `SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
  - includes `SkyrimDiagWinUI/SkyrimDiagDumpToolNative.dll`
  - no longer requires or packages `SkyrimDiagDumpTool.exe` / `SkyrimDiagDumpTool.ini`

## v0.2.4 (2026-02-06)

### Added
- New modern WinUI 3 viewer shell (`SkyrimDiagDumpToolWinUI.exe`) with beginner-first layout:
  - dump picker + one-click analysis
  - crash snapshot card (summary/bucket/module/mod hint)
  - top cause candidates list
  - recommended next-step checklist
  - quick action to open legacy advanced viewer
- Windows helper script `scripts/build-winui.cmd` to publish WinUI viewer in self-contained mode.
- Packaging enhancement: `scripts/package.py` now auto-includes WinUI publish artifacts when found (configurable with `--winui-dir` and `--no-winui`).

### Changed
- Helper default dump viewer executable changed to `SkyrimDiagWinUI\SkyrimDiagDumpToolWinUI.exe`.
- Helper executable resolution now safely falls back to legacy `SkyrimDiagDumpTool.exe` if WinUI executable is missing.
- README and default ini guidance updated for WinUI-first workflow.

## v0.2.3 (2026-02-06)

### Added
- Crash bucketing key (`crash_bucket_key`) output in Summary JSON/Report, plus callstack symbolization improvements to better group repeated CTDs by signature.
- Beginner-first DumpTool UX:
  - Default beginner view with primary CTA (`Check Cause Candidates` / `원인 후보 확인하기`)
  - Top-5 candidate + evidence presentation
  - Explicit `Advanced analysis` toggle to access full tabs.
- DumpTool single-window reuse path: when already open, new dump opens in the same window via inter-process message handoff (`WM_COPYDATA`) instead of creating extra windows.
- New helper viewer auto-open policy options in `SkyrimDiagHelper.ini`:
  - `AutoOpenViewerOnCrash`
  - `AutoOpenViewerOnHang`
  - `AutoOpenViewerOnManualCapture`
  - `AutoOpenHangAfterProcessExit`
  - `AutoOpenHangDelayMs`
  - `AutoOpenViewerBeginnerMode`
- Optional ETW capture around hang dumps (`EnableEtwCaptureOnHang`, `EtwWprExe`, `EtwProfile`, `EtwMaxDurationSec`) as best-effort diagnostics.
- New bucket unit test target (`skydiag_bucket_tests`) and test source (`tests/bucket_tests.cpp`).

### Changed
- Helper dump flow now separates headless analysis from viewer launch:
  - Crash: viewer can open immediately
  - Hang: latest hang dump can be queued and auto-opened after process exit (with configurable delay)
  - Manual capture: viewer auto-open remains off by default.
- DumpTool now persists beginner/advanced default mode in `SkyrimDiagDumpTool.ini` (`BeginnerMode=1|0`) and supports CLI overrides (`--simple-ui`, `--advanced-ui`).

## v0.2.2 (2026-02-03)

### Fixed
- Further reduced Alt-Tab false hang dumps: after returning to foreground, keep suppressing hang dumps while the game window is responsive (and not in a loading screen), until the heartbeat advances.

### Added
- CrashLogger SSE/AE v1.18.0 support: parse and surface the new `C++ EXCEPTION:` details (Type / Info / Throw Location / Module) in evidence, reports, and JSON output.
- DumpTool i18n (EN/KO): English-first UI/output for Nexus + in-app language toggle (persists via `SkyrimDiagDumpTool.ini` and supports CLI `--lang en|ko`).
- DumpTool UI polish: modern owner-draw buttons, better padding (Summary/WCT), WCT mono font, evidence row striping, and Windows 11 rounded corners (best-effort).

## v0.2.1 (2026-02-01)

### Fixed
- Further reduced false hang dumps around Alt-Tab / background pause by keeping suppression “sticky” until the heartbeat advances, and adding a short foreground grace window (`ForegroundGraceSec`).
- Improved CrashLogger SSE/AE log compatibility (v1.17.0+): better detection and parsing for thread dump logs (`threaddump-*.log`) and stack-trace edge cases.

### Added
- Lightweight cross-platform unit tests for hang suppression logic and CrashLogger log parsing core (Linux-friendly, no Win32 deps).

## v0.2.0 (2026-02-01)

### Fixed
- Prevented false hang dumps when the user Alt-Tabs: by default, hang capture is suppressed while Skyrim is not the foreground window (`SuppressHangWhenNotForeground=1`).
- Reduced false positives around menus/shutdown by using a more conservative menu threshold (`HangThresholdInMenuSec`) and a short re-check grace period before writing hang dumps.

### Changed
- DumpTool internal architecture: split into `SkyrimDiagDumpToolCore` (analysis/output) + `SkyrimDiagDumpTool` (UI) to reduce coupling and make future maintenance safer.
- Improved documentation for beta testing and common misinterpretations (manual snapshot vs. real CTD/hang).

## v0.1.0-beta.1 (2026-01-30)

- Initial public beta release.
