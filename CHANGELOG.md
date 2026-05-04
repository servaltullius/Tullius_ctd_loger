# Changelog

> **버전 갭 안내:** v0.2.7, v0.2.24, v0.2.38은 RC(Release Candidate)만 배포 후 정식 릴리즈 없이 다음 버전으로 넘어간 번호입니다.

## v0.2.51 (2026-05-04)

### 한눈에 보기
- 이번 릴리즈는 **CrashLoggerSSE v1.21/v1.22 객체 introspection 호환성 보강**입니다.
- Crash Logger가 새로 출력하는 `SpellItem`, `EffectSetting`, `BGSLocation`, `NavMesh` 요약 라인에서도 원인 후보 ESP/FormID와 객체 타입/이름을 유지하도록 파서를 보강했습니다.
- 기존 `RDI: (Character*) ...` 형식은 그대로 유지하고, `RDX: RE::SpellItem "..." [0x...] (Mod.esp)` 같은 새 요약 포맷만 fallback으로 처리합니다.

### 수정
- **CrashLogger parser: simplified introspection 지원** — `POSSIBLE RELEVANT OBJECTS` 안의 `RE::SpellItem`, `RE::EffectSetting`, `RE::BGSLocation`, `RE::NavMesh` 라인에서 타입, 표시 이름, FormID, plugin 파일명을 함께 추출합니다.
- **Regression tests: v1.21/v1.22 객체 라인 가드 추가** — 최신 CrashLoggerSSE의 spell/effect/location/navmesh 예시 라인을 파서 테스트에 추가해 이후 포맷 회귀를 잡도록 했습니다.

### 테스트
- Linux 전체 테스트 `57/57` 통과.
- Windows native build: 성공.
- Windows WinUI build: 성공.
- Packaging(`dist/Tullius_ctd_loger_v0.2.51.zip`, `--no-pdb`): 성공.
- Release gate: `OK`.

## v0.2.50 (2026-04-28)

### 한눈에 보기
- 이번 릴리즈는 **MO2 활성 프로필 기준의 provider/slot 진단 정확도 보정**입니다.
- 활성 profile의 `modlist.txt`를 읽은 경우, 비활성화된 모드가 loose-file provider 후보처럼 보이지 않도록 했습니다.
- ESL / full plugin 슬롯 경고는 활성 플러그인만 세고, full plugin 판정이 불확실한 항목은 고신뢰 슬롯 한계 경고에서 제외하도록 조정했습니다.
- 프로젝트를 `G:\skyrim project\Tullius_ctd_loger`로 옮긴 뒤에도 릴리즈 게이트와 외부 build tree 테스트가 현재 repo root를 올바르게 잡도록 보강했습니다.

### 수정
- **MO2 provider hint: 비활성 모드 제외** — 활성 profile modlist를 정상적으로 읽은 경우 provider 검색을 활성 모드 범위로 제한해, 꺼져 있는 모드가 리소스 제공 후보처럼 표시되는 오탐을 줄였습니다.
- **Plugin rules: 활성 슬롯 기준 보정** — `esl_count_gte`는 활성 ESL만 세도록 바꾸고, 새 `full_plugin_count_gte` 조건은 활성 full plugin 중 슬롯 타입을 알고 있는 항목만 세도록 추가했습니다.
- **Release gate: 이동 후 경로 안정화** — `verify_release_gate.sh`가 예전 WSL checkout / Windows mirror 경로 대신 스크립트 위치에서 기본 repo root를 계산하도록 변경했습니다.
- **Tests: 외부 build tree 안정화** — `skydiag_candidate_consensus_tests`가 repo 밖 build tree에서도 source root를 찾도록 CTest 환경을 보강했습니다.

### 테스트
- Linux 전체 테스트 `57/57` 통과.
- Windows native build: 성공.
- Windows WinUI build: 성공.
- Packaging(`dist/Tullius_ctd_loger_v0.2.50.zip`, `--no-pdb`): 성공.
- Release gate: `OK`.

