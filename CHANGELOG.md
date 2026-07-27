# Changelog

> **버전 갭 안내:** v0.2.7, v0.2.24, v0.2.38은 RC(Release Candidate)만 배포 후 정식 릴리즈 없이 다음 버전으로 넘어간 번호입니다.

## v0.2.58 (2026-07-28)

### 한눈에 보기
- 이번 릴리즈는 **CTD 증거가 조용히 사라지는 경로를 막고, 릴리즈·CI 검증 체계를 실제로 동작하게 연결**합니다.
- 덤프 쓰기가 일시적으로 실패해도 사고를 잃지 않으며, 정상 종료(exit 0)로 삭제되는 실제 결함은 메타데이터만이라도 남깁니다.
- CrashLogger가 우리보다 늦게 로드되는 로드오더에서도 중첩 예외 억제가 동작합니다.
- 새 기능보다 **기존 기능이 실제로 검증되는지**에 무게를 둔 릴리즈입니다.

### 수정
- **덤프 쓰기 재시도** — 크래시 이벤트는 덤프 기록 전에 소비되고 같은 결함으로 다시 신호되지 않으므로, 일시적 쓰기 실패는 곧 사고 유실이었습니다. 대상 프로세스가 살아 있는 동안 제한된 횟수만큼 그 자리에서 재시도합니다.
- **정상 종료 증거 격리(신규, 기본 켜짐)** — exit 0은 예외가 처리되었다는 신호로 보고 덤프를 삭제하지만, 외부 크래시 핸들러의 `ExitProcess(0)`이나 종료 코드를 정규화하는 런처 때문에 실제 CTD가 0으로 끝나는 경우가 있습니다. 강한 결함이 이미 게시되었고 하트비트 복구가 관측되지 않았다면 초기 필터와 지연된 프로세스 종료 경로 모두에서 `SkyrimDiag_CleanExitEvidence_*.json`을 산출물 삭제 전에 기록합니다. JSON은 캡처 당시 결함 정보와 `dump_preserved` 상태를 보존합니다. 기본 설정에서는 덤프와 파생 리포트를 삭제하고, `PreserveFilteredCrashDumps=1`이면 덤프는 남기되 파생 리포트와 자동 동작만 억제합니다. JSON 쓰기가 실패하면 증거를 모두 잃지 않도록 덤프를 자동 보존합니다. `SkyrimDiagHelper.ini`의 `EnableCleanExitEvidenceQuarantine`으로 JSON 기록을 끌 수 있습니다.
- **늦게 로드되는 CrashLogger 대응** — CrashLogger도 SKSE 플러그인이라 우리 뒤에 로드될 수 있고, 그러면 설치 시점 조회 결과가 비어 중첩 결함 억제가 영구히 꺼졌습니다. SKSE `kPostLoad` 시점부터 모듈 범위를 다시 조회하고, 크래시 핸들러가 찢어진 범위를 절대 관측하지 않도록 write-once 게시 규약으로 공개합니다.
- **양성 예외 코드 분류 보정** — 싱글 스텝, 스레드 이름 설정, `OutputDebugString` 예외를 강한 결함 분류에서 제외해 unsafe `CrashHookMode=2`에서 이런 정상 알림이 clean-exit 증거 JSON의 대상이 되지 않게 했습니다. 기본 `CrashHookMode=1`의 치명적 예외 선택은 기존과 같습니다.
- **분석기 이식성/불필요 복사 수정** — `Mo2Index`의 경로 사본 2곳을 참조로 바꾸고, 상위 코드 유닛이 부호 확장되던 `wchar_t` 폭 확장 경로를 부호 없는 등가 타입 경유로 고쳤습니다.

