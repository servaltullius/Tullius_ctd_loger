# ADR-0009: Main-Thread Wait Classification and Game State for Freezes

## Status
Accepted

## Context
2026-10-09에 사용자의 실사고 프리징 덤프 5개를 다시 분석했습니다. 다섯 건 모두 결론이 약하거나
틀렸습니다.

- 4건은 메인 스레드가 `ntdll!NtDelayExecution ← KERNELBASE!SleepEx ← SkyrimSE.exe+0xe46e21`,
  즉 렌더 패스 도중 게임 엔진 안에서 Sleep으로 대기 중이었습니다. EngineFixes, CommunityShaders,
  FSMP는 그 아래 호출 경로에 있었을 뿐인데, 리포트는 깊은 프레임 하나(점수 1)를 근거로
  CommunityShaders를 실행 우선 후보이자 다음 조치로 올렸습니다.
- 1건은 메인 스레드가 NVIDIA 드라이버(`nvwgf2umx.dll`) 안의 `WaitForSingleObjectEx`에서
  기다리고 있었고, 스택은 드라이버에서 끊겼습니다. 리포트는 포인터 스캔으로 FSMP를 지목했습니다.
- 화면용 스택은 첫 플러그인 프레임 두 개 앞에서 시작해서, 메인 스레드가 무엇을 기다리는지 보여 주는
  맨 위 프레임이 잘렸습니다.
- 블랙박스에는 콘솔이 열려 있었다는 것(3건), 일시정지 메뉴를 연 채 84분 공백이 있었다는 것(1건)이
  기록돼 있었지만 결론에는 쓰이지 않았고, 84분 공백은 `max=5057699ms`로만 나왔습니다.
- 처음에는 엔진 대기를 스핀락으로 짐작해 메인 스레드 레지스터와 스택 근처에서 `{소유 스레드 ID,
  잠금 횟수}` 모양의 포인터를 찾았지만 4건 모두 없었습니다. 덤프에 들어 있던 실행 코드를 읽어 보니
  `SkyrimSE.exe+0xe46e21`은 스핀락이 아니라 `ID3D11DeviceContext::End`(vtable +0xE0) 뒤에
  `GetData`(vtable +0xE8)를 부르고, 결과가 없으면 `Sleep(1)`을 반복하는 루프였습니다. 즉 네 건 모두
  엔진이 **GPU 작업이 끝나기를 기다린** 것이고, 다섯 건 전부가 GPU 쪽 대기였습니다. 디스크의
  SkyrimSE.exe는 Steam DRM으로 코드가 암호화돼 있어 이 확인은 덤프 메모리로만 할 수 있습니다.

## Decision

1. 프리징 캡처에서 modal 대화상자가 아니면, modal 판정과 같은 메인 스레드 프레임(맨 위부터,
   시스템 DLL은 심볼 이름 포함)으로 메인 스레드가 무엇을 하고 있었는지 분류한다
   (`ClassifyMainThreadWait`).
   - `engine_wait`: 맨 위 시스템 프레임에 Sleep 계열이나 동기화 대기 API가 있고, 그것을 부른
     첫 비시스템 프레임이 게임 EXE. Sleep 대기이고, 게임 EXE 프레임의 호출 직전 코드(덤프 메모리에서만
     읽은 0x60바이트)가 "vtable +0xE8 간접 호출(`call [reg+0E8h]`, `GetData`)과 바로 뒤의 결과 검사
     (`test eax,eax` 또는 `cmp eax,1`), 그리고 Sleep 호출로 끝남" 모양이면
     `engine_wait_detail = gpu_query_poll`(GPU 쿼리 결과 대기)로 구체화한다. 주소가 아니라
     코드 모양으로 판별하므로 게임 버전에 묶이지 않고, 덤프에 코드가 없으면 그냥 `engine_wait`로 남는다.
   - `plugin_wait`: 같은 대기를 플러그인 DLL이 불렀다.
   - `graphics_driver_wait`: 대기 위쪽에 그래픽 드라이버 UMD(NVIDIA/AMD/Intel)나 D3D/DXGI가 있다.
     드라이버는 DriverStore 경로라 시스템처럼 보일 수 있어 이름으로 판별한다.
   - `running`: 맨 위에 대기 API가 없다(코드를 실행 중).
   - `unknown`: 맨 위 시스템 프레임에 가까운 이름이 없거나 호출자에 닿지 못했다.
