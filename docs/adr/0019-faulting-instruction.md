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
   - 판정: `null_pointer`(기준 레지스터가 0 또는 64KB 미만), `bad_index`(인덱스×배율이 64KB 이상, 또는 기준 레지스터 없이 인덱스로 고른 주소), `bad_pointer`(기준 레지스터가 메모리가 없는 주소), `noncanonical`, `misaligned`, `fixed_address`(레지스터 없는 절대·RIP 상대 주소), `stack`(push/pop, 인덱스 없는 rsp 기준; 인덱스가 크면 `bad_index`), `address_only`.
   - 명령어를 가져오다 난 예외(종류가 실행이거나, 예외 주소 자체를 읽다 남): `null_jump`(64KB 미만) / `bad_jump`. 스택 맨 위 반환 주소 바로 앞이 그 주소로 가는 call이면(`call rel32`는 대상, `call reg`는 레지스터, `call [mem]`은 덤프에서 읽은 슬롯 값으로 확인) 호출 위치(`모듈+오프셋`)와 그 명령어를 덧붙입니다.
   - 예외가 CrashLogger.dll/CrashLoggerSSE.dll 안이면 건너뜁니다(`IsCrashLoggerModuleName`, Crash Logger 스레드 판별과 같은 이름 목록). 다른 크래시를 기록하며 메모리를 조사하다 낸 2차 예외라는 기존 해석과 같습니다.
3. **출력 계약**
   - 근거 항목 "예외를 낸 명령어가 한 일(덤프에서 해독)" / "What the faulting instruction did (decoded from the dump)"을 "예외 파라미터 분석" 바로 뒤에 둡니다. 레지스터로 증명된 판정과 명령어 가져오기 예외는 신뢰도 높음, `address_only`는 중간입니다. 예: `SkyrimSE.exe+0x3ff101의 명령어(movups)가 [rax+0x30]에서 16바이트를 읽으려 했는데 rax가 0이었습니다. 이미 사라졌거나 만들어진 적 없는 오브젝트를 썼다는 뜻입니다(null 포인터).`
   - 빠른 답: 쉬운 말 한 문장을 "어떻게 튕겼나" / "How it crashed" 줄로 따로 둡니다(`quick_answer.how_it_crashed`, `null_pointer`, `bad_pointer`, `bad_index`, `noncanonical`, `null_jump`, `bad_jump`만). 확실하지 않은 이유가 없는 빠른 답(의심 모드가 없거나 가능성 높음)에도 나옵니다. 빠른 답 제목 줄, 해볼 일, 확실하지 않은 이유, 결론 문장, 후보와 신뢰도, 다음 조치, 시그니처, 버킷 키는 바꾸지 않습니다. 어느 모드가 원인인지가 아니라 어떻게 터졌는지만 말하기 때문입니다. 뷰어 카드와 요약 복사·공유 텍스트는 확실하지 않은 이유 다음에 이 줄을 보여 줍니다.
   - 리포트: `Module+Offset:` 다음 줄에 `FaultInstruction:` / `예외 명령어:` (`mov qword ptr [rax+0x10] (48 8B 58 10), read 0x10, rax=0x0 -> null_pointer`, 호출 위치가 있으면 덧붙임).
   - 요약 JSON `fault_instruction`(추가 필드, 판정이 있을 때만): `verdict`, `access`(`read`/`write`/`execute`), `address`, `mnemonic`, `operand`, `bytes`, `access_size`, `address_matches`, `text`(근거 문장과 같음). `address_matches`일 때 `effective_address`, 기준 레지스터가 있으면 `base_register`·`base_value`, 인덱스가 있으면 `index_register`·`index_value`·`scale`(없는 쪽은 키를 쓰지 않음). 호출 위치가 있으면 `call_site`, `call_instruction`. 주소와 값은 `0x` 대문자 16진 문자열입니다.
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