### 빌드·검증
- **버전 소스 불일치 게이트** — `vcpkg.json`이 `CMakeLists.txt`보다 14개 릴리즈 뒤처진 `0.2.43`에 머물러 있었습니다. 릴리즈 경로가 한쪽만 읽고 둘을 비교하지 않았기 때문입니다. 두 값을 맞추고, 앞으로 불일치하면 릴리즈가 실패합니다.
- **clang-tidy를 CI에 연결** — 설정 파일만 있고 아무도 실행하지 않던 상태였습니다. Linux CI에서 `WarningsAsErrors`로 돌고, 검사 대상 파일 목록은 컴파일 데이터베이스에서 도출하므로 새 분석기 소스가 자동 포함됩니다. Windows 전용 소스는 이 게이트 밖이며, 커버 목록을 CI 로그에 출력해 그 공백이 보이게 했습니다.
- **퍼저를 CI에서 실제 실행** — 크래시 로그 파서는 다른 모드가 쓴 파일을 읽는, 이 프로젝트에서 가장 신뢰할 수 없는 입력을 다룹니다. `crashlogger`/`wct` 파서 퍼저를 CI에서 실행하고, libFuzzer가 새 입력을 첫 번째 코퍼스 인자에 쓰므로 스크래치 디렉터리를 앞에 두어 검수된 시드 코퍼스가 오염되지 않게 했습니다.
- **Windows 테스트를 CI에서 실행** — 헬퍼·플러그인 런타임 테스트는 Windows에서만 빌드되므로, 그동안 로컬 실행에만 의존하고 있었습니다.
- **분석기 동작 회귀 게이트 신설** — `skydiag_quality_corpus_runner`가 raw `CandidateSignal` 픽스처를 production `BuildCandidateConsensus()`에 통과시켜 후보 순위·상태·신뢰도·점수·기권을 생성하고, `skydiag_quality_corpus_gate_tests`가 그 임시 Summary만 품질 채점기에 전달합니다. 미리 계산된 Summary는 소스 코퍼스에 둘 수 없습니다. 이는 **실사고 정확도 측정이 아니라 candidate-consensus 동작 회귀 감지**이며, 정확도 주장은 릴리즈 게이트 7단계가 계속 담당합니다.
- **미측정 상태를 명확히 보고** — 검수된 실사고 코퍼스가 없으면 릴리즈 게이트가 "이번 릴리즈의 실사고 귀속 정확도는 미검증"이라고 명시적으로 출력합니다. 통과로 위장하지 않습니다.
- **CI 배선 자체를 지키는 테스트** — `skydiag_ci_wiring_tests`가 clang-tidy·퍼저·Windows ctest 호출이 워크플로에서 사라지면 실패합니다.
- **태그 릴리즈도 전체 Linux 게이트 실행** — 일반 CI가 버전 태그를 제외하므로, 릴리즈 워크플로가 unit·ASan+UBSan·clang-tidy·parser fuzz를 직접 다시 실행한 뒤 Windows 빌드/패키징으로 넘어갑니다.
- **Windows 테스트 이식성 보정** — source/XAML guard는 CRLF를 LF로 정규화하고, .NET share-text fixture의 stdout은 UTF-8로 명시해 Windows runner의 기본 CP1252 때문에 검증이 실패하지 않게 했습니다.

### 주의사항
- **2차 결함 보존 기능은 이번 릴리즈에서 제외했습니다.** 최초의 강한 결함을 이후 결함으로부터 지키는 기능을 구현했다가 릴리즈 전에 되돌렸습니다. 보존은 설계상 `crash_seq`를 움직이지 않는데 `crash_seq`가 이 프로토콜의 유일한 세대 카운터라, 헬퍼 입장에서 억제 사실이 보이지 않습니다. 버전이 없는 별도 상태 플래그를 더해도 두 프로세스가 하나의 사고를 원자적으로 볼 수는 없어, 검사를 추가할 때마다 다른 인터리빙이 남았습니다. 안전한 구현에는 단일 원자적 선형화 지점을 갖는 프로토콜, 즉 `SharedLayout` 버전 상향과 ADR-0004 호환성 검토가 필요하며 다음 릴리즈 과제입니다. **v0.2.57 대비 동시성 위험은 추가되지 않았습니다.**
- 정상 종료 증거 격리는 기본적으로 JSON 메타데이터만 남깁니다. 덤프가 필요하면 `PreserveFilteredCrashDumps=1`을 켜야 하며, 이 경우 JSON도 덤프가 보존됐다고 명시합니다.
- 검수된 실사고 코퍼스가 여전히 없으므로, 다른 크래시 로거 대비 적중률을 수치로 주장하지 않습니다.
- 플러그인, Helper, 분석기와 WinUI가 함께 바뀌므로 이전 릴리즈 파일과 섞지 말고 zip 전체를 업데이트해 주세요.