2. `engine_wait`와 `graphics_driver_wait`는 스택 아래쪽 플러그인이 일으킨 대기가 아니므로
   ("방관자 대기"), modal 대화상자와 같은 원칙으로 다룬다.
   - suspect가 메인 스레드 스택에서 나왔을 때만(스택 분석이 WCT cycle 스레드를 고를 수 있다)
     스택 기반 후보 신호를 만들지 않고, 스택 suspect는 `Low`로 낮추며 호출 경로라고 표시한다.
   - 스레드 그룹 합의, 반복 이력, first-chance 신호는 그대로 쓴다.
   - 요약 문장은 어디서 기다렸는지와 스택 아래쪽 플러그인(호출 경로)을 말하고, 신뢰도는 `Low`.
   - `[Main thread]` / `[메인 스레드]` 안내를 프리징 체크리스트에 넣고, `NextAction`은 modal
     대화상자와 같은 우선순위로 이 안내를 고른다. 데드락, 동기화 정지, 로더 stall로 판정됐거나
     스레드 그룹 합의가 있으면 이 안내를 넣지 않는다(그 판정이 먼저이고, 대기는 근거 항목으로
     남는다).
   - 드라이버 대기와 GPU 쿼리 대기(`IsGpuWait`)는 같은 GPU 쪽 안내(드라이버, 그래픽 인젝터·업스케일러,
     비디오 메모리)를 쓰고, 감지된 그래픽 인젝터(ENB, ReShade, DXVK)를 적는다.
3. 대기 분류가 프리징 상태 id를 직접 바꾸지는 않는다. 다만 스택 후보가 빠지므로 같은 덤프가
   `freeze_candidate` 대신 `freeze_ambiguous`가 될 수 있다. 대기 분류는 첫 번째
   `primary_reasons`로 들어가고, 데드락, 동기화 정지, 로더 stall에서는 그 판정의 이유 뒤에
   붙는다. modal 대화상자는 대기 분류를 쓰지 않는다.
4. 프리징 캡처의 메인 스레드 스택은 0번 프레임부터 16개를 보여 준다. 크래시 스택과 crash bucket
   키는 그대로다.
5. 블랙박스에서 캡처 시점의 게임 상태를 만든다.
   - 열려 있던 게임 메뉴: MenuOpen/MenuClose를 재생해 끝까지 열린 바닐라 메뉴(Console,
     TweenMenu, InventoryMenu 등)만 남긴다. HUD 위젯과 커서 같은 오버레이는 제외한다.
   - 마지막 로딩이 끝난 뒤 지난 시간.
   - 5분 이상의 PerfHitch 공백은 PC 절전이나 최소화로 보고, 언제 끝났는지와 함께 기록한다.
   - 시간은 메인 스레드의 마지막 하트비트(프리징이 시작된 때)까지 잰다. 조용히 플레이하면
     블랙박스 이벤트가 쌓이지 않아, 마지막 이벤트를 기준으로 하면 시간이 0에 가깝게 나온다.
     하트비트가 없으면 마지막 이벤트를 쓴다.
   - 프리징 요약 문장(modal 제외), "캡처 당시 게임 상태" 근거 항목, 콘솔과 긴 공백에 대한
     `[Context]` / `[상황]` 안내에 쓴다. 히치 통계는 그대로 두되 최댓값에 긴 공백이 포함됐다고 적는다.

6. 프리징의 버킷 키는 메인 스레드 대기로 정한다(`HANG1-<hash>`). modal 대화상자는 호출 모듈과
   Address Library 오류 종류, `engine_wait`·`graphics_driver_wait`·`plugin_wait`는 대기 종류,
   `engine_wait_detail`, 대기 모듈, 대기 API로 해시한다. 같은 프리징이라도 대기 아래 스택(그때
   호출 경로에 있던 플러그인, 스택 분석이 내려간 깊이)은 캡처마다 달라, 스택 키로는 실사고 GPU 대기
   4건이 모두 다른 버킷이었다. `running`, `unknown`과 크래시는 기존 스택 키(`CTD3-`)를 쓴다.
   대기가 방관자 대기이거나 modal 대화상자이면, 스택 suspect를 이력의 후보 키로 기록하지 않는다.
   기록하면 같은 프리징이 반복될 때마다 버킷 반복 신호가 그 플러그인을 다시 후보로 올린다.