## v0.2.49 (2026-04-02)

### 한눈에 보기
- 이번 정식 릴리즈는 **non-system DLL CTD에서 단일 DLL 과단정을 더 줄이는 정확도 보정**입니다.
- 이제 non-system DLL CTD도 `actionable_candidates` 합의 경로에 들어가고, raw fault DLL 하나만으로 후보가 고정되지 않도록 조정했습니다.
- `Crash Logger frame + 같은 덤프 stack`만 있는 경우는 더 이상 독립 교차검증처럼 취급하지 않고, `fault-location cluster`로 낮춰 보여줍니다.

### 수정
- **Engine: non-system DLL candidate consensus 확장** — EXE/system/hook/hang 케이스에만 국한되던 `actionable_candidates` 합의 경로를 non-system DLL CTD에도 열어, `Crash Logger frame`, `stack`, `history`, `resource` 같은 신호를 함께 비교하도록 확장.
- **Engine: weak fault-location cluster 강등** — `Crash Logger frame + 같은 덤프 stack` 정도만 있는 후보는 `cross_validated`처럼 승격하지 않고, low/cautious path로 내려 summary/evidence/recommendation이 피해 위치 가능성을 더 정직하게 노출하도록 조정.
- **Output: phrasing 정렬** — non-system DLL CTD에서 `유력 후보`처럼 읽히던 wording을 `fault-location 단서`, `주변 probable DLL 비교` 쪽으로 옮겨, raw crash site와 최종 해석을 더 분리해서 읽을 수 있게 함.

### 테스트
- Linux 전체 테스트 `57/57` 통과.
- Windows native build: 성공.
- Windows WinUI build: 성공.
- Packaging(`dist/Tullius_ctd_loger_v0.2.49.zip`, `--no-pdb`): 성공.
- Release gate: `OK`.

## v0.2.49-rc1 (2026-03-31)

### 한눈에 보기
- 이번 프리릴리즈는 **non-system DLL CTD에서 단일 DLL 과단정을 더 줄이는 정확도 보정**입니다.
- 이제 non-system DLL CTD도 `actionable_candidates` 합의 경로에 들어가고, raw fault DLL 하나만으로 후보가 고정되지 않도록 조정했습니다.
- `Crash Logger frame + 같은 덤프 stack`만 있는 경우는 더 이상 독립 교차검증처럼 취급하지 않고, `fault-location cluster`로 낮춰 보여줍니다.

### 수정
- **Engine: non-system DLL candidate consensus 확장** — EXE/system/hook/hang 케이스에만 국한되던 `actionable_candidates` 합의 경로를 non-system DLL CTD에도 열어, `Crash Logger frame`, `stack`, `history`, `resource` 같은 신호를 함께 비교하도록 확장.
- **Engine: weak fault-location cluster 강등** — `Crash Logger frame + 같은 덤프 stack` 정도만 있는 후보는 `cross_validated`처럼 승격하지 않고, low/cautious path로 내려 summary/evidence/recommendation이 피해 위치 가능성을 더 정직하게 노출하도록 조정.
- **Output: phrasing 정렬** — non-system DLL CTD에서 `유력 후보`처럼 읽히던 wording을 `fault-location 단서`, `주변 probable DLL 비교` 쪽으로 옮겨, raw crash site와 최종 해석을 더 분리해서 읽을 수 있게 함.

### 테스트
- Linux 전체 테스트 `57/57` 통과.
- Windows native build: 성공.
- Windows WinUI build: 성공.
- Packaging(`dist/Tullius_ctd_loger_v0.2.49-rc1.zip`, `--no-pdb`): 성공.
- Release gate: `OK`.

## v0.2.48 (2026-03-31)