## Addendum (2026-10-26): 이동한 곳의 메모리 상태
- 2026-10-26 강제 종료 덤프(ADR-0022)의 `bad_jump`는 0x7FF7055F0000으로 갔고, 그 주소는 덤프의 메모리 정보 스트림(`MemoryInfoListStream`)에서 `MEM_FREE`였습니다(해제된 후킹 트램펄린). "잘못된(해제됐거나 망가진) 함수 포인터"보다 분명하게 말할 수 있습니다.
- `ProbeFaultInstruction`(분석기 쪽, Windows 헤더 사용)이 `bad_jump`의 대상 주소를 메모리 정보 스트림에서 찾아 `FaultInstructionInfo::target_state`에 넣습니다: `freed`(MEM_FREE), `reserved`(MEM_RESERVE), `not_executable`(커밋됐지만 실행 권한 없음, 또는 guard/noaccess), `executable`. 스트림이 없거나 주소를 덮지 않으면 비웁니다. 해독기(`FaultInstruction.cpp`)는 이 문자열만 읽으므로 Windows 헤더 없이 그대로입니다.
- `null_jump`은 대상이 처음 64KB(할당된 적 없는 영역)라 해제됐다고 하지 않으므로 상태를 넣지 않습니다.
- 문장
  - `freed`: 근거 "코드가 X 주소의 명령을 실행하려 했지만 그곳에는 할당된 메모리가 없었습니다. 해제된 메모리(예: 제거된 후킹 코드)로 이동했다는 뜻입니다." / "The code tried to run instructions at X, but no memory was allocated there: it jumped into memory that had been freed (for example a hook stub that was removed).", 빠른 답 "해제된 메모리(예: 제거된 후킹 코드)로 이동하다가 튕겼습니다." / "It crashed jumping into memory that had been freed (for example a hook stub that was removed)."
  - `not_executable`: 근거 "…그곳은 실행할 수 없는 데이터 메모리입니다. 데이터를 가리키는 잘못된 함수 포인터나 반환 주소로…" / "…which is memory holding data, not code (it is not executable)…", 빠른 답 "코드가 아닌 데이터 주소로 이동하다가 튕겼습니다(망가진 함수 포인터)." / "It crashed jumping to data instead of code (a broken function pointer)."
  - 그 밖(`reserved`, `executable`, 빈 값): 기존 문장.
  - 호출 위치 문장은 그대로 뒤에 붙습니다.
- 출력 계약: 요약 JSON `fault_instruction.target_state`(값이 있을 때만), 리포트 `FaultInstruction: execute 0x7FF7055F0000 (freed), from call qword ptr [rsp+0x20] -> bad_jump`.
- 검증: `skydiag_fault_instruction_tests`(문장·빠른 답·리포트 줄, 다른 상태는 기존 문장, null_jump은 freed라 하지 않음), `skydiag_quick_answer_tests`(요약 JSON `target_state`, 리포트 줄). 보관 덤프 21건 재분석에서 바뀐 것은 `SkyrimDiag_Crash_20261026_140735_958`(`target_state` = `freed`, 근거·빠른 답·리포트 줄) 하나이고, 나머지 덤프에는 `bad_jump`이 없어 그대로입니다.

## Addendum (2026-11-15): `null_jump`의 크래시 콜스택
- 2026-11-15의 실제 크래시(v0.2.76-rc2 로컬 빌드)는 `po3_LightPlacer.dll`의 `call qword ptr [rax+0x18]`이 0으로 간 `null_jump`였습니다. 분석기는 RIP가 0인 스레드를 걸러 내 콜스택을 하나도 만들지 못했고(`[Stackwalk] DbgHelp stackwalk failed, falling back to stack scan`), 빠른 답은 스택 스캔만으로 Light Placer를 "가능성 낮음, 메모리 근처에서 보였을 뿐"이라고 했습니다. 같은 크래시의 Crash Logger 로그는 스택 맨 위의 반환 주소에서 다시 시작해 Light Placer의 조명 생성 경로를 그대로 보여 줬습니다.
- 결정: 예외 스레드의 판정이 `null_jump`이고 컨텍스트의 RIP가 예외 주소이면, 스택 맨 위(RSP)의 값에서 RSP+8로 걷기 시작합니다(호출이 넣어 둔 반환 주소로, 그 함수의 `ret`이 했을 일. `ResumeAtCallerOfNullJump`). 그 값을 반환 주소로 보는 것은 셋이 모두 맞을 때뿐입니다.
  - 모듈 안의 코드 영역입니다.
  - 그 주소 바로 앞에서 끝나는 `call` 명령이 있습니다(`EndsWithCall`). 0으로 간 것이 `jmp`(꼬리 호출, 후킹 스텁)였다면 스택 맨 위에는 반환 주소가 아니라 그 자리에 있던 아무 값이 있어서, 모듈 안의 값이어도 프레임이 아닙니다. 명령 바이트는 덤프의 것을 먼저 읽고, 없으면 SkyrimSE.exe가 아닌 모듈만 로컬 파일에서 읽습니다(로컬 SkyrimSE.exe는 코드가 암호화돼 있음).
  - 그 함수에 unwind 정보가 있습니다. 다른 함수를 부르는 x64 함수는 프롤로그가 있으므로 unwind 정보가 있습니다.
  - 하나라도 아니면 지금처럼 걷지 않습니다(콜스택 없이 스택 스캔).
  - 예외 주소(0)는 프레임 0으로 남깁니다. 그래서 프레임 1부터가 반환 주소라는 규칙(게임 함수 이름 붙이기 등)이 그대로이고, 리포트와 뷰어의 콜스택 첫 줄은 `0x0`입니다.
  - `bad_jump`(모듈 밖의 0이 아닌 주소)는 그대로입니다. 그 주소는 0이 아니어서 DbgHelp가 첫 프레임으로 받습니다. 실제로 반환 주소에서 이어지는지는 따로 확인하지 않았습니다.
