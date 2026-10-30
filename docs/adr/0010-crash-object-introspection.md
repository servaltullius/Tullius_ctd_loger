# ADR-0010: Crash Object Introspection from the Dump

## Status
Accepted

## Context
Crash Logger에서 사용자가 가장 많이 보는 부분은 "크래시 당시 레지스터와 스택이 가리킨 오브젝트" 목록입니다. 예를 들어 `RCX: Character "도로롱" [0xFEAD081B] AE_StellarBlade_Doro.esp`처럼, 어떤 게임 오브젝트를 다루다 죽었는지와 그 오브젝트를 정의한 플러그인을 보여 줍니다. 이 분석기는 그동안 같은 크래시의 Crash Logger 로그가 있을 때만 이 정보를 썼습니다.

2026-10-10에 보관 중인 실사고 덤프를 확인했습니다.

- CTD 덤프의 크래시 스레드 레지스터 중 힙 포인터(RCX, RBX, RBP, R12, R13, R15)는 하나도 덤프에서 읽을 수 없었습니다.
- 크래시, 프리징, 수동 덤프 모두 게임 힙을 거의 담지 않습니다(1~15MB). 덤프 크기 대부분은 모듈 이미지입니다.
- SkyrimSE.exe 이미지 59MB 중 덤프에 든 것은 26.8MB로 주로 코드였고, RTTI가 있는 `.rdata`가 들어 있다고 볼 수 없었습니다.

Crash Logger는 게임 프로세스 안에서 돌아 이 메모리를 바로 읽습니다. 프로세스 밖에서 덤프를 쓰는 이 도구가 같은 정보를 얻으려면, 덤프를 쓸 때 그 메모리를 함께 담아야 합니다.

## Decision

1. 판별 로직은 `shared/SkyrimDiagRtti.h` 하나로 둔다. 메모리 읽기 함수만 받는 템플릿이라 헬퍼(실행 중인 게임 메모리)와 분석기(덤프 메모리)가 같은 코드를 쓴다.
   - MSVC x64 RTTI를 읽는다: vtable 앞의 CompleteObjectLocator(signature 1)부터 모듈 기준 주소, 타입 이름(`.?AVCharacter@@`), 클래스 계층(base class descriptor)까지.
   - 계층에 `TESForm`이 고정 오프셋(가상 상속이 아님)으로 있으면 TESForm으로 본다. 그 위치에서 `formID`(0x14), `formType`(0x1A), `sourceFiles`(0x08, `BSStaticArray<TESFile*>`)를 읽고, 각 `TESFile::fileName`(0x58)을 읽는다. 오프셋은 SE와 AE가 같다.
   - 포인터 범위, 정렬, locator signature, 64KB 경계의 모듈 기준 주소, `.?A`로 시작하는 출력 가능 ASCII 이름을 검사해 숫자나 쓰레기 값을 오브젝트로 보지 않는다.
2. 헬퍼는 CTD 덤프를 쓸 때(PSS 스냅샷이 아닐 때), 게임이 크래시 핸들러에서 멈춰 있는 동안 위 로직을 실제 게임 메모리에 돌린다.
   - 대상: 크래시 컨텍스트의 범용 레지스터 15개(RSP 제외)와 RSP부터 0x800바이트의 스택 값.
   - 오브젝트로 판별된 값에 대해 판별 중 읽은 모든 범위와 오브젝트 앞 0x100바이트를 기록한다. 오브젝트가 아니었던 값의 읽기는 버린다.
   - 겹치는 범위를 합친 뒤 `MemoryCallback`으로 덤프에 추가한다. 상한은 2MB다.
3. 분석기는 덤프의 예외 컨텍스트와 스택에서 같은 대상을 같은 순서로 다시 읽어, 같은 로직을 덤프 메모리에만 돌린다. 로컬 모듈 파일로 대신 읽지 않는다.
   - 같은 오브젝트가 여러 곳에서 나오면 Crash Logger의 위치·타입 가중치가 가장 높은 곳을 남긴다.
   - 결과는 관련도순 최대 24개다.