### 한눈에 보기
- 이번 버전은 **Crash Logger 단서를 Tullius 결론과 더 정확히 맞추는 후속 패치**입니다.
- Crash Logger의 `CALL STACK ([P]robable / [S]tack scan)` 형식을 제대로 읽어, `[P]` 체인을 실제 후보 근거로 반영합니다.
- non-system DLL에서 direct fault가 잡혀도, 독립 근거가 부족하면 바로 `유력 후보 / 높음`으로 단정하지 않도록 완화했습니다.
- mod author에게 바로 보고하라는 안내도 `cross_validated`처럼 교차검증된 경우에만 유지합니다.

### 수정
- **Crash Logger parser: mixed call stack 형식 지원** — `PROBABLE CALL STACK:`뿐 아니라 `CALL STACK ([P]robable / [S]tack scan):` 헤더도 인식하고, 혼합 형식에서는 `[P]` 행만 probable call stack으로 수집하도록 수정.
- **Summary: non-system DLL 과단정 완화** — direct fault DLL이 있어도 `frame only`, `reference clue`, `related`, `conflicting`, `cross_validated` 상태를 구분해 요약 문장을 다르게 쓰고, 피해 위치(victim location) 가능성을 더 정직하게 노출.
- **Recommendations: DLL guidance 조정** — 독립 신호 합의가 없는 경우에는 DLL을 바로 근본 원인으로 단정하지 말라는 안내를 우선하고, mod author 보고 권고는 fault-module candidate가 교차검증된 경우에만 노출.

### 테스트
- Crash Logger parser 테스트에 mixed `[P]/[S]` call stack fixture를 추가.
- output snapshot / analysis engine runtime 테스트에 `no second independent signal`, `victim location`, `fault-location evidence only` 같은 비과장 계약을 추가.
- Linux 전체 테스트 `57/57` 통과.
- Windows native build / WinUI build / Packaging(`--no-pdb`) / release gate 확인.

## v0.2.47 (2026-03-31)

### 한눈에 보기
- 이번 버전은 **실사용 피드백으로 확인된 환경 탐지 오경고를 줄이는 후속 패치**입니다.
- `msdia140.dll`이 게임의 `SKSE\Plugins`에만 있어도 분석기가 실제로 찾고 사용할 수 있게 했습니다.
- MO2 환경에서 `plugins.txt`를 놓쳐 `Could not find plugins.txt`가 뜨던 케이스를 더 잘 따라가도록 보강했습니다.
- 플러그인 헤더를 읽지 못한 경우에는 `ESP_FULL_SLOT_NEAR_LIMIT`를 고신뢰 경고처럼 띄우지 않도록 조정했습니다.

### 수정
- **Engine: bundled `msdia140.dll` 탐지 보강** — 분석기 프로세스 DLL 검색 경로에 없더라도, 게임 설치의 `Data\SKSE\Plugins\msdia140.dll`을 직접 찾아 로드할 수 있게 조정.
- **Helper: MO2 profile 탐지 보강** — `usvfs_x64.dll`/`uvsfs64.dll` 모듈 실제 경로를 이용해 `ModOrganizer.ini`와 활성 profile의 `plugins.txt`를 찾는 fallback을 추가.
- **Preflight: non-ESL 슬롯 경고 false positive 완화** — 플러그인 헤더를 읽지 못한 항목은 `slot_type_known`으로 분리하고, 슬롯 분류가 불완전할 때는 `254 슬롯 근접` 경고를 대략적 검사로 낮춰 표시.

### 테스트
- plugin scanner 가드 테스트에 `slot_type_known`, MO2 module-path fallback, plugin stream 계약 검증을 추가.
- preflight 가드 테스트에 `slot-limit check is approximate` 계약을 추가.
- analysis engine runtime 테스트에 bundled `msdia140.dll` 탐지 소스 계약을 추가.
- Linux 전체 테스트 `57/57` 통과.
- Windows native build / WinUI build / Packaging(`--no-pdb`) / release gate 확인.

