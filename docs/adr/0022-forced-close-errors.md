# ADR-0022: 강제 종료 중 오류 (Alt+F4, 창 닫기, 종료)

## Status
Accepted

## Context
- 2026-10-26 1.6.1170 실사고: 사용자가 Alt+F4로 게임을 강제 종료했고, 그 직후 접근 위반이 났습니다. 헬퍼는 평소 CTD처럼 덤프를 쓰고 뷰어를 자동으로 열었습니다(`reason=crash_exit`, 이어서 `Target process exited (exit_code=1)`).
- 덤프의 메인 스레드는 아직 프레임을 그리던 중이었습니다: `BSGraphics::Renderer::End` → `PrismaUI.dll!PrismaUI::Core::D3DPresent` → `REL::Relocation<void(unsigned)>::operator()` → PrismaUI.dll+0x59ce2의 `call qword ptr [rsp+0x20]` → 0x7FF7055F0000 실행 예외. 그 주소는 덤프의 메모리 정보 스트림에서 `MEM_FREE`였고, SkyrimSE.exe 바로 아래 64KB 간격으로 놓인 4KB `PAGE_EXECUTE_READWRITE` 개인 페이지 줄(후킹 라이브러리의 트램펄린) 중 그 페이지와 이웃 둘이 해제돼 있었습니다. 닫는 동안 풀린 후킹 트램펄린을 다른 플러그인의 present 후킹이 계속 불렀다는 뜻입니다.
- 어느 스레드에도 아직 종료·언로드 코드(ExitProcess, LdrShutdownProcess, FreeLibrary, DestroyWindow)가 없어서, 덤프만 보고는 종료 중이었음을 알 수 없었습니다. 정상 종료 처리(헬퍼의 종료 코드 0 격리, `CleanExitEvidence`)도 종료 코드 1이라 해당하지 않습니다.
- 사용자 결정: 이것은 "게임이 닫히다 난 크래시"가 아니라 사용자가 강제 종료한 것입니다. CTD로 분류하지 않고(크래시 이력·버킷 통계에서 제외), 뷰어를 자동으로 열지 않으며, 덤프·리포트·요약은 그대로 씁니다. 빠른 답은 "Alt+F4로 게임을 강제 종료하는 과정에서 난 오류입니다. 플레이 중에 튕긴 CTD가 아닙니다." / "세이브에는 보통 영향이 없습니다" / "따로 할 일은 없습니다". 상세 분석(예외 명령어, 호출 경로, 모듈)은 모드 제작자에게 도움이 되므로 그대로 둡니다.

## Decision

1. **플러그인: 닫기 요청 기록**(`plugin/src/CloseRequestHooks.cpp`, 공통 규칙 `shared/SkyrimDiagCloseRequest.h`)
   - `kDataLoaded`(게임 창이 있는 시점)에 `RE::Main::wnd`의 스레드에 `SetWindowsHookExW(WH_CALLWNDPROC, …, nullptr, threadId)`로 스레드 훅을 하나 겁니다. 같은 프로세스의 스레드 훅이라 다른 프로세스에 DLL을 넣지 않고, 모듈은 이미 고정(pin)돼 있어 훅 프로시저가 프로세스 수명 동안 유효합니다. 훅은 풀지 않습니다.
   - 보는 메시지: 게임 창(`Main::wnd`, 메시지마다 다시 읽음)으로 **보내진**(sent) `WM_SYSCOMMAND(SC_CLOSE)`와 `WM_CLOSE`. Alt+F4와 X 버튼은 모두 DefWindowProc가 `SC_CLOSE`를, 다시 `WM_CLOSE`를 보내는 경로라 둘 다 잡힙니다. 그 순간 `GetKeyState(VK_MENU)`·`GetKeyState(VK_F4)`(처리 중인 메시지 시점의 입력 상태)가 눌려 있으면 Alt+F4, 아니면 창 닫기, `SC_CLOSE` 없이 온 `WM_CLOSE`(작업 관리자의 작업 끝내기, 다른 프로그램, 게임 자신)는 `close_message`입니다. 같은 종류가 1초 안에 다시 오면 같은 요청으로 봅니다.
   - 게임 자신의 종료(데스크톱으로 나가기): 하트비트 작업(메인 루프 안에서 실행)이 `Main::quitGame`이 꺼짐→켜짐으로 바뀌는 것을 한 번 기록합니다(`game_quit`). 메인 루프가 그 뒤 작업을 더 돌리지 않으면 놓칠 수 있는 최선 노력입니다.
   - 서브클래싱(`SetWindowLongPtr(GWLP_WNDPROC)`) 대신 `WH_CALLWNDPROC`를 고른 이유: ENB·ReShade·오버레이·UI 모드가 창 프로시저를 서브클래싱하는데, 우리가 끼어들면 체인 순서와 해제 순서를 맞춰야 하고 ANSI/Unicode 프로시저 변환 위험도 생깁니다. `WH_CALLWNDPROC`는 창 프로시저를 건드리지 않고, 메시지를 바꿀 수 없으며(문서상 관찰 전용), 항상 `CallNextHookEx`로 넘기므로 메시지 처리를 바꾸지 않습니다. 다른 훅과는 OS의 훅 체인으로 공존합니다.
   - 비용: 보내진 메시지마다 메시지 번호 비교 두 번. 게시(posted) 메시지는 보지 않으므로(`WH_GETMESSAGE` 미사용) 마우스·키 입력 흐름에는 훅이 없습니다. 기록은 기존 블랙박스 링에 잠금 없이 한 번 씁니다.
   - `SkyrimDiag.ini`의 `EnableCloseRequestLog=1`(기본 켬)으로 끌 수 있습니다.
