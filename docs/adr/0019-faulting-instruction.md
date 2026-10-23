# ADR-0019: 예외를 낸 명령어 해독

## Status
Accepted

## Context
- 접근 위반(`0xC0000005`) 크래시에서 리포트가 말하는 것은 "접근 위반: 읽기 주소=0x10"이 전부였습니다. 그 주소를 어떤 명령어가 어떤 레지스터로 만들었는지(오브젝트 포인터가 0이었는지, 인덱스가 컸는지)는 Crash Logger 로그의 명령어 문자열을 따로 봐야 알 수 있었고, 로그가 없거나 다른 크래시의 로그와 짝지어지면 볼 수 없었습니다.
- 보관 중인 실사고 덤프 19건 가운데 접근 위반은 5건이고, 모두 덤프에 예외 주소의 코드 바이트와 예외 시점의 레지스터가 있습니다. 그중 4건은 레지스터로 다시 계산한 주소가 예외 주소와 정확히 같습니다(`mov rbx,[rax+0x10]`·`mov rdi,[rax+0x8]`·`movups xmm0,[rax+0x30]`에서 rax=0, `cmp [r8+r9*8],r10`에서 r9가 큰 인덱스). 나머지 1건은 CrashLogger.dll 내부의 2차 예외입니다.
- 명령어 형식은 모두 VEX가 아닌 일반 x64 명령어였습니다.

## Decision
1. **해독기**(`FaultInstruction.h/.cpp`, Windows 헤더 없음, Linux 테스트 포함) — 게임과 플러그인 코드가 메모리에서 터지는 형식만 손으로 해독합니다.
   - 정수: `mov`(8A/8B/88/89/C6/C7), `movzx`, `movsx`, `movsxd`, `cmp`·`test`·`add`·`or`·`adc`·`sbb`·`and`·`sub`·`xor`(00–3B, 80/81/83, 84/85, F6/F7), `inc`/`dec`, `not`/`neg`/`mul`/`imul`/`div`/`idiv`, 시프트, `xchg`, `xadd`, `cmpxchg`, `cmovcc`, `setcc`, `bt*`(0F BA)
   - SSE: `movss`/`movsd`/`movups`/`movupd`, `movaps`/`movapd`, `movdqa`/`movdqu`, `movd`/`movq`, `movlps`/`movhps`류, `comiss`/`ucomiss`/`comisd`/`ucomisd`, 스칼라·패킹 산술(`add`/`mul`/`sub`/`div`/`min`/`max`/`sqrt`/`cmp`), `and`/`or`/`xor`ps/pd, 변환(`cvtsi2ss` 등), `shufps`/`unpck*`
   - 분기와 스택: `call [mem]`, `jmp [mem]`, `push [mem]`, `push`/`pop` 레지스터, 그리고 호출 위치 확인용 `call rel32`·`call reg`
   - 레거시 접두사(66, F2, F3, lock, 무시되는 세그먼트), REX, ModRM, SIB, disp8/disp32, RIP 상대 주소를 처리합니다. `lea`, `nop`, `prefetch`는 메모리에 접근하지 않는 명령어로 해독합니다.
   - VEX/EVEX, 0F 38/0F 3A, 67(32비트 주소), FS/GS 세그먼트, `rep` 문자열 명령어, MMX는 해독하지 않습니다. 모르는 명령어면 아무것도 말하지 않습니다.
   - Zydis 같은 라이브러리는 쓰지 않았습니다. 필요한 것은 메모리 피연산자 하나와 그 크기·읽기/쓰기뿐이고, 의존성을 더하면 vcpkg, CI 캐시, Linux 빌드가 함께 바뀝니다.
2. **교차 검증**(`ExplainFaultInstruction`, `ProbeFaultInstruction`)
   - 코드 바이트는 덤프 자신의 메모리에서만 읽습니다. 디스크의 SkyrimSE.exe는 코드가 암호화돼 있어, 다른 바이트로 짐작하느니 말하지 않는 편이 낫습니다.
   - 예외 스트림의 컨텍스트 RIP가 예외 주소와 같을 때만 레지스터를 씁니다.
   - 명령어의 읽기/쓰기가 예외의 종류와 맞고, 레지스터로 계산한 주소가 예외 주소와 같거나 그 접근 범위 안(페이지 경계를 넘는 읽기)일 때만 레지스터 값을 말합니다. 그렇지 않으면 `address_only`: 읽기/쓰기와 주소만 말하고 레지스터와 명령어는 문장에 넣지 않습니다.
   - 일반 보호 예외(예외 주소 `0xFFFFFFFFFFFFFFFF`): 계산한 주소가 canonical이 아니면 `noncanonical`, 16바이트 정렬이 필요한 SSE에서 정렬이 안 맞으면 `misaligned`, 그 밖에는 아무것도 말하지 않습니다.
   - 판정: `null_pointer`(기준 레지스터가 0 또는 64KB 미만), `bad_index`(기준 + 인덱스×배율에서 인덱스 몫이 64KB 이상), `bad_pointer`(기준 레지스터가 메모리가 없는 주소), `noncanonical`, `misaligned`, `fixed_address`(레지스터 없는 절대·RIP 상대 주소), `stack`(push/pop, rsp 기준), `address_only`.
   - 명령어를 가져오다 난 예외(종류가 실행이거나, 예외 주소 자체를 읽다 남): `null_jump`(64KB 미만) / `bad_jump`. 스택 맨 위 반환 주소 바로 앞이 그 주소로 가는 call이면(`call rel32`는 대상, `call reg`는 레지스터, `call [mem]`은 덤프에서 읽은 슬롯 값으로 확인) 호출 위치(`모듈+오프셋`)와 그 명령어를 덧붙입니다.
   - 예외가 CrashLogger.dll/CrashLoggerSSE.dll 안이면 건너뜁니다. 다른 크래시를 기록하며 메모리를 조사하다 낸 2차 예외라는 기존 해석과 같습니다.