## v0.2.46 (2026-03-30)

### 한눈에 보기
- 이번 버전은 **WinUI에서 바로 보이는 작은 불편과 지원 혼선을 줄이는 유지보수 업데이트**입니다.
- `Raw Data` 탭의 긴 텍스트를 더 직접적으로 스크롤해서 볼 수 있게 했습니다.
- `Triage` 탭에서 `Evidence` 패널을 접고 펼칠 때 페이지 폭이 흔들리는 현상을 줄였습니다.
- `address_db` 로딩 실패 메시지를 더 구체적으로 나눠, 파일 누락인지 게임 버전 미지원인지 바로 구분할 수 있게 했습니다.

### 수정
- **WinUI: Raw Data 텍스트 박스 스크롤바 명시** — `WCT JSON`은 가로/세로 스크롤을 모두 직접 사용할 수 있게 하고, `Report`는 세로 스크롤을 안정적으로 노출해 긴 출력 확인이 쉬워지도록 조정.
- **WinUI: Triage 레이아웃 폭 흔들림 완화** — 루트 스크롤 영역이 세로 스크롤바 폭을 항상 예약하도록 바꿔 `Evidence` expander 확장/축소 시 본문 폭이 변하는 현상을 줄임.
- **Engine: address_db 진단 세분화** — `address_db/skyrimse_functions.json` 실패를 단일 문구로 뭉뚱그리지 않고, `파일 없음`과 `현재 game_version 항목 없음`을 구분해서 보고하도록 개선.

### 테스트
- WinUI XAML 가드 테스트에 `Raw Data` 스크롤바 계약과 루트 스크롤바 폭 고정 계약을 추가.
- AddressResolver 런타임 테스트에 load status 분기와 `missing file / missing game version` 구분 케이스를 추가.
- Linux 전체 테스트 `57/57` 통과.
- Windows native build / WinUI build / Packaging(`--no-pdb`) / release gate 확인.

## v0.2.45 (2026-03-25)

### 한눈에 보기
- 이번 버전은 **CTD 원인 후보를 더 쉽게 읽고 더 덜 헷갈리게 보여주는 업데이트**입니다.
- Crash Logger가 같이 있는 경우, Tullius가 **Crash Logger가 가리키는 DLL 후보를 전보다 더 앞에, 더 직접적으로 보여줍니다.**
- Crash Logger가 없어도, Tullius 단독 callstack 분석 결과를 **약한 추정과 구분해서** 읽기 쉽게 정리했습니다.
- freeze / hang 진단은 **근거가 부족하면 무리하게 단정하지 않고**, 합의된 신호가 있을 때만 더 강하게 보여주도록 조정했습니다.
- 공유 텍스트와 텍스트 리포트도 정리해서, **왜 이 후보를 의심하는지**를 예전보다 바로 이해하기 쉬워졌습니다.

### 추가
- **CTD: Crash Logger frame-first 해석 경로 강화** — direct fault DLL, 첫 actionable probable frame, same-DLL streak, C++ exception module을 CTD 후보 승격의 핵심 신호로 반영. EXE/system victim 크래시에서도 Crash Logger가 강하게 가리키는 DLL 후보를 summary/report/WinUI/share text에서 먼저 보여주도록 개선.
- **CTD: Tullius 단독 callstack 해석 경로 보강** — Crash Logger가 없는 상태에서도 강한 stackwalk-only 후보를 별도 경로로 드러내고, 약한 stack-scan 단서와 구분해 표시하도록 정리.
- **Capture quality: richer crash dump profile 도입** — crash / crash recapture profile에 `process_thread_data`, `full_memory_info`, `module_headers`, `indirect_memory`, `ignore_inaccessible_memory`를 배선하고, callback-shaped dump bootstrap을 추가해 더 나은 CTD 해석 입력을 확보.
- **Freeze/PSS: snapshot + WCT 합의 품질 노출** — freeze snapshot flags를 `VA_SPACE` / `SECTION_INFORMATION`까지 확대하고, WCT 2회 캡처 기반 `cycle_consensus`, `consistent_loading_signal`, `capture_passes` 메타데이터를 summary/report에 기록.