2. **프로토콜**: `EventType::kCloseRequest = 90`, payload a=`CloseRequestKind`(1 alt_f4, 2 window_close, 3 close_message, 4 game_quit), b=창 메시지(game_quit은 0), c=wParam. `kFirstChanceException`(80)을 더할 때처럼 레이아웃과 기존 이벤트의 해석은 그대로이고 이전 헬퍼·분석기는 모르는 종류를 건너뛰므로(분석기는 `Unknown`으로 표시) `kVersion`은 올리지 않습니다(ADR-0004의 버전 증가 조건에 해당하지 않음, 배포는 여전히 한 zip).
3. **규칙**(`ForcedCloseScanner`): 블랙박스에서 가장 최근 `kCrash` 이벤트보다 앞서고 그로부터 `kForcedCloseCrashWindowMs` = **10초** 안에 있는 닫기 요청이 있으면 강제 종료 중 오류입니다. Alt+F4 뒤 스카이림은 메인 루프를 멈추고 렌더러·힙·플러그인 DLL을 정리하며, 큰 로드오더에서는 몇 초 걸립니다. 10초는 느린 정리까지 덮고, 게임이 무시한 닫기 요청 직후 플레이 중 크래시가 날 가능성보다 정리 중 크래시가 훨씬 흔합니다. 여럿이면 Alt+F4 > 창 닫기 > game_quit > close_message(게임이 종료하며 스스로 보낼 수 있음) 순으로 이름을 정하고, 같은 종류면 가까운 것으로 시간을 잽니다. 크래시 뒤의 요청, 더 오래된 크래시 근처의 요청은 세지 않습니다. 헬퍼와 분석기가 같은 상수를 쓰므로 INI로 바꾸지 않습니다(바꾸면 둘이 어긋남).
4. **헬퍼**(`CrashCapture.cpp`): 크래시 이벤트 때 잡은 블랙박스 스냅샷에 규칙을 적용합니다(`FindForcedCloseInSnapshot`). 맞으면 `This crash followed a close request (Alt+F4) 0.8 s earlier: …`를 남기고, 뷰어를 바로 열지도, 종료 시로 미루지도 않습니다(`Skipped viewer auto-open: crash followed a close request (Alt+F4) 0.8 s earlier.`). 핸들된 예외로 풀렸다가 비정상 종료로 다시 보고되는 경우(`PromoteReleasedCrashDump`)도 같습니다. 덤프·매니페스트·헤드리스 분석(리포트·요약)은 그대로 진행합니다. 요약의 `analysis.is_forced_close`가 참이면 재수집 정책용 크래시 버킷 통계(`SkyrimDiag_CrashBucketStats.json`)를 갱신하지 않습니다.
5. **분석기**: 덤프의 블랙박스 스트림에 같은 규칙을 적용해 `AnalysisResult::forced_close`를 채웁니다(예외가 있는 덤프만, 검증된 정상 종료가 있으면 그것이 우선).
   - 정상 종료 플래그(`is_filtered_clean_exit`)를 재사용하지 않고 형제 플래그를 둔 이유: 정상 종료는 헬퍼 사이드카로만 확인되고 "종료 코드 0, 원인 모드 지목 안 함"을 뜻해 후보·예외 명령어 등 분석을 모두 비웁니다. 강제 종료는 덤프 자체로 확인되고 분석을 그대로 남겨야 하며 예외로 끝난(종료 코드 1) 경우입니다. 대신 정상 종료가 하는 제외 처리는 똑같이 따릅니다: 크래시 이력 읽기/추가(`crash_history.json`, 반복 횟수·버킷 상관), 문제 해결 가이드를 건너뜁니다. `is_crash_like`는 참으로 둡니다(분석 경로 유지).
   - 사용자 문장은 항상 사용자의 행동으로 씁니다(게임이 스스로 꺼졌다고 하지 않음). Alt+F4: "Alt+F4로 게임을 강제 종료하는 과정에서 난 오류입니다." / "This error happened while you force-quit the game (Alt+F4).", 창 닫기·close_message: "게임 창을 닫아 게임을 종료하는 과정에서 난 오류입니다." / "This error happened while you closed the game window.", game_quit: "게임 종료를 고른 뒤 게임이 닫히는 과정에서 난 오류입니다." / "This error happened while the game was closing after you chose to quit." 뒤에 "플레이 중에 튕긴 CTD가 아닙니다." / "It is not a crash during play."
   - 빠른 답: `kind`/`case` = `forced_close`, 제목 = 위 두 문장 + "세이브에는 보통 영향이 없습니다." / "Saves are usually unaffected.", 해볼 일 = "따로 할 일은 없습니다." / "Nothing to do.", 의심 모드·확실하지 않은 이유·어떻게 튕겼나 줄 없음.
   - 결론 문장: 위 두 문장을 앞에 붙이고 기존 결론을 그대로 잇습니다. 근거 첫 항목 "강제 종료 중 오류(CTD 아님)" / "Error during a forced close (not a CTD)"(신뢰도 높음, 몇 초 전 무슨 요청인지). 권장 첫 줄 `[강제 종료]` / `[Forced close]`(다음 조치로 선택, 즉시 그룹 `forced_close`). "실제 CTD"라고 하는 C++ 예외 해석 줄과 `CPP_EXCEPTION_HANDLED` 시그니처는 내지 않습니다.