### 테스트
- Windows native build: 성공.
- Windows 전체 테스트 `64/64` 통과.
- Ubuntu Linux build: 성공.
- Linux 전체 테스트 `60/60` 통과.
- clang-tidy(`WarningsAsErrors`): clean.
- Windows WinUI self-contained publish: 성공.
- Packaging(`dist/Tullius_ctd_loger_v0.2.58.zip`, `--no-pdb`): 성공 (`87,783,466` bytes, 523 entries, PDB 0개).
- Release gate: `OK` (핵심 PE/Windows App SDK x64, 현재 빌드 해시 일치, 버전 소스 일치).
- 실사고 품질 코퍼스: `SKIPPED (not measured)` — 코퍼스 미제공.
- SHA-256: `ED3C69AA09B2CD05707511B3C2194D80A69E41ACB65ABEC12656C9A5F4F6A501`.

## v0.2.57 (2026-07-23)

### 한눈에 보기
- 이번 릴리즈는 **CrashLogger SSE v1.24 실사고 호환성과 CTD 로그 화면의 읽기 흐름을 함께 개선**합니다.
- 일반 런타임 `CrashLogger.log`를 실제 크래시 로그로 잘못 페어링하지 않으며, 원래 예외가 기록된 뒤 CrashLogger 내부에서 발생한 후속 예외가 크래시 컨텍스트를 덮지 않도록 했습니다.
- 덤프의 표면상 크래시 위치가 CrashLogger이더라도 페어링된 로그에 행동 가능한 비훅 DLL 프레임이 있으면, 해당 후보를 원인 확정이 아닌 우선 점검 대상으로 안내합니다.

### 수정
- **CrashLogger v1.24 로그 판별 강화** — `Thread dump` 문구만 있는 런타임 로그는 제외하고 실제 `Callstack:` 섹션까지 있는 크래시 아티팩트만 페어링 후보로 사용합니다.
- **원래 예외 컨텍스트 보존** — 핸들러 설치 시 CrashLogger 모듈 범위를 캐시하고, 이미 크래시가 고정된 뒤 해당 범위에서 발생한 후속 예외는 최초 컨텍스트를 교체하지 못하게 했습니다.
- **훅 프레임워크 요약 보정** — raw dump가 CrashLogger 같은 훅 프레임워크를 가리킬 때 페어링된 Crash Logger 프레임 기반 비훅 DLL 후보를 우선 점검 대상으로 표시하되, 피해 위치일 가능성과 중간 이하의 신뢰도 표현을 유지합니다.
- **실제 형식 회귀 테스트** — 개인정보를 제거한 CrashLogger v1.24 실제 형식 fixture를 추가하고 런타임 로그 오선택, 직접 오류 프레임, 검색 통합 경로를 검증합니다.
- **WinUI 반응형 배치** — 창의 실제 viewport 너비를 사용하고 좁은 창에서는 CrashLogger 기준, 근거 합의, 다음 조치 카드를 세로로 배치합니다.
- **WinUI 빈 상태와 읽기 순서** — 분석 전과 원시 데이터 없음 상태를 별도 안내하며, 크래시 맥락과 권장 조치 뒤에 검토 피드백이 오도록 화면 순서를 정리했습니다.
- **한국어 문구 정리** — Tullius 콜스택, DLL 점검 안내, 선행 예외, 리포트/원시 데이터와 시작 오류 문구를 자연스러운 한국어로 통일했습니다.

### 주의사항
- Crash Logger 프레임과 Tullius 후보는 원인 확정이 아니라 우선 점검 근거입니다. 검토 완료 실사고 코퍼스가 없으므로 다른 크래시 로거보다 높은 적중률을 수치로 주장하지 않습니다.
- 제공된 기존 v1.24 사고의 재분석으로 로그 선택과 요약 보정은 확인했지만, 최초 예외 보존 변경은 새 v0.2.57 DLL을 설치한 뒤 발생한 실사고로 현장 검증이 한 번 더 필요합니다.
- 플러그인, Helper, 분석기와 WinUI가 함께 바뀌므로 이전 릴리즈 파일과 섞지 말고 zip 전체를 업데이트해 주세요.