4. 덤프 메모리 읽기는 겹치는 범위를 하나로 펴고, 바로 이어진 범위에 걸친 읽기를 이어서 처리한다. 헬퍼가 추가한 범위가 덤프에 이미 있던 범위 안에 있으면, 이전 방식으로는 그 뒤쪽 주소 읽기가 실패할 수 있었다.
5. 첫 단계에서는 근거 항목(`Low`), 리포트, 요약 JSON으로만 보여 준다. 실행 우선 후보, 신뢰도, 요약 문장에는 쓰지 않는다. 실사고 CTD로 확인한 뒤 Crash Logger 오브젝트 참조처럼 후보 신호로 쓸지 정한다.

## Output Contract

- 요약 JSON `dump_objects[]`: `location`(`RCX`, `RSP+68`), `address`, `type`, `module`, `is_form`, `form_id`, `form_type`, `source_files[]`, `relevance`
- 리포트 `DumpObjects:`(한국어 `덤프 오브젝트:`) 아래 한 줄씩. 예: `RCX: Character [0xFEAD081B] Skyrim.esm -> AE_StellarBlade_Doro.esp`. TESForm이 아니면 `RSI: BSFadeNode (SkyrimSE.exe)`
- 근거 항목 "크래시 당시 레지스터·스택이 가리킨 오브젝트(덤프에서 읽음)" (최대 5개)

## Consequences

### Positive

- Crash Logger 로그가 없어도 크래시가 다루던 게임 오브젝트의 타입, FormID, 정의한 플러그인과 마지막으로 바꾼 플러그인이 리포트에 나온다.
- 헬퍼와 분석기가 같은 코드를 쓰므로 덤프에 무엇을 담아야 하는지가 판별 로직에서 자동으로 정해진다.

### Limitations

- 이 버전 이전의 덤프에는 오브젝트 메모리가 없어 아무것도 나오지 않는다.
- CTD만 대상이다. 프리징과 수동 캡처는 메인 스레드가 엔진 대기 중인 경우가 대부분이라 다음 단계로 미룬다.
- 에디터 ID, NiAVObject 이름, 레퍼런스가 따로 가진 표시 이름(ExtraTextDisplayData)은 아직 읽지 않는다.
- `TESForm` 판별은 RTTI 이름에 기대므로, 게임이 RTTI를 지운 빌드라면 타입만 없고 아무것도 나오지 않는다.
- 덤프 크기는 오브젝트 수에 따라 최대 2MB 늘어난다(보통 수십 KB).

## Addendum (2026-10-12): names and base objects

- 이름: 클래스 계층에서 `TESFullName` 기반 클래스(가상 상속이 아닌 것)의 오프셋(mdisp)을 찾아, 그 위치 +0x08의 BSFixedString(문자 포인터)이 가리키는 문자열을 읽는다. 클래스마다 다른 오프셋을 계층이 알려 주므로 따로 표를 두지 않는다.
- 레퍼런스: `TESObjectREFR` 기반이 있는 폼은 그 위치 +0x40(`OBJ_REFR::objectReference`, CommonLibSSE-NG 기준)의 포인터를 한 단계만 따라간다. 그 대상이 `TESBoundObject` 기반을 가진 폼일 때만 베이스 오브젝트로 인정해, 타입·FormID·플러그인을 기록하고 이름이 없으면 그 이름을 쓴다. 오프셋이 틀리면 엉뚱한 이름 대신 아무것도 나오지 않는다.
- 문자열은 끝(NUL)까지 다 읽혔을 때만 쓴다. 해제·재사용된 메모리에서 도중에 끊긴 바이트는 이름으로 남기지 않는다.
- 문자열은 유효한 UTF-8이면 UTF-8로, 아니면 분석하는 PC의 ANSI 코드 페이지로 읽는다. 짧은 CP949 한글 이름이 우연히 유효한 UTF-8이면 깨져 보일 수 있다(플러그인 파일 이름과 같은 한계).
- 출력: `RCX: Character "Blood Dragon" [0xFF0036F8] (base TESNPC [0x0001D8F6] Skyrim.esm)`. 요약 JSON의 `dump_objects`에 `name`과 `base_object`(`type`, `form_id`, `source_files`)가 더해진다. 배치된 액터는 베이스 NPC를 정의한 플러그인이 나온다. 레벨드 리스트에서 생긴 액터(대부분의 적)는 베이스가 실행 중에 만든 임시 NPC(0xFF…)라 이름만 나오고 플러그인은 나오지 않는다. 원래 NPC를 가리키는 `ExtraLeveledCreature`는 아직 읽지 않는다.
- 후보에는 여전히 쓰지 않는다.