## Output Contract

- 요약 JSON `freeze_analysis.main_thread_wait`: `kind`, `wait_class`(`sleep`/`sync`),
  `engine_wait_detail`(`gpu_query_poll`/빈 문자열), `wait_api`, `waiting_module`, `waiting_mod_name`,
  `path_modules`
- 요약 JSON `freeze_analysis.game_state`: `open_menus`, `seconds_since_load_end`(-1 = 기록 없음),
  `pause_gap_seconds`, `pause_gap_ended_seconds_before`
- 리포트 `FreezeAnalysis` 아래 `main_thread_wait ...`, `game_state ...` 줄
- `crash_bucket_key`: 대기로 분류된 프리징은 `HANG1-` 접두사. 이전 이력의 `CTD3-` 키와는 합쳐지지 않는다.

## Consequences

### Positive

- 실사고 프리징 5건 모두에서 근거 없이 지목하던 모듈(CommunityShaders, FSMP)이 빠지고, 다섯 건 모두
  GPU를 기다렸다는 결론(엔진의 쿼리 대기 4건, 드라이버 대기 1건)과 당시 상황(콘솔, 84분 공백)이 나온다.

### Limitations

- GPU 쿼리 대기라는 것까지만 알려 주고, GPU가 왜 끝내지 못했는지(어느 셰이더나 그리기 작업인지)는
  덤프로 알 수 없다.
- 쿼리 대기가 아닌 엔진 대기(다른 Sleep 루프, 동기화 대기)는 여전히 "무엇을 기다렸는지 모름"이다.
- 블랙박스 링 버퍼보다 먼저 열린 메뉴는 보이지 않는다.
- 시스템 DLL 심볼과 unwind 정보는 같은 버전의 로컬 이미지에 기대므로, 다른 PC나 Windows 업데이트
  뒤에 이전 덤프를 분석하면 스택이 ntdll에서 끊겨 `unknown`이 된다(2026-10-11 업데이트 뒤 실사고
  프리징 5건 모두 그랬다). 캡처 직후의 분석은 같은 DLL이라 영향이 없다. 이후의 덤프는 ADR-0011로 시스템 DLL 데이터를 담는다.
- 게임 EXE가 플러그인이 쥔 SRW 잠금이나 critical section에서 기다려도 `engine_wait`로 분류된다.
  WCT는 SRW 잠금을 보지 못하므로, 스레드 그룹 합의가 없으면 그 플러그인의 스택 후보가 빠진다.
- ENB의 `d3d11.dll`, ReShade의 `dxgi.dll` 같은 프록시 DLL은 이름으로 그래픽 런타임으로 본다.
  프록시 자체 코드에서 기다려도 "그래픽 드라이버 안에서 대기"로 나온다(안내에는 감지된 인젝터를
  적는다).

## Addendum (2026-10-12): the game thread the main thread waits for

원인을 미리 아는 프리징을 실게임에서 일으킨 시험(`tools/crash_tester`)에서 빈틈이 드러났습니다. 1.6.1170에서 하트비트를 기록하는 스레드(창·렌더를 맡는 첫 스레드)는 SKSE `AddTask` 작업을 직접 돌리지 않습니다. 그 작업은 게임의 작업 스레드(`BSJobs::JobThread`)에서 돌고, 첫 스레드는 매 프레임 엔진 안의 `WaitForSingleObjectEx`로 그 작업이 끝나기를 기다립니다. 그래서 플러그인 task가 멈추면 메인 스레드는 "엔진 안에서 대기"로만 보였습니다. 리포트는 메인 스레드 경로의 hdtsmp64.dll(FSMP)만 언급했고, 원인이 다른 두 프리징이 같은 `HANG1-` 버킷으로 묶였습니다.