### 수정
- **CTD: ambiguous candidate 노이즈 완화** — `frame`이 이미 합의된 후보를 `object-ref/history` 보조 신호가 불필요하게 `conflicting`으로 끌어내리던 경로를 줄이고, `frame + first-chance`, `frame + history`, `frame + near resource provider` 같은 다중 신호를 더 자연스럽게 보여주도록 조정.
- **공유/리포트: 해석 경로를 직접 노출** — WinUI 공유 텍스트와 텍스트 리포트에 `CrashLogger reading path`, `Next action`, `capture quality`, `freeze support_quality`를 직접 표시해 사용자가 왜 그런 결론이 나왔는지 바로 볼 수 있도록 정리.
- **Freeze: legacy WCT 샘플 보수 해석 유지** — 새 consensus 메타데이터가 없는 과거 hang dump는 `freeze_ambiguous` / `live_process` 수준으로 안전하게 내려가도록 재검증.

### 테스트
- Crash Logger 최소 excerpt fixture 6종과 share text fixture 5종을 추가해 `parser -> candidate -> summary -> WinUI/share text` 회귀를 고정.
- capture profile / dump writer / incident manifest / freeze consensus 가드 테스트를 확장.
- Linux release CI에 `.NET 8 SDK` setup을 추가하고, share text fixture runner가 non-WSL Linux 경로를 직접 사용하도록 조정.
- Linux 전체 테스트 `57/57` 통과, Windows native build / WinUI build / packaging / release gate 확인.

## v0.2.44 (2026-03-24)

### 수정
- **Helper: machine-code-aware dump capture 보강** — 기본 crash profile과 crash recapture profile에서 `MiniDumpWithCodeSegs`를 함께 요청하도록 변경. 외부 reverse-engineering/disassembly 도구가 dump 안에서 기계어 바이트를 찾지 못해 `not found machine code`로 실패하던 사례를 완화.
- **Dump metadata: code segment 포함 여부 노출** — incident manifest, summary JSON, report text에 `include_code_segments` / `CaptureProfileCodeSegments`를 기록해 실제 캡처 프로필을 사후 확인할 수 있도록 정리.
- **문서/배포 INI: DumpMode=1 설명 보정** — 배포용 `SkyrimDiagHelper.ini` 주석을 현재 기본 프로필(`WithThreadInfo+HandleData+UnloadedModules+CodeSegs`)에 맞게 갱신.

### 테스트
- dump profile/source guard 테스트에 code-segment 캡처 계약 검증 추가.
- incident manifest / output snapshot 테스트에 code-segment 메타데이터 출력 검증 추가.
- Linux: `ctest --test-dir build-linux-red --output-on-failure` 통과(`55/55`).

## v0.2.43 (2026-03-23)

### 수정
- **Helper: blank `OutputDir` 기본 출력 하위 폴더 적용** — `OutputDir=`를 비워 두면 기본 출력 위치 바로 아래가 아니라 `Tullius Ctd Logs` 하위 폴더를 사용하도록 변경. MO2 `overwrite`가 빠르게 지저분해지는 문제를 완화.
- **WinUI: 새 기본 출력 폴더 자동 발견** — blank `OutputDir` 환경에서 `Tullius Ctd Logs` 하위 폴더를 우선 스캔하고, 기존 legacy 기본 위치도 함께 찾아서 업데이트 직후에도 기존 dump를 계속 발견할 수 있도록 조정.
- **문서/배포 INI: `OutputDir` 사용법 명확화** — 따옴표 불필요, 상대경로 허용, blank 값의 의미를 README/한글 문서/Nexus 설명/배포용 ini에 맞춰 정리.