6. **출력 계약**(추가 필드)
   - 요약 JSON `analysis.is_forced_close`(항상), `forced_close`(감지 시): `kind`, `seconds_before_crash`(0.001초 단위), `window_seconds`(10).
   - 리포트: `Summary:` 다음 줄 `Classification: FORCED_CLOSE (not a CTD; alt_f4, 0.8 s before the error)` / `분류: 강제 종료 (CTD 아님; alt_f4, 오류 0.8초 전)`.
   - 블랙박스 이벤트 이름 `CloseRequest`, 상세 `kind=alt_f4 msg=0x0112 wparam=0xF060`.
   - `crash_bucket_key`는 정상 종료처럼 그대로 씁니다(이력에 넣지 않을 뿐).
7. **뷰어**: 정상 종료 표시 경로를 따라 버킷 줄 "분류: 강제 종료 (CTD 아님)" / "Classification: FORCED_CLOSE (not CTD)", 빠른 요약의 분류 "강제 종료 — CTD 아님", 공유 텍스트 머리글 "⚪ Skyrim 강제 종료 중 오류 리포트" / "⚪ Skyrim Forced-Close Error Report"(CTD 리포트 아님), 유형 줄 `FORCED_CLOSE`. 후보 목록은 그대로 보여 줍니다.

## Consequences
- 닫기 요청 기록이 없는 이전 덤프는 아무것도 바뀌지 않습니다.
- 게임 창에 `WM_CLOSE`를 게시(post)하는 닫기(일부 외부 도구)는 보내진 메시지가 아니라서 보지 못합니다. 게시된 `WM_SYSCOMMAND(SC_CLOSE)`는 DefWindowProc가 보내는 `WM_CLOSE`로 잡히되 `close_message`가 됩니다(문장은 창 닫기와 같음). 놓친 경우는 이전과 같이 CTD로 보고됩니다.
- 사용자가 닫기를 요청했는데 게임이 무시하고 10초 안에 플레이 중 크래시가 나면 강제 종료로 잘못 분류됩니다. 이 경우에도 상세 분석은 그대로입니다.
- 플러그인 훅이 설치되지 않으면(창을 못 찾음, 훅 거부) `SkyrimDiag.log`에 `close request log off`가 남고, 동작은 이전과 같습니다.

## Verification
- `skydiag_close_request_tests`(Linux 포함): 메시지 분류(Alt+F4·X 버튼·WM_CLOSE 단독·반복·다른 SC_ 명령), 10초 경계, 크래시 뒤 요청·오래된 크래시·모르는 종류 제외, 우선순위, 링 보관 개수.
- `skydiag_crash_capture_filter_logic_tests`: 헬퍼 로그 문장. `skydiag_helper_false_positive_runtime_tests`: 스냅샷 판정과, 실제 덤프를 쓰는 크래시 이벤트 처리에서 뷰어를 열지도 미루지도 않고 로그를 남기는지.
- `skydiag_quick_answer_tests`: 블랙박스 판정, 이벤트 이름·상세, 한국어·영어 빠른 답·결론 접두·근거·권장·다음 조치, 요약 JSON, 리포트 줄, 종류별 문장, 정상 종료 우선, "실제 CTD" 문구 없음. `skydiag_next_action_tests`, `skydiag_recommendation_class_tests`: `[강제 종료]` 태그.
- `share_text_fixture_tests.py`: 공유 텍스트 머리글·분류·다음 조치.
- 보관 덤프 21건(한국어·영어 42회)을 main 빌드와 나란히 재분석(`scripts/compare_analyses.py`): 닫기 요청 이벤트가 있는 덤프가 없으므로 강제 종료로 분류된 것은 없고(`analysis.is_forced_close` 모두 false), 바뀐 것은 ADR-0019 부록의 `SkyrimDiag_Crash_20261026_140735_958` 빠른 답 "어떻게 튕겼나" 줄(해제된 메모리)뿐입니다.