1. 메인 스레드가 엔진 안에서 동기화 객체를 기다릴 때(`engine_wait`, `wait_class=sync`), 덤프의 ThreadInfoList에서 시작 주소가 게임 EXE 안에 있는 스레드(게임이 만든 스레드)의 스택 위쪽 12프레임을 같은 심볼 세션에서 푼다.
2. 그중 플러그인 코드가 있는 스레드를 찾는다. Windows, 게임, SKSE 런타임, 그래픽 스택, 알려진 훅 프레임워크, SkyrimDiag 자신은 플러그인 코드로 치지 않는다.
   - 순서: 플러그인이 직접 대기·Sleep을 부른 스레드(`waiting_in_plugin`), 플러그인이 부른 엔진 코드가 맨 위인 스레드(`engine_call_from_plugin`), 플러그인 코드가 맨 위인 스레드(`running_in_plugin`). 같으면 플러그인 프레임이 얕은 쪽.
   - 제외: 스레드 시작 → 게임 스레드 함수(3프레임 이하) → 플러그인 → 동기화 대기 모양. 플러그인이 게임 스레드의 루프를 차지하고 일거리를 기다리는 평소 상태다(Hitch Hunter가 시험의 모든 캡처에서 이 모양이었다).
3. 찾으면 `main_thread_wait.blocked_*`에 기록하고:
   - 그 플러그인을 후보 맨 앞에 둔다(신뢰도 중간). 메인 스레드 경로의 모듈은 그대로 낮춘다.
   - 결론 문장, `[메인 스레드]` 다음 조치, 근거 항목, 리포트 `main_thread_wait` 줄, 요약 JSON `blocked_thread`에 적는다.
   - 버킷 키에 플러그인과 상태를 넣어(`|blocked=…|state=…`) 원인이 다른 프리징을 나눈다.
   - 이력 후보로 그 플러그인만 기록한다.

한계: 메인 스레드가 정확히 어느 스레드를 기다렸는지(대기 객체의 주인)는 덤프로 알 수 없어, "그 시각 플러그인 코드 안에 있던 게임 스레드"로 추정한다. 보관 중인 실사고 프리징은 GPU 대기이거나 Windows 업데이트 뒤 스택을 풀 수 없어 이 판정이 걸리지 않았으므로, 실사고에서의 오탐 여부는 아직 검증되지 않았다.

## Verification

- `skydiag_main_thread_wait_tests`(Linux): 실사고 덤프의 실제 코드 바이트로 GPU 쿼리 대기 판별과 그 변형, 실사고 모양의 엔진 대기, 짧은 드라이버 스택,
  플러그인 대기, 실행 중, 판별 불가.
- `skydiag_freeze_candidate_consensus_tests`(Linux): 대기 분류가 첫 이유가 되고 상태 id는
  그대로이며 modal이 우선하고, 데드락에서는 대기 이유가 뒤에 붙는다.
- `skydiag_freeze_context_report_tests`: 블랙박스 메뉴 재생, 로딩 후 시간(하트비트 기준),
  긴 공백, 요약·근거·안내·`NextAction`, 스택 후보 없음, 다른 스레드의 suspect는 후보 유지,
  동기화 정지·데드락에서 `[Main thread]` 안내 없음, 크래시 리포트 무변화.
- `skydiag_share_text_fixture_tests`: WinUI 뷰어와 공유 텍스트가 리포트의 `NextAction`
  (`next_action_index`)을 따른다.
- 보관 중인 실사고 프리징 5건과 modal 1건 재분석.
- 막힌 게임 스레드: `skydiag_main_thread_wait_tests`가 시험 덤프의 스택 모양(플러그인 Sleep, 엔진 스핀, Hitch Hunter 유휴 루프)과 제외 규칙을, `skydiag_crash_test_findings_tests`가 결론·다음 조치·근거·JSON·리포트·버킷·이력을 확인한다. 시험 프리징 3건(`SkyrimDiag_Hang_20261012_191024_970`, `_191131_297`, `_192730_714`)을 다시 분석하면 모두 CrashTester.dll을 가리키고, 앞의 두 건은 같은 버킷, 세 번째는 다른 버킷이 된다.

## References

- `dump_tool/src/MainThreadWait.cpp`
- `dump_tool/src/AnalyzerInternalsBlackbox.cpp`
- `dump_tool/src/FreezeCandidateConsensus.cpp`
- `docs/adr/0006-modal-dialog-hang-classification.md`