### 테스트
- Windows native build: 성공.
- Windows 전체 테스트 `62/62` 통과.
- Windows WinUI self-contained publish: 성공.
- Ubuntu Linux build: 성공.
- Linux 전체 테스트 `58/58` 통과.
- Packaging(`dist/Tullius_ctd_loger_v0.2.57.zip`, `--no-pdb`): 성공 (`87,652,779` bytes, 523 entries, PDB 0개).
- Release gate: `OK` (핵심 PE/Windows App SDK x64, 현재 빌드 해시 일치).
- 실사고 품질 코퍼스: `SKIPPED (not measured)` — 코퍼스 미제공.
- SHA-256: `543F522018031FD078CAF78D459897FAB7228E7AD3CB190F76A1A83BFD5F3B83`.

## v0.2.56 (2026-07-20)

### 한눈에 보기
- 이번 릴리즈는 **정상 종료 오탐 경합, CTD 근거 신뢰도, 런타임 비용과 배포 검증을 함께 보강**합니다.
- 크래시 버킷을 심볼 문자열 대신 모듈명과 RVA로 계산하는 `CTD2` 형식으로 전환하고, 같은 덤프 재분석이 history 통계를 부풀리지 않도록 했습니다.
- Crash Logger 로그가 여러 개 가까운 시각에 존재하면 모호한 페어링으로 표시하고 해당 단서의 신뢰도와 후보 가중치를 낮춥니다.

### 수정
- **정상 종료 분석 경합 제거** — headless 분석기 프로세스를 Helper가 추적하고, 최종 `exit_code=0`이면 프로세스를 종료한 뒤 CTD 파생 산출물을 정리해 늦은 summary 재생성을 막습니다.
- **Crash history 멱등성** — dump 파일명을 대소문자 비구분 키로 사용해 동일 덤프 재분석 결과를 갱신하고, 현재 덤프는 반복 근거 계산에서 제외합니다.
- **Canonical crash bucket v2** — 예외 코드, fault module+RVA, 선택된 callstack의 module+RVA를 해시해 심볼 서버 상태나 함수명 표현 차이에 덜 민감한 `CTD2-*` 키를 생성합니다.
- **Crash Logger 페어링 품질** — 선택 로그의 시간차, 차순위 시간차, 유효 후보 수와 근접 경쟁 로그 수를 summary/report에 기록합니다. 2초 이내 경쟁 로그가 있으면 독립 stack suspect 재정렬과 High 승격을 막습니다.
- **플러그인 핫패스 경량화** — first-chance 예외 rate limit을 모듈 경로 확인보다 먼저 수행하고 고정 버퍼를 사용합니다. heartbeat와 별개인 모듈/스레드 lifecycle 열거 주기를 1초로 낮춥니다.
- **릴리스 hard gate 강화** — 버전명, PDB 부재, 핵심 PE x64, 현재 빌드와 ZIP 내부 파일의 SHA-256 일치, self-contained Windows App SDK 런타임 파일 포함을 검증합니다.
- **실사고 품질 게이트 연결** — 검토 완료 코퍼스와 모든 임계값이 설정된 경우에만 정확도 기준을 강제하며, 코퍼스가 없으면 통과로 오인하지 않도록 `SKIPPED (not measured)`로 표시합니다.
- **WinUI 안내 정리** — v0.2.52+ self-contained 배포와 v0.2.53+ launcher/app 폴더 구조를 README, Beta, Nexus 안내에 맞게 통일했습니다.

### 주의사항
- 새 버킷 키는 `CTD2-` 접두사를 사용하므로 기존 `CTD-` history 그룹과 자동으로 합쳐지지 않습니다.
- 실제 CTD 원인 적중률은 `triage.ground_truth_mod`가 채워진 검토 완료 실사고 코퍼스가 필요합니다. 이번 릴리즈는 합성 테스트만으로 정확도 백분율을 주장하지 않습니다.
- v0.2.52+에서는 .NET Desktop Runtime 8 또는 Windows App Runtime 1.8을 별도로 설치할 필요가 없습니다. 런타임 설치 창이 뜨면 기존 `SkyrimDiagWinUI` 폴더를 제거하고 zip 전체를 다시 설치해 주세요.