## Addendum (2026-10-22): freezes and manual captures

2026-10-22 프리징(`engine_spin_nan`, SkyrimSE.exe+0xD17465)에서 멈춘 잡 스레드 19588의 rbx/rdi/r12/r14는 힙을 가리켰지만, 기본 DumpMode=1 프리징 덤프에는 힙이 없어 어떤 액터를 다루던 중인지 알 수 없었습니다. 위 Limitations의 "CTD만 대상"을 프리징과 수동 캡처로 넓힙니다.

- 대상 스레드(`shared/SkyrimDiagHangObjectThreads.h`, 헬퍼와 분석기가 같은 규칙): 메인 스레드(SessionStart/하트비트), WCT에서 메인 스레드의 대기 체인에 있는 스레드(잠금 소유자 순서), WCT가 실행 중(Running)으로 본 스레드. 실행 중 스레드는 게임 실행 파일에서 시작한 스레드(BSJobs 잡 스레드 등) 먼저, 그다음 두 패스 모두 실행 중인 스레드 먼저. 최대 16개.
- 헬퍼: 크래시 덤프가 아니고, PSS 스냅샷과 전체 메모리 덤프가 아닌 프리징·수동 덤프에서, MiniDumpWriteDump가 스레드마다 넘기는 컨텍스트(`ThreadCallback`/`ThreadExCallback`, 덤프에 실리는 바로 그 레지스터)를 모아 두었다가 첫 `MemoryCallback`에서 선택 순서대로 위 판별 로직을 돌린다. 이때 스레드는 덤프 작성기가 정지시켜 둔 상태라 따로 정지시키지 않는다.
  - 게임이 계속 실행되므로 읽기 전에 `VirtualQueryEx`로 페이지를 확인한다: 커밋됐고 읽을 수 있는 페이지만, guard·no-access 페이지는 절대 안 됨(guard를 건드리면 스택이 더 자라지 못함), 실행 페이지는 모듈 이미지 안일 때만(훅이 .rdata를 RWX로 남겨도 RTTI는 읽음).
  - 상한: 전체 4MB, 스레드당 1MB(바이트 상한은 읽기 단위의 하드 캡), 시간 100ms. 헬퍼 로그에 캡처마다 한 줄: `Object memory: threads=<읽음>/<선택> ranges=<n> bytes=<n> time_us=<n>`(상한에 걸리면 `(budget reached)`).
  - 재시도(ERROR_PARTIAL_COPY) 때도 같은 범위를 다시 넘긴다.
- 분석기: 프리징·수동 덤프에서 같은 스레드에 더해 판정이 가리키는 스레드(잠금 소유자, 메인 스레드가 기다린 게임 스레드, `engine_spin` 스레드)의 덤프 컨텍스트로 `ReadDumpObjects`를 돌린다. 오브젝트가 나온 스레드만 남긴다.
- 판정 스레드: `engine_spin_nan`이면 그 스레드, 잠금 소유자 상태(`lock_owner_wait` 등)면 소유자, 아니면 메인 스레드가 기다린 게임 스레드. 그 스레드의 가장 관련도 높은 폼이 단서가 된다.
- 단서일 뿐이다: 판정, 신뢰도, 후보, 요약 문장은 바뀌지 않고, 오브젝트를 정의한 플러그인을 원인으로 지목하지 않는다. 서명 매칭에도 쓰지 않는다. 이름과 플러그인 파일 이름만 나오고 경로는 나오지 않는다.

### Output Contract (freeze)