### 리팩터링
- Helper: 기본 출력 경로 계산 헬퍼를 정리하고, 더 이상 쓰지 않는 중복 기본 경로 처리 코드를 제거.

### 테스트
- helper 설정 가드 테스트에 기본 `Tullius Ctd Logs` 계약 검증 추가.
- WinUI 자동 dump 발견 가드 테스트에 새 기본 출력 하위 폴더 및 legacy fallback 검증 추가.

## v0.2.42 (2026-03-05)

### 추가
- **DumpTool: CrashLogger ESP/ESM 오브젝트 참조 파싱** — CrashLogger의 POSSIBLE RELEVANT OBJECTS / REGISTERS 섹션에서 크래시 시점에 처리 중이던 게임 오브젝트의 소속 ESP/ESM을 파싱. 게임 EXE 내부 크래시에서 DLL 기반 용의자를 특정할 수 없을 때 "어떤 모드의 오브젝트를 처리 중이었는지" 증거와 권장 조치를 제공.
- **DumpTool: CrashLogger FormID 파싱** — `[0xFEAD081B]` 형태의 FormID를 ESP 참조와 함께 추출. JSON/텍스트 출력, 근거(Evidence), 권장사항, WinUI Quick Summary·공유·클립보드 텍스트에 FormID 표시. xEdit에서 문제 오브젝트를 바로 찾을 수 있는 핵심 정보 제공.
- **Helper: NGIO 잔디 캐싱 모드 자동 감지** — Skyrim 루트에 `PrecacheGrass.txt`가 있으면 크래시/행 감지를 모두 억제하고 경량 대기 루프로 전환. MO2 GrassPrecacher의 자동 재시작 사이클이 Helper 팝업에 의해 방해받지 않음.
- Helper: `SuppressDuringGrassCaching` INI 옵션 추가 (기본값 1). 0으로 설정 시 잔디 캐싱 모드 감지 비활성화.
- DumpTool: `IsSystemishModule` D3D/DXGI/OpenGL/디버깅 DLL 13종 추가 — 그래픽 드라이버 DLL이 용의자로 잘못 표시되는 문제 완화.
- DumpTool: `TroubleshootingGuideDatabase` 클래스 추출 — 트러블슈팅 가이드 매칭 로직을 독립 클래스로 분리, 재사용 가능.
- DumpTool: 리소스 로그 보존 상한 80→120 확대.
- DumpTool: CrashLogger 타임스탬프 파싱 함수 (`TryExtractCompactTimestampFromStem`, `TryExtractDashedTimestampFromStem`) 추가 + 검증 테스트 13개.

### UI
- **WinUI: Glassmorphism + Gradient 비주얼 향상** — AcrylicBrush 반투명 카드 배경, cyan→purple 그라데이션 악센트 바/보더, KPI 카드 상단 그라데이션 바, Suspects 좌측 그라데이션 스트라이프, 섹션 아이콘(FontIcon) 추가, 2컬럼 글로우 디바이더, ANALYZE NOW 버튼 그라데이션 적용.

### 수정
- **WinUI: Quick Summary 카드에 CrashLogger ESP/ESM 우선 표시** — 기존에 DLL 스택 스캔만 표시하던 Quick Summary 카드, 후보 목록, 공유 텍스트를 CrashLogger ESP 참조 우선으로 개편. 요약 문장과 UI가 일치하도록 수정.
- DumpTool: CrashLogger 시간 매칭 창 30분→5분 축소 — 무관한 과거 로그 매칭 방지.