### 테스트
- Windows native build: 성공.
- Windows 전체 테스트 `62/62` 통과.
- Windows WinUI self-contained publish: 성공.
- Ubuntu Linux build: 성공.
- Linux 전체 테스트 `58/58` 통과.
- Packaging(`dist/Tullius_ctd_loger_v0.2.56.zip`, `--no-pdb`): 성공 (`87,647,479` bytes, 523 entries, PDB 0개).
- Release gate: `OK` (핵심 PE/Windows App SDK x64, 현재 빌드 해시 일치).
- 실사고 품질 코퍼스: `SKIPPED (not measured)` — 코퍼스 미제공.
- SHA-256: `E10D82377C43CD9ED3E0CE36730B73983804C3C4B8A0AA779DF74531CB59C072`.

## v0.2.55 (2026-07-18)

### 한눈에 보기
- 이번 릴리즈는 **정상 게임 종료를 CTD로 오인하던 문제를 수정한 hotfix**입니다.
- SKSE DLL의 first-chance 접근 위반이 기록되더라도 게임 프로세스의 최종 `exit_code=0`이면 처리된 예외로 판정합니다.
- 정상 종료가 확정되면 CTD 분석, 지연 뷰어, 덤프와 파생 리포트가 사용자에게 실제 CTD처럼 노출되지 않습니다.

### 수정
- **종료 코드 우선 판정** — 예외 코드의 강도와 관계없이 최종 프로세스 종료 코드가 0이면 정상 종료로 확정하고, 비정상 종료(`exit_code!=0`)의 기존 CTD 처리는 유지합니다.
- **strong-exception 우회 제거** — 공유 메모리에 접근 위반이 남아 있어도 정상 종료를 CTD로 되돌리거나 지연 뷰어를 실행하지 않습니다.
- **오탐 산출물 정리** — 진행 중인 headless 분석과 ETW를 중단하고 dump, report, summary, blackbox, WCT, PluginScan, incident manifest를 정리합니다.
- **보존 옵션 정합성** — `PreserveFilteredCrashDumps=1`에서는 원본 dump만 남기고 파생 CTD 산출물과 capture/viewer latch는 제거합니다.
- **회귀 테스트** — `0xC0000005 + InMenu + exit_code=0` 사례와 strong shared-memory crash evidence가 있는 실제 프로세스 종료 경로를 추가했습니다.

### 주의사항
- 외부 크래시 핸들러가 실제 치명적 CTD의 프로세스 종료 코드를 강제로 0으로 바꾸는 매우 드문 환경에서는 해당 사고가 정상 종료로 필터링될 수 있습니다.
- 필터링된 원본 dump를 조사 목적으로 남기려면 `SkyrimDiagHelper.ini`에서 `PreserveFilteredCrashDumps=1`을 사용해야 합니다.

### 테스트
- Windows native build: 성공.
- Windows 전체 테스트 `61/61` 통과.
- Windows WinUI self-contained publish: 성공.
- Ubuntu Linux build: 성공.
- Linux 전체 테스트 `57/57` 통과.
- Packaging(`dist/Tullius_ctd_loger_v0.2.55.zip`, `--no-pdb`): 성공 (`87,614,201` bytes, 523 entries, PDB 0개).
- Release gate: `OK`.
- SHA-256: `C58113A22589947F08504F28B2199031945E746C26251B1D20FC615CE0BB010C`.

## v0.2.54 (2026-07-13)

### 한눈에 보기
- 이번 릴리즈는 **CTD 원인 과단정 방지와 크래시 캡처 정합성 보강** 릴리즈입니다.
- 플러그인과 Helper 사이의 크래시 컨텍스트를 원자적으로 커밋해, 기록 도중의 예외 정보가 덤프에 섞이는 가능성을 줄였습니다.
- 알려진 크래시 서명은 기본적으로 "발생 메커니즘"을 설명하고, 별도의 후보 합의 결과가 실제 근본 원인 후보를 판단하도록 역할을 분리했습니다.
- Crash Logger의 원시 프레임은 보존하되 시스템 DLL, 게임 실행 파일, 훅 프레임워크 같은 비행동 신호가 유력 원인으로 승격되지 않도록 했습니다.