- 요약 JSON `freeze_analysis.thread_objects[]`(오브젝트가 있을 때만): `tid`, `roles[]`(`main`/`wait_chain`/`lock_owner`/`blocked_thread`/`engine_spin`/`running`), `objects[]`(`dump_objects[]`와 같은 필드).
- 요약 JSON `freeze_analysis.verdict_thread_object`(판정 스레드에 폼이 있을 때만): 그 폼의 `dump_objects` 필드에 `tid`, `text`(`Character "Lydia" (FormID 0xFEAD081B, Skyrim.esm -> MyMod.esp)`; 플러그인이 없는 런타임 폼은 `(FormID 0xFF0036F8, base TESNPC 0x000A2C94 Skyrim.esm)`).
- 리포트 FreezeAnalysis 아래 스레드마다 `  thread_objects tid=<tid> roles=<roles>`와 오브젝트 최대 5줄(`DumpObjects:`와 같은 형식).
- 빠른 답: "확실하지 않은 이유"가 있을 때 그 뒤에 `멈춘 계산이 다루던 대상: <폼>.`(`engine_spin_nan`) 또는 `멈춘 스레드가 다루던 대상: <폼>.`(영어: `The stuck calculation was handling:` / `The stuck thread was handling:`).
- 근거 항목(낮음) "멈춘 스레드의 레지스터·스택이 가리킨 오브젝트(덤프에서 읽음)": 판정 스레드와 오브젝트 최대 5개.

### Limitations (freeze)

- 이 버전 이전의 프리징·수동 덤프에는 힙이 없어 아무것도 나오지 않는다(보관 중인 실사고 덤프 20건 재분석에서 출력 변화 없음).
- 스레드가 실행 중이면 오브젝트 메모리는 덤프 작성기가 스레드를 정지시킨 시점의 값이다. 같은 순간의 레지스터와 맞지만, 정지 직전에 바뀐 필드는 다를 수 있다.
- 덤프 크기는 최대 4MB 늘어난다(테스트에서 스레드 3\~5개에 1\~2KB, 실제 게임은 스레드당 보통 수십 KB로 예상).

## Verification

- `skydiag_rtti_decoder_tests`(Linux): 손으로 만든 주소 공간에서 Character 폼 판별, 숫자·잘못된 locator·이름이 아닌 값 거부, 계층·파일 일부가 없는 경우, 읽을 수 없는 메모리 바로 앞에서 끝나는 타입 이름, TESForm이 아닌 클래스, 이름 해독.
- `skydiag_crash_object_e2e_tests`: 레지스터가 Skyrim과 같은 레이아웃의 실제 C++ 오브젝트(MSVC RTTI)를 가리키는 크래시를 실제 헬퍼 덤프 경로로 쓰고, 실제 분석기로 타입, FormID, 플러그인 파일 두 개, RTTI 모듈, 근거 항목을 확인한다. 헬퍼가 메모리를 담지 않으면 실패한다.
- 이름과 베이스 오브젝트: `skydiag_rtti_decoder_tests`가 UTF-8 이름, 레퍼런스→베이스, `TESBoundObject`가 아닌 대상 거부, 읽을 수 없는 이름, 도중에 끊기거나 제어 문자가 섞인 이름, TESFullName 부분을 가리키는 포인터(완전 오브젝트를 먼저 찾음)를 확인하고, `skydiag_crash_object_e2e_tests`가 다중 상속으로 TESFullName이 0이 아닌 위치에 있는 실제 MSVC 클래스로 이름, 베이스 타입·FormID·플러그인, 근거 문장, 요약 JSON의 `name`·`base_object`를 덤프에서 다시 읽는다.
- 보관 중인 실사고 덤프 15건 재분석: 오브젝트 0개(메모리 없음), 다른 결과는 변화 없음.
- 프리징(2026-10-22 부록): `skydiag_hang_object_threads_tests`(Linux)가 스레드 선택 순서·상한·잘못된 WCT를, `skydiag_crash_object_e2e_tests`가 레지스터를 바꿔 둔 실제 스레드들의 실제 헬퍼 프리징 덤프(메인·잠금 소유자·실행 중 스레드는 읽고, 선택되지 않은 스레드와 guard·no-access·실행 페이지·숫자는 건너뜀, guard 플래그 유지, 바이트·시간 상한)와 실제 `ExecuteConfirmedHangCapture` 경로(헬퍼 로그 한 줄, 분석기 `thread_objects`)를, `skydiag_quick_answer_tests`가 빠른 답 단서·근거 항목·요약 JSON과 판정·후보 불변을 확인한다.

## References

- `shared/SkyrimDiagRtti.h`
- `helper/src/CrashObjectMemory.cpp`
- `helper/src/HangObjectMemory.cpp`
- `shared/SkyrimDiagHangObjectThreads.h`
- `dump_tool/src/DumpObjects.cpp`
- `dump_tool/src/AnalyzerInternalsStackwalkMemory.cpp`