### 리팩터링
- **코드 중복 대폭 제거**: `ConfidenceText` 5곳→I18nCore.h 1곳, `MakeKernelName` 2곳→SkyrimDiagProtocol.h, `Hex32`/`Hex64` 2곳→HexFormat.h.
- DumpTool: 스코어링 매직넘버 14개를 명명 상수로 전환 (`kWeightDepth0`, `kHighConfMinScore` 등).
- DumpTool: `CrashLoggerRankBonus` 매직넘버 5개 상수화.
- DumpTool: `ScopedHistoryFileLock` 디렉토리 기반→Windows Named Mutex 전환 — 프로세스 크래시 시 잠금 자동 해제.
- DumpTool: `AnalyzeDump()` 550줄 → 11개 서브함수 분할 (`LoadSupportDatabases`, `IntegrateCrashLogger`, `RunStackwalk` 등).
- DumpTool: `BuildEvidenceItems`/`WriteOutputs` 분리, `isActionableSuspect` 중복 제거, `CrashLoggerParseCore.h` → `.h/.cpp` 분리.
- DumpTool: 진단 로깅 인프라 — `AnalysisResult.diagnostics` 벡터로 데이터 로드 실패, CrashLogger 통합 에러, 스택워크 폴백 등 8개 경로에서 best-effort 실패 메시지 수집. JSON/텍스트 출력 + WinUI 표시.
- Helper: Win32 HANDLE RAII 래퍼 `UniqueHandle` 도입 — `CreateFileW`/`CreateMutexW`/`OpenProcess` 등 수동 `CloseHandle` 6곳 제거.
- WinUI: **MVVM 패턴 적용** — `MainWindow.xaml.cs` 1053→465줄(56% 감소). 상태/컬렉션/텍스트 빌더를 `MainWindowViewModel.cs`로 분리. 코드비하인드는 UI 바인딩·이벤트 핸들러만 담당.
- Plugin: 워치독 스레드 `std::thread::detach()` → `std::jthread` + `stop_token` — DLL 언로드 시 안전한 종료.

### 인프라
- `.clang-tidy` 정적 분석 설정 — bugprone/performance/modernize 규칙.
- CI: ASan+UBSan 빌드 잡 추가 (`linux-asan`).
- libFuzzer 퍼징 하네스 2개 추가 (`fuzz_crashlogger_parser`, `fuzz_wct_parser`) + 시드 코퍼스.

### 테스트
- CrashLogger 타임스탬프 파싱 테스트 13개 추가 (Compact/Dashed 포맷, 유효성 검증, 엣지케이스).
- CrashLogger ESP/ESM 오브젝트 참조 파싱 테스트 16개 추가 (바닐라/CC 필터, Modified by 스킵, 유니코드 이름, 스코어링, 집계, malformed 입력 방어).
- CrashLogger FormID 파싱 테스트 10개 추가 (`ExtractFormIdBefore`, `ExtractEspRefsFromLine`, 전파/집계 검증).
- WCT JSON 파싱 테스트 11개 추가 (빈 입력, 사이클 우선순위, maxN 제한, capture 파싱).
- MO2 경로 추론 테스트 10개 추가 (대소문자, 역슬래시, 빈 입력 방어).
- 진단 로깅 가드 테스트 추가 (소스 파일 내 diagnostics 인프라 존재 검증).
- 총 테스트 수: 47개 (기존 26개 → 47개).

## v0.2.41 (2026-02-28)