- 결과: 그런 덤프는 콜스택과 콜스택 후보가 생기고 버킷 키가 바뀝니다. 업데이트 전에 기록된 같은 크래시와는 다른 버킷이 되어, 리포트의 반복 크래시 이력이 이어지지 않습니다. 다음 릴리즈 노트에 적어야 합니다.
- 한계: 후킹 모드가 모듈 밖에 만든 코드(트램펄린) 안에서 null을 부르면 반환 주소가 모듈 밖이라 다시 시작하지 않고, 콜스택은 지금처럼 비어 있습니다. 그런 코드에는 unwind 정보가 없어서 다시 시작해도 그 아래로 이어 가지 못합니다. Crash Logger는 이 경우에도 그 아래를 스택 스캔으로 보여 줍니다.
- 검증:
  - `skydiag_fault_instruction_tests`(Linux 포함): 반환 주소에서 RSP+8로 다시 시작하는지, 그 앞이 `call`이 아니거나(`jmp`, 다른 명령, 읽지 못한 바이트) unwind 정보가 없거나 코드 주소가 아니거나 읽을 수 없거나 RSP가 없거나 끝이면 다시 시작하지 않는지. `EndsWithCall`이 `call [mem]`, `call [rip+disp]`, `call rel32`, `call reg`를 받고 `jmp [mem]`과 다른 명령을 받지 않는지.
  - `skydiag_crash_object_e2e_tests`: 시스템 호출에서 멈춘 스레드의 컨텍스트를 RIP 0으로 바꿔 실제 덤프를 쓰고 분석하면, 콜스택이 `0x0`, `KERNELBASE.dll`(호출한 함수), 테스트 실행 파일의 프레임으로 이어지는지.
  - 되돌려 보기: 다시 시작하지 않기, 프레임 0을 남기지 않기, RSP를 옮기지 않기, 코드 주소 확인 없애기, `call` 확인 없애기, unwind 정보 확인 없애기를 하나씩 넣어 각각 위 테스트가 실패하는 것을 확인했습니다.
  - 실행 테스트가 없는 것: SkyrimSE.exe 안의 반환 주소는 덤프의 명령 바이트로만 `call`을 확인하는 것. 덤프의 바이트를 쓰지 않게 바꿔도 위 테스트는 통과합니다(그 테스트의 호출자는 KERNELBASE.dll이라 로컬 파일로 확인됨).
  - 실제 덤프 재분석(`scripts/compare_analyses.py`, v0.2.76-rc2 분석기 대비, 덤프 44개의 한·영 88건): 바뀐 것은 위 크래시 2건뿐입니다. 콜스택이 Crash Logger의 것과 같은 순서(`TraverseScenegraphObjects` ← `LightData::GenLight` ← `LightManager::AttachLight` ← … ← `SkyrimSE.exe` ID 68445)로 나오고, 빠른 답이 Light Placer를 "가능성 높음"으로, 후보 충돌 안내가 "DLL 후보부터"로 바뀌었습니다. 나머지 86건은 같습니다.
- 남은 것: 이 크래시에서도 호출 위치(`call qword ptr [rax+0x18]`) 문장은 나오지 않았습니다. 호출이 읽은 메모리(`[rax+0x18]`, 힙)가 덤프에 없어 그 호출이 0으로 갔는지 확인할 수 없기 때문입니다(`FindCallTo`).

## Verification
- `skydiag_fault_instruction_tests`(Linux 포함): 위 형식마다 합성 바이트열의 해독(크기, 읽기/쓰기, 피연산자 문자열, SIB·r12/r13·REX.X·disp32·RIP 상대 주소·즉값 길이), 거부 형식(VEX/EVEX, 67, GS, 0F 38, MMX, popcnt, rep, 잘린 바이트), 판정마다의 교차 검증과 영어·한국어 문장(레지스터가 0인 경우 전체 문장 일치), `address_only`에서 레지스터를 말하지 않음, 호출 위치 찾기(`call [rax+0x18]`, `call [rsp+8]`의 rsp 보정, `call reg`, `call rel32`, 긴 호출 명령어의 꼬리가 짧은 호출로 보이는 경우)
- `skydiag_quick_answer_tests`: 근거 항목의 위치·신뢰도·문장, 빠른 답의 덧붙인 문장과 나머지 불변, 요약 JSON `fault_instruction`, 리포트 줄, 레지스터 없는 경우
- 보관 덤프 19건을 변경 전후로 나란히 재분석