### 수정
- **Crash capture: 커밋 시퀀스 도입** — `crash_seq` seqlock 프로토콜로 플러그인 기록과 Helper 읽기를 동기화하고, 동일한 안정 스냅샷으로 blackbox/exception stream을 생성합니다.
- **Crash capture: 복구·종료 경합 보정** — 복구된 first-chance 예외는 자신이 기록한 시퀀스만 해제할 수 있으며, Helper는 프로세스 종료 전에 대기 중인 crash event를 먼저 처리합니다.
- **Signature: 메커니즘/근본 원인 분리** — 서명 schema를 엄격히 검증하고 `scope`, `mechanism`, `match_confidence`를 출력합니다. `D6DDDA_VRAM`은 SkyrimSE 1.5.97.0의 정확한 접근 위반 패턴인 `D6DDDA_1597_AV`로 좁혔습니다.
- **Candidate consensus: 예외 스레드 우선** — 정상 stackwalk와 fallback scan 모두 예외 스레드를 우선하고, 낮은 품질의 stack 및 capture-quality 신호가 후보 신뢰도를 과도하게 올리지 못하도록 했습니다.
- **Candidate identity/history: 키 충돌 방지** — 후보 키가 구분자와 Unicode를 보존하며, 모호한 v1 history key가 다른 후보를 잘못 boost하지 않도록 history schema v2를 사용합니다.
- **Crash Logger: 로그 페어링 강화** — dump/log artifact 종류를 구분하고 최대 시간 창을 120초로 제한하며, 이름 규칙보다 실제 시간 차이를 먼저 비교합니다.
- **Crash Logger/WinUI: 원시 관측과 행동 후보 분리** — 세 frame field별 eligibility를 summary에 기록하고, WinUI와 권장 조치가 같은 행동 가능성 판정을 사용합니다.
- **품질 게이트: 실사용 표시 순서 측정** — 중복 incident와 충돌 라벨을 fail-closed하고 top-1 accuracy, top-3 recall, High-confidence precision, abstention rate를 분리해 측정합니다.
- **테스트: release build 검증 실효성 강화** — RelWithDebInfo에서도 assertion을 활성화하고, Crash Logger 실제 파일/타임스탬프 통합 테스트와 새 summary schema 회귀 검사를 추가했습니다.
- **개발 도구 정리** — 더 이상 사용하지 않는 Vibekit 스크립트, 에이전트 안내, 전용 CI/테스트를 제거하고 현재 빌드·패키징 계약에 맞췄습니다.

### 주의사항
- 공유 메모리 프로토콜이 v3으로 올라갔으므로 `SkyrimDiag.dll`과 `SkyrimDiagHelper.exe`를 서로 다른 릴리즈에서 섞지 말고 zip 전체를 함께 업데이트해야 합니다.
- 서명의 High confidence는 해당 패턴의 일치 신뢰도이며, 특정 모드·에셋이 근본 원인이라는 자동 확정을 의미하지 않습니다.
- 실제 CTD 원인 적중률은 검토자가 `triage.ground_truth_mod`를 채운 실사고 코퍼스로 별도 측정해야 하며, 합성 테스트 결과를 정확도 백분율로 사용하지 않습니다.

### 테스트
- Windows native build: 성공.
- Windows 전체 테스트 `61/61` 통과.
- Windows WinUI self-contained publish: 성공.
- Ubuntu Linux build: 성공.
- Linux 전체 테스트 `57/57` 통과.
- Packaging(`dist/Tullius_ctd_loger_v0.2.54.zip`, `--no-pdb`): 성공 (`87,612,303` bytes, 523 entries, PDB 0개).
- Package content check: protocol v3 Plugin/Helper와 `D6DDDA_1597_AV` 서명 포함 확인.
- Release gate: `OK`.
- SHA-256: `50090AC11C6E2B99C46ACEBFDD0AA34AA48A22F60F3DB8604F781C9CEF76AEC4`.