### 추가
- **Plugin: 크래시 예외 필터를 블랙리스트 방식으로 전환** — 기존 화이트리스트(15개 코드) 대신 블랙리스트(5개 무해 코드 제외)를 적용. `EXCEPTION_NONCONTINUABLE_EXCEPTION`, `STATUS_FATAL_APP_EXIT`, 모드 커스텀 예외 등 이전에 누락되던 크래시를 자동 감지.
- **Plugin: 모드 메뉴 이름 자동 표시** — MenuOpen/Close 이벤트 payload에 메뉴 이름 UTF-8 문자열을 인라인 저장. 모든 모드 메뉴가 해시 대신 실제 이름(`SKI_WidgetMenu`, `TrueHUD` 등)으로 표시됨 (구버전 덤프 하위 호환).
- **DumpTool: 이벤트 로그 가독성 개선** — `FormatEventDetail` 구현으로 PerfHitch, MenuOpen/Close(FNV-1a 해시 → 알려진 메뉴 이름 역해석), Heartbeat/CellChange에 사람이 읽을 수 있는 요약 텍스트 자동 생성. 프리징 직전 10초 이내 이벤트 컨텍스트 요약을 Evidence에 추가.
- **DumpTool: 크래시 히스토리 상관 분석** — 동일 `crash_bucket_key` 반복 발생 시 Evidence에 "반복 크래시 패턴" 표시 + Summary JSON에 `history_correlation` 필드 출력.
- **DumpTool: 트러블슈팅 가이드 시스템** — 크래시 유형별(ACCESS_VIOLATION, D6DDDA, C++ Exception, 프리징, 로딩 중 크래시, 스냅샷) 단계별 가이드 6개를 자동 매칭.
- Helper: `PreserveFilteredCrashDumps=1` INI 옵션 추가 — 거짓양성 필터가 삭제하려는 덤프를 보존하여 크래시 미감지 원인 진단 가능.
- Helper: Preflight에 비-ESL 플러그인 240개 초과 경고 및 알려진 비호환 모드 조합 체크 추가.
- WinUI: Discord/Reddit 커뮤니티 공유용 이모지+마크다운 포맷 복사 버튼 추가.
- WinUI: 동일 패턴 반복 시 "동일 패턴 N회 반복 발생" 배지 표시.
- WinUI: 접이식 트러블슈팅 체크리스트 UI 추가.
- WinUI: 이벤트 탭에서 `detail` 필드 기반 가독성 높은 포맷으로 표시.

### 수정
- DumpTool: `MissingMasters` 판정의 암묵 런타임 마스터 예외 목록에 `_ResourcePack.esl`/`ResourcePack.esl`를 추가해 false positive 완화.
- DumpTool: 정상 종료 덤프에 대한 CTD/BEES 힌트 억제 — 스냅샷 유사 인시던트에서 크래시 전용 라벨과 권장사항을 게이트.
- DumpTool: CrashLogger 상관 용의자 순위를 우선하도록 랭킹 로직 보정.
- DumpTool: JSON 데이터 파일 로드 시 `version` 필드 필수화 + 잘못된 항목 스킵/경고 로그.
- Helper: 정상 종료(exit_code=0) 시 크래시 뷰어 팝업 억제 강화.
- Helper: exit_code=0이면서 강한 크래시 증거(strong-crash)가 있는 경우 크래시 뷰어를 지연 실행하도록 개선.
- Helper: 크래시 뷰어 실행 결과를 확인하고, 실패 시 Win32 에러코드/경로를 로그에 기록.
- WinUI: bare catch 블록에 진단 로깅 추가.

### 리팩터링
- Helper: `HandleCrashEventTick()` 395줄 → ~130줄로 축소 — 6개 함수 추출 및 종료 예외 판정 로직 통합.
- DumpTool: `internal::` 래퍼 함수 4개를 제거하고 `minidump::` 단일 네임스페이스로 통합.
- DumpTool: EvidenceBuilder 파일 재구성 — `EvidenceBuilderInternals*` → `EvidenceBuilder*`로 간결화.
- DumpTool: AnalysisSummary JSON 파싱 헬퍼 추출, `WideLower` 유틸 통합.

### 테스트
- CrashLogger 파서 엣지케이스 테스트 20개 추가 (기존 18개 → 38개).
- 크래시 캡처 필터/리팩터링 구조 검증 가드 테스트 추가.
- 이벤트 가독성/메뉴 이름 인라인 저장 가드 테스트 10개 추가.
- AnalysisSummary 헬퍼 구조 가드 테스트 추가.
- Linux: `ctest --test-dir build-linux-test --output-on-failure` 통과(`44/44`).

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
