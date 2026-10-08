# ADR-0006: Modal Dialog Hang Classification

## Status
Accepted

## Context
SKSE 플러그인이나 SKSE 런타임이 로드 오류를 `MessageBoxW`로 띄우면 게임 메인
스레드는 사용자 입력을 기다리는 modal 루프에 머뭅니다. Heartbeat가 멈추므로
헬퍼는 정상적으로 프리징 덤프를 남기지만, 분석기는 이 상태를 인식하지 못했습니다.

- `user32.dll`, `win32u.dll` 프레임은 systemish 모듈로 점수에서 조용히 제외됩니다.
- 대화상자를 띄운 쪽이 SKSE 런타임(hook framework)이나 게임 EXE이면, 훅 강등과
  actionable 필터 때문에 스택 더 아래의 무관한 DLL이 후보로 올라갑니다.
- 정식 stackwalk가 실패하면 메인 스레드 포인터 스캔이 스택 메모리의 오래된 반환
  주소를 집계합니다.

GitHub issue #3은 v0.2.53에서 이 경로로 무관한 `ColdBreathNG.dll`이 지목된 사례를
보고했습니다. 2026-10-01 재현 테스트에서는 정식 stackwalk가
`win32u!NtUserWaitMessage` 한 프레임에서 멈춰 포인터 스캔으로 넘어갔습니다. 공용
stackwalk 루프는 `StackWalk64`의 첫 호출(컨텍스트 자신의 프레임)을 진전 없음으로
보고 멈추며, hang 덤프에는 시스템 DLL의 unwind 데이터 메모리도 없습니다.

## Decision

1. 헬퍼는 hang 캡처 시 대상 프로세스의 보이는 `#32770` 창(MessageBox, DialogBox,
   TaskDialog)을 열거해 WCT JSON의 `modal_dialogs`에 소유 스레드, 제목, 본문,
   owner 비활성 여부를 기록한다. `GetWindowTextW`만 사용하고 대상에 메시지를 보내지
   않으므로 멈춘 대상이 헬퍼를 막을 수 없다. 필드가 없으면 구버전 캡처로 본다.
2. 분석기는 프리징 기준 메인 스레드(ADR-0005)에 대해 두 근거를 따로 판정한다.
   - 창 근거: 메인 스레드가 소유한 대화상자가 `modal_dialogs`에 있다.
   - 스택 근거: 메인 스레드 상위 24프레임 안에 `user32` MessageBox/DialogBox 계열
     또는 `comctl32` TaskDialog 진입점(변위 `0x800` 이하)이 있고, 그 위 프레임이
     모두 시스템 모듈이다.
3. 스택 근거는 공용 정식 stackwalk의 메인 스레드 프레임을 사용한다. unwind 방식은
   ADR-0007을 따른다.
4. 대화상자를 연 모듈은 modal API 아래 첫 비시스템 프레임이다. 게임 EXE, SKSE
   런타임, hook framework에서 멈추며 그보다 아래 모듈로 승격하지 않는다.
5. 둘 중 하나라도 성립하면 프리징 상태는 `modal_dialog_wait`이며 다른 상태보다 먼저
   판정한다. 두 근거가 모두 있으면 `High`, 하나면 `Medium`이다.
6. modal 대기에서는 Crash Logger frame, 스택 밀도, 스레드 그룹, 인접 리소스 후보를
   만들지 않는다. 호출자가 일반 플러그인일 때만 `modal_dialog_owner` 후보를
   `related / Medium`으로 만든다. 스택 suspect는 `Low`로 낮추고 원인이 아님을 표시한다.
7. 대화상자 본문의 사용자 프로필 경로 이름은 `<user>`로 가리고 줄바꿈은 ` / `로
   합친다.
8. 본문이 CommonLibSSE(-NG)의 Address Library 오류 문구이면 종류를 붙인다.
   - `plugin_incompatible`: "Unsupported address library format",
     "Failed to find the id within the address library". 플러그인 빌드가 현재 게임
     버전을 지원하지 않으므로 맞는 파일로 바꾸라고 안내한다.
   - `address_library_missing`: "Failed to locate an appropriate address library",
     "failed to open address library file". 이 게임 버전용 Address Library를
     설치하라고 안내하고, 이미 있다면 다른 판(SE/AE)용 빌드일 수 있다고 덧붙인다.
   - 판정은 본문만 보며 신뢰도와 후보는 바꾸지 않는다. 호출 모듈을 못 찾으면
     CommonLib이 제목에 넣는 플러그인 파일 이름으로 대상을 부른다.

## Output Contract

요약 JSON의 `freeze_analysis.modal_dialog_wait`에는 다음이 기록된다.

- `detected`, `window_evidence`, `stack_evidence`, `main_thread_id`
- `wait_api`, `dialog_title`, `dialog_text`
- `caller_module_filename`, `caller_inferred_mod_name`
- `caller_kind` (`plugin` / `skse_runtime` / `hook_framework` / `game_exe` / `none`)
- `address_library_issue` (`plugin_incompatible` / `address_library_missing` / 빈 문자열)
- `other_thread_dialog_count`

## Consequences

### Positive

- 사용자가 볼 수 있는 대화상자 자체가 원인 안내가 되고, 스택 구경꾼 모듈 지목을 막는다.
- 창 근거는 기호나 unwind 데이터와 무관하게 동작한다.

### Limitations

- 메인 스레드 ID를 추론할 수 없는(blackbox가 없는) 덤프에서는 판정하지 않는다.
- 다른 PC에서 분석하면 로컬 이미지가 맞지 않아 스택 근거가 빠지고 창 근거만 남을 수 있다.
- modal 루프가 디스패치한 플러그인 콜백이 메인 스레드에서 실행 중이면 스택 근거는
  성립하지 않는다.
- 공용 stackwalk의 1프레임 제한은 ADR-0007에서 해결했다.

## Verification

- `skydiag_modal_dialog_wait_tests`: 스택 매칭, 호출자 분류, WCT 파싱, 근거 결합, 경로 가림,
  Address Library 오류 문구 분류.
- `skydiag_modal_dialog_recommendation_tests`: Address Library 오류별 요약 문장과 안내.
- `skydiag_freeze_candidate_consensus_tests`, `skydiag_candidate_consensus_tests`: 상태 우선순위와 후보 신뢰도.
- `skydiag_helper_hang_runtime_tests`: 실제 MessageBox의 소유 스레드와 본문 캡처.
- `skydiag_modal_dialog_hang_e2e_tests`: 실제 헬퍼 hang 캡처를 분석기로 분석해 상태,
  두 근거, 호출 모듈, 후보 억제를 확인한다.

## References

- `helper/src/ModalDialogProbe.cpp`
- `dump_tool/src/ModalDialogWait.cpp`
- `dump_tool/src/AnalyzerInternalsStackwalkMemory.cpp`
- `dump_tool/src/FreezeCandidateConsensus.cpp`
- `docs/adr/0005-hang-main-thread-and-thread-group-consensus.md`