## v0.2.53 (2026-05-08)

### 한눈에 보기
- 이번 릴리즈는 **WinUI self-contained 폴더 정리 hotfix**입니다.
- `v0.2.52`에서 런타임 오류를 막기 위해 self-contained 파일을 모두 포함하면서 `SkyrimDiagWinUI` 폴더가 너무 복잡해진 문제를 정리했습니다.
- 이제 사용자는 `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`만 찾으면 되고, 많은 .NET/Windows App SDK 런타임 파일은 `SKSE/Plugins/SkyrimDiagWinUI/app/` 아래로 모입니다.

### 수정
- **WinUI launcher 추가** — Helper가 기존처럼 `SkyrimDiagWinUI\SkyrimDiagDumpToolWinUI.exe`를 실행하면, 작은 네이티브 런처가 `app\SkyrimDiagDumpToolWinUI.exe`를 실행하고 종료까지 기다립니다.
- **Package layout 정리** — 실제 WinUI self-contained 앱, `SkyrimDiagDumpToolNative.dll`, analyzer data 파일을 `SkyrimDiagWinUI/app/` 아래에 배치합니다.
- **Dump discovery 보정** — 실제 WinUI 앱이 `app/` 하위에서 실행되어도 `SkyrimDiagHelper.ini`와 MO2 overwrite 출력 위치를 올바르게 찾도록 경로 추론을 보강했습니다.
- **Helper diagnostics 보정** — WinUI가 즉시 종료될 때 안내하는 로그 경로를 새 launcher/app 레이아웃에 맞췄습니다.

### 테스트
- Packaging/WinUI dump discovery target tests: 실패 확인 후 통과.
- Linux 전체 테스트 `57/57` 통과.
- Windows native build: 성공.
- Windows WinUI self-contained publish: 성공.
- Packaging(`dist/Tullius_ctd_loger_v0.2.53.zip`, `--no-pdb`): 성공 (`87,689,734` bytes).
- Package layout check: `SkyrimDiagWinUI` 최상위 1개 파일, `app/` 아래 511개 파일.
- Release gate: `OK`.
- WinUI launcher startup smoke: zip 추출본에서 launcher가 5초 이상 정상 실행 상태 유지.
- Windows helper runtime smoke 3종: 통과.

## v0.2.52 (2026-05-08)

### 한눈에 보기
- 이번 릴리즈는 **WinUI 뷰어 런타임 설치 오류 hotfix**입니다.
- `SkyrimDiagDumpToolWinUI.exe`가 Windows App Runtime 1.8/MSIX 설치 상태에 민감하게 실패하던 배포 방식을 self-contained publish로 바꿨습니다.
- 릴리즈 zip 크기는 커지지만, 사용자는 WinUI 뷰어 실행을 위해 .NET Desktop Runtime 8 또는 Windows App Runtime 1.8을 별도로 설치할 필요가 없습니다.

### 수정
- **WinUI 배포: self-contained 전환** — `dotnet publish --self-contained true`와 `WindowsAppSDKSelfContained=true`를 사용해 WinUI 실행 파일 옆에 .NET/Windows App SDK 런타임 파일을 함께 배포합니다.
- **Release gate: zip 크기 기준 갱신** — self-contained WinUI 포함으로 정상 zip 크기가 수십 MB까지 커질 수 있어 hard gate를 100MB로 조정했습니다.
- **문서: 런타임 안내 보정** — v0.2.52+ 릴리즈에서는 별도 Windows App Runtime 1.8 설치가 필요 없다는 점과, v0.2.49~v0.2.51의 기존 framework-dependent 동작을 명확히 적었습니다.

### 테스트
- Linux 전체 테스트 `57/57` 통과.
- Windows native build: 성공.
- Windows WinUI self-contained publish: 성공 (`build-winui`, 330개 파일 / 약 213MB uncompressed).
- Packaging(`dist/Tullius_ctd_loger_v0.2.52.zip`, `--no-pdb`): 성공 (`87,572,243` bytes).
- Release gate: `OK`.
- WinUI startup smoke: `SkyrimDiagDumpToolWinUI.exe`가 5초 이상 정상 실행 상태 유지.

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