3. **출력 계약**
   - 근거 항목 "예외를 낸 명령어가 한 일(덤프에서 해독)" / "What the faulting instruction did (decoded from the dump)"을 "예외 파라미터 분석" 바로 뒤에 둡니다. 레지스터로 증명된 판정과 명령어 가져오기 예외는 신뢰도 높음, `address_only`는 중간입니다. 예: `SkyrimSE.exe+0x3ff101의 명령어(movups)가 [rax+0x30]에서 16바이트를 읽으려 했는데 rax가 0이었습니다. 이미 사라졌거나 만들어진 적 없는 오브젝트를 썼다는 뜻입니다(null 포인터).`
   - 빠른 답: "확실하지 않은 이유" 뒤에 쉬운 말 한 문장을 덧붙입니다(`null_pointer`, `bad_pointer`, `bad_index`, `noncanonical`, `null_jump`, `bad_jump`만). 빠른 답 제목 줄, 해볼 일, 결론 문장, 후보와 신뢰도, 다음 조치, 시그니처, 버킷 키는 바꾸지 않습니다. 어느 모드가 원인인지가 아니라 어떻게 터졌는지만 말하기 때문입니다. 확실하지 않은 이유가 없는 빠른 답(가능성 높음)에는 넣지 않습니다.
   - 리포트: `Module+Offset:` 다음 줄에 `FaultInstruction:` / `예외 명령어:` (`mov qword ptr [rax+0x10] (48 8B 58 10), read 0x10, rax=0x0 -> null_pointer`, 호출 위치가 있으면 덧붙임).
   - 요약 JSON `fault_instruction`(추가 필드, 판정이 있을 때만): `verdict`, `access`(`read`/`write`/`execute`), `address`, `mnemonic`, `operand`, `bytes`, `access_size`, `address_matches`, `text`(근거 문장과 같음). `address_matches`일 때 `effective_address`, `base_register`, `base_value`, `index_register`, `index_value`, `scale`. 호출 위치가 있으면 `call_site`, `call_instruction`. 주소와 값은 `0x` 대문자 16진 문자열입니다.
   - 메모리 내용은 명령어 자신의 바이트(최대 15바이트) 말고는 출력하지 않습니다. 경로는 없습니다.
   - 정상 종료로 격리된 덤프에서는 비웁니다.

## Consequences
- 보관 덤프 재분석(한국어·영어):
  - `SkyrimDiag_Crash_20260918_171205_079`: `SkyrimUpscaler.dll+0x156144`, `mov [rax+0x10]`, rax=0 → `null_pointer`
  - `SkyrimDiag_Crash_20260922_154912_604`: `CalamityAffixes.dll+0x15cc1d`, `mov [rax+0x8]`, rax=0 → `null_pointer`
  - `SkyrimDiag_Crash_20261005_143810_399`: `SmoothCam.dll+0x15fcc`, `cmp [r8+r9*8]`, r9=0x689AC → `bad_index`
  - `SkyrimDiag_Crash_20261007_123742_440`: `SkyrimSE.exe+0x3ff101`, `movups [rax+0x30]`, rax=0 → `null_pointer`
  - `SkyrimDiag_Crash_20261005_122858_100`(CrashLogger.dll 내부 2차 예외), 접근 위반이 아닌 크래시 3건, 프리징·수동 덤프 11건은 출력 변화 없음(분석 시각이 들어가는 반복 이력 줄 제외)
  - 네 덤프 모두 Crash Logger 로그가 적은 명령어(`mov rbx, [rax+0x10]`, `movups xmm0, [rax+0x30]` 등)와 같습니다.
- 지원하지 않는 형식(AVX로 컴파일된 플러그인, `rep movs`를 쓰는 memcpy)에서 터지면 아무것도 말하지 않습니다.
- "rax가 0"은 어떤 오브젝트였는지, 어느 모드가 그 값을 만들었는지 말하지 않습니다. 오브젝트는 ADR-0010의 덤프 오브젝트와 Crash Logger 참조가 보여 줍니다.
- 예외 주소가 모듈 밖인 실행 예외는 덤프에 반환 주소가 남아 있을 때만 호출 위치가 나옵니다(꼬리 호출 `jmp`는 남기지 않음).

## Verification
- `skydiag_fault_instruction_tests`(Linux 포함): 위 형식마다 합성 바이트열의 해독(크기, 읽기/쓰기, 피연산자 문자열, SIB·r12/r13·REX.X·disp32·RIP 상대 주소·즉값 길이), 거부 형식(VEX/EVEX, 67, GS, 0F 38, MMX, popcnt, rep, 잘린 바이트), 판정마다의 교차 검증과 영어·한국어 문장(레지스터가 0인 경우 전체 문장 일치), `address_only`에서 레지스터를 말하지 않음, 호출 위치 찾기(`call [rax+0x18]`, `call [rsp+8]`의 rsp 보정, `call reg`, `call rel32`)
- `skydiag_quick_answer_tests`: 근거 항목의 위치·신뢰도·문장, 빠른 답의 덧붙인 문장과 나머지 불변, 요약 JSON `fault_instruction`, 리포트 줄, 레지스터 없는 경우
- 보관 덤프 19건을 변경 전후로 나란히 재분석
