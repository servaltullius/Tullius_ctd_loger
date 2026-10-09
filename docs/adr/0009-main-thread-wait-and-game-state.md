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

## Decision

1. 프리징 캡처에서 modal 대화상자가 아니면, modal 판정과 같은 메인 스레드 프레임(맨 위부터,
   시스템 DLL은 심볼 이름 포함)으로 메인 스레드가 무엇을 하고 있었는지 분류한다
   (`ClassifyMainThreadWait`).
   - `engine_wait`: 맨 위 시스템 프레임에 Sleep 계열이나 동기화 대기 API가 있고, 그것을 부른
     첫 비시스템 프레임이 게임 EXE.
   - `plugin_wait`: 같은 대기를 플러그인 DLL이 불렀다.
   - `graphics_driver_wait`: 대기 위쪽에 그래픽 드라이버 UMD(NVIDIA/AMD/Intel)나 D3D/DXGI가 있다.
     드라이버는 DriverStore 경로라 시스템처럼 보일 수 있어 이름으로 판별한다.
   - `running`: 맨 위에 대기 API가 없다(코드를 실행 중).
   - `unknown`: 맨 위 시스템 프레임에 가까운 이름이 없거나 호출자에 닿지 못했다.
2. `engine_wait`와 `graphics_driver_wait`는 스택 아래쪽 플러그인이 일으킨 대기가 아니므로
   ("방관자 대기"), modal 대화상자와 같은 원칙으로 다룬다.
   - 스택 기반 후보 신호를 만들지 않고, 스택 suspect는 `Low`로 낮추며 호출 경로라고 표시한다.
   - 스레드 그룹 합의, 반복 이력, first-chance 신호는 그대로 쓴다.
   - 요약 문장은 어디서 기다렸는지와 스택 아래쪽 플러그인(호출 경로)을 말하고, 신뢰도는 `Low`.
   - `[Main thread]` / `[메인 스레드]` 안내를 프리징 체크리스트 맨 앞에 두고, `NextAction`은
     modal 대화상자와 같은 우선순위로 이 안내를 고른다.
   - 드라이버 대기 안내에는 감지된 그래픽 인젝터(ENB, ReShade, DXVK)를 적는다.
3. 프리징 상태 id(`freeze_candidate`, `freeze_ambiguous` 등)는 바꾸지 않는다. 대기 분류는 첫
   번째 `primary_reasons`로 들어간다. 데드락, 동기화 정지, 로더 stall 판정과 modal 대화상자가 여전히
   먼저다.
4. 프리징 캡처의 메인 스레드 스택은 0번 프레임부터 16개를 보여 준다. 크래시 스택과 crash bucket
   키는 그대로다.
5. 블랙박스에서 캡처 시점의 게임 상태를 만든다.
   - 열려 있던 게임 메뉴: MenuOpen/MenuClose를 재생해 끝까지 열린 바닐라 메뉴(Console,
     TweenMenu, InventoryMenu 등)만 남긴다. HUD 위젯과 커서 같은 오버레이는 제외한다.
   - 마지막 로딩이 끝난 뒤 지난 시간.
   - 5분 이상의 PerfHitch 공백은 PC 절전이나 최소화로 보고, 언제 끝났는지와 함께 기록한다.
   - 프리징 요약 문장(modal 제외), "캡처 당시 게임 상태" 근거 항목, 콘솔과 긴 공백에 대한
     `[Context]` / `[상황]` 안내에 쓴다. 히치 통계는 그대로 두되 최댓값에 긴 공백이 포함됐다고 적는다.

## Output Contract

- 요약 JSON `freeze_analysis.main_thread_wait`: `kind`, `wait_class`(`sleep`/`sync`), `wait_api`,
  `waiting_module`, `waiting_mod_name`, `path_modules`
- 요약 JSON `freeze_analysis.game_state`: `open_menus`, `seconds_since_load_end`(-1 = 기록 없음),
  `pause_gap_seconds`, `pause_gap_ended_seconds_before`
- 리포트 `FreezeAnalysis` 아래 `main_thread_wait ...`, `game_state ...` 줄

## Consequences

### Positive

- 실사고 프리징 5건 모두에서 근거 없이 지목하던 모듈(CommunityShaders, FSMP)이 빠지고, 메인
  스레드가 엔진이나 드라이버에서 기다렸다는 사실과 당시 상황(콘솔, 84분 공백)이 결론에 나온다.

### Limitations

- 엔진이 무엇을 기다렸는지(어느 스레드의 잠금인지)는 알려 주지 않는다. 엔진 스핀락의 소유
  스레드를 메모리에서 찾는 방법은 별도로 실현 가능성을 확인한다.
- 블랙박스 링 버퍼보다 먼저 열린 메뉴는 보이지 않는다.
- 시스템 DLL 심볼은 로컬 이미지의 export 이름에 기대므로, 다른 PC에서 분석하면 `unknown`이 늘
  수 있다.

## Verification

- `skydiag_main_thread_wait_tests`(Linux): 실사고 모양의 엔진 대기, 짧은 드라이버 스택,
  플러그인 대기, 실행 중, 판별 불가.
- `skydiag_freeze_candidate_consensus_tests`(Linux): 대기 분류가 첫 이유가 되고 상태 id는
  그대로이며 modal이 우선한다.
- `skydiag_freeze_context_report_tests`: 블랙박스 메뉴 재생, 로딩 후 시간, 긴 공백, 요약·근거·
  안내·`NextAction`, 스택 후보 없음, 크래시 리포트 무변화.
- 보관 중인 실사고 프리징 5건과 modal 1건 재분석.

## References

- `dump_tool/src/MainThreadWait.cpp`
- `dump_tool/src/AnalyzerInternalsBlackbox.cpp`
- `dump_tool/src/FreezeCandidateConsensus.cpp`
- `docs/adr/0006-modal-dialog-hang-classification.md`
