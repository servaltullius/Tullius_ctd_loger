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
- 오브젝트 이름(TESFullName), 에디터 ID, 레퍼런스의 베이스 오브젝트, NiAVObject 이름은 아직 읽지 않는다. 클래스마다 오프셋이 다르다.
- `TESForm` 판별은 RTTI 이름에 기대므로, 게임이 RTTI를 지운 빌드라면 타입만 없고 아무것도 나오지 않는다.
- 덤프 크기는 오브젝트 수에 따라 최대 2MB 늘어난다(보통 수십 KB).

## Verification

- `skydiag_rtti_decoder_tests`(Linux): 손으로 만든 주소 공간에서 Character 폼 판별, 숫자·잘못된 locator·이름이 아닌 값 거부, 계층·파일 일부가 없는 경우, 읽을 수 없는 메모리 바로 앞에서 끝나는 타입 이름, TESForm이 아닌 클래스, 이름 해독.
- `skydiag_crash_object_e2e_tests`: 레지스터가 Skyrim과 같은 레이아웃의 실제 C++ 오브젝트(MSVC RTTI)를 가리키는 크래시를 실제 헬퍼 덤프 경로로 쓰고, 실제 분석기로 타입, FormID, 플러그인 파일 두 개, RTTI 모듈, 근거 항목을 확인한다. 헬퍼가 메모리를 담지 않으면 실패한다.
- 보관 중인 실사고 덤프 15건 재분석: 오브젝트 0개(메모리 없음), 다른 결과는 변화 없음.

## References

- `shared/SkyrimDiagRtti.h`
- `helper/src/CrashObjectMemory.cpp`
- `dump_tool/src/DumpObjects.cpp`
- `dump_tool/src/AnalyzerInternalsStackwalkMemory.cpp`
