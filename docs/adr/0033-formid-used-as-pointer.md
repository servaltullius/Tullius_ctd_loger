# ADR-0033: 포인터 자리에 든 FormID는 게임의 FormID 표로 찾아 이름을 붙인다

## Status
Accepted (2026-11-17). 구현했고 자동 테스트와 실게임으로 확인했습니다(Verification).

## Context
- 2026-11-15와 11-16에 같은 세이브(Save96)를 불러오다 같은 크래시가 두 번 났습니다.
  - 예외 위치: `OSLAroused.dll+0x5a5a0`, `cmp byte ptr [rdx+0x1a], 0x1a`. OSLAroused가 직접 만든 작업 스레드였고, 불러오기 11~13초째였습니다.
  - 두 번 모두 `rdx=0x12EB6`이었습니다. 무작위로 망가진 포인터였다면 값이 매번 달랐을 것입니다.
- `0x00012EB6`은 Skyrim.esm의 `ArmorIronShield`(철 방패)의 FormID입니다(houseCARL로 확인). `[주소+0x1A]`는 TESForm의 폼 종류 자리이고, 비교값 0x1A는 방어구입니다.
  - 레지스터에는 OSLAroused의 `GetWornArmor` 람다와 NPC 아티스(0x1A6D6)가 있었습니다. 아티스의 인벤토리를 읽다가 폼 포인터 대신 FormID를 쓴 것으로 보입니다.
  - 불러오는 중이라 인벤토리 항목에 아직 FormID가 들어 있었을 수 있다는 것은 가설입니다.
- 분석기는 "이미 해제된 메모리나 망가진 포인터"라고만 말했습니다.
  - 분석기가 읽는 것은 포인터가 가리키는 오브젝트뿐입니다(ADR-0010). FormID로 폼을 찾는 기능은 없었습니다.
  - 게임의 FormID 표(`TESForm::allForms`, 힙의 해시 표)는 덤프에 없습니다. 이번 덤프에는 그 표를 가리키는 exe의 전역 변수도 없었습니다.
- 값이 어떤 폼의 FormID와 같다는 것만으로는 근거가 약합니다. Skyrim.esm의 FormID는 0x10000~0x10FFFF 구간에 빽빽해서, 작은 정수가 우연히 어떤 폼과 맞을 수 있습니다.

## Decision
1. **Helper가 크래시 순간 레지스터 값을 게임의 FormID 표에서 찾습니다.**
   - 대상: 크래시 레지스터 15개(rsp 제외, ADR-0010의 루트와 같은 순서) 중 값이 0x10000 이상 0xFFFFFFFF 이하인 것(`IsFormIdCandidate`). 32비트보다 넓은 값은 FormID가 아닙니다. 64KB 미만은 일부러 뺍니다(Consequences의 마지막 항목).
   - 표의 위치: 게임 exe 베이스 + Address Library에서 찾은 `TESForm::allForms`의 오프셋. ID는 SE 1.5.x가 514351, AE 1.6.x가 400507입니다(CommonLibSSE-NG의 `RELOCATION_ID(514351, 400507)`). 어느 ID를 쓸지는 Address Library 파일 형식으로 고릅니다(1: SE, 2: AE). 1.7.x(형식 5)도 AE ID를 그대로 쓴다고 보고 400507을 쓰지만, 확인하지 않았습니다. 다르면 아래 검증에서 걸러져 아무것도 붙지 않습니다. 파일은 ADR-0027에서 이미 읽어 두는 것을 그대로 씁니다. 크래시 때 파싱하느라 붙잡힌 스레드가 늦게 풀리지 않도록, 게임에 붙을 때 오프셋까지 구해 두고 `SkyrimDiagHelper.log`에 남깁니다(`FormID lookup for crash dumps: TESForm::allForms at the game executable+0x20FBB88.`, 없으면 `off`).
   - 찾는 법: CommonLibSSE-NG의 `BSTHashMap` 배치를 따릅니다(`shared/SkyrimDiagFormMap.h`).
     - 표 머리 0x30바이트: 칸 수(0x0C, 2의 거듭제곱), 빈 칸 수(0x10), 체인 끝 표시(0x18), 엔트리 배열(0x28).
     - 엔트리 0x18바이트: FormID, 폼 포인터, 다음 엔트리. 다음 엔트리가 0이면 빈 칸입니다.
     - 해시는 CRC-32(0xEDB88320)를 0에서 시작해 마지막 반전 없이 FormID 4바이트에 돌린 값이고, 시작 칸은 해시 & (칸 수 - 1)입니다.
   - 검증: 칸 수가 2의 거듭제곱이고 2^26 이하일 것, 빈 칸 수가 칸 수보다 작을 것, 체인이 엔트리 배열 안의 엔트리 경계만 가리킬 것, 체인 길이 64 이하일 것, 찾은 폼의 `formID`(0x14)가 찾던 값과 같을 것. 하나라도 어긋나면 찾지 못한 것으로 둡니다.
   - 찾으면 읽은 바이트(전역 변수 8바이트, 표 머리, 거쳐 간 엔트리)와 그 폼 자체(ADR-0010의 오브젝트 읽기: RTTI, FormID, 이름, 플러그인)를 덤프에 넣습니다. 찾지 못하면 그 조회로 읽은 것은 넣지 않습니다.
   - 찾은 폼은 레지스터·스택이 가리키는 오브젝트보다 먼저 읽습니다. 분석기는 폼 오브젝트까지 덤프에 있어야 이름을 붙일 수 있는데, 크래시 오브젝트 메모리 상한(2MB)이 스택 쪽에서 먼저 차면 뒤에 붙인 폼은 읽히지 않기 때문입니다.
   - 크래시 덤프에서만 합니다. 프리징·수동 캡처의 오브젝트 수집에는 넣지 않습니다. 이 정보를 쓰는 곳이 접근 위반의 예외 명령어 분석뿐이기 때문입니다.
   - 다른 스레드는 그동안 계속 돌기 때문에, 표가 바뀌는 중에 읽을 수 있습니다. 위 검증으로 걸러지고, 걸리면 아무것도 붙지 않을 뿐입니다. 락은 잡지 않습니다(밖에서 읽기만 함).
2. **분석기는 같은 조회를 덤프에 대고 다시 합니다.**
   - 조건: 예외 명령어 판정이 `bad_pointer`이고(ADR-0019), 기준 레지스터로 주소가 맞아떨어지며, 그 값이 위 범위일 때.
   - 덤프의 Address Library 스트림과 게임 exe 모듈의 베이스로 같은 전역 변수 주소를 구하고, 덤프의 메모리만으로 `FindForm`을 돌립니다. 찾은 폼은 덤프 오브젝트와 같은 방식으로 읽어 이름을 붙입니다(`DescribeDumpObjectTarget`).
   - Helper가 넣은 바이트가 없으면(이 기능 이전의 덤프, 또는 Helper가 찾지 못함) 아무것도 붙지 않습니다.
3. **보여 주는 곳.**
   - 근거 "예외를 낸 명령어가 한 일": "오브젝트가 이미 해제됐거나 포인터가 망가졌을 가능성이 큽니다" 대신 "이 값은 이 게임에 있는 폼 TESObjectARMO "철 방패" (FormID 0x00012EB6, Skyrim.esm …)의 FormID와 같습니다. 폼을 가리키는 포인터 자리에 FormID가 들어갔을 수 있습니다."
   - 빠른 답의 "어떻게 튕겼나": "폼의 번호(FormID)를 포인터처럼 쓰다가 튕긴 것으로 보입니다." ADR-0019의 이 줄은 어떻게 터졌는지만 말한다는 원칙 그대로입니다. 어느 모드가 원인인지, 후보와 신뢰도, 버킷 키는 바꾸지 않습니다.
   - 리포트의 예외 명령어 줄 끝에 `(rdx 값은 이 폼의 FormID: …)`, 요약 JSON의 `fault_instruction.base_value_form`.
   - 문장은 "같습니다"와 "들어갔을 수 있습니다"로 씁니다. 우연히 맞을 수 있다는 Context의 한계 때문입니다. 다만 그 값이 포인터 자리(명령어의 기준 레지스터)에 있었고 그 자리에 메모리가 없었다는 두 조건이 이미 붙어 있어서, 아무 레지스터나 맞춰 보는 것보다는 훨씬 좁습니다.
4. **Address Library 파서를 공유 헤더로 옮겼습니다.** Helper도 ID로 오프셋을 찾아야 해서, `dump_tool/src/AddressLibrary.cpp`를 `shared/SkyrimDiagAddressLibrary.h`로 옮기고 `OffsetOf(id)`를 더했습니다. 분석기 쪽 이름(`skydiag::dump_tool::AddressLibrary`)은 그대로 씁니다.
5. **크래시 테스터에 Ctrl+Shift+Home을 더했습니다.** CrashTester.dll이 FormID `0x00012EB6`을 폼 포인터로 써서 폼 종류(`[rax+0x1A]`)를 읽습니다. 무인 실행은 `AutoScenario=14`입니다.

## Consequences
- FormID를 포인터로 쓴 크래시에서, 어느 폼의 FormID였는지가 리포트에 나옵니다. 이번 OSLAroused 크래시라면 "철 방패"가 나왔을 것이고, 인벤토리를 읽다 난 크래시라는 것을 바로 알 수 있었을 것입니다.
- 이 기능을 넣은 Helper로 캡처한 덤프에서만 나옵니다. 그 전 덤프를 다시 분석해도 바뀌지 않습니다(표가 덤프에 없음).
- 크래시 덤프가 조회마다 수백 바이트 커집니다(표 머리 0x30, 엔트리 0x18씩, 폼 오브젝트 창 0x100). 크래시 오브젝트 메모리 상한(2MB) 안입니다.
- 게임의 내부 배치(전역 변수 ID, 해시 표 배치, 해시 함수)에 기댑니다. 게임 업데이트로 바뀌면 검증에서 걸러져 아무것도 붙지 않습니다. 틀린 폼이 붙지는 않습니다(찾은 폼의 `formID`를 다시 확인함).
- VR은 다루지 않습니다(Address Library 형식이 다르고, 이 도구가 지원하지 않음).
- 0x10000 미만의 FormID(플레이어 0x14, 금화 0xF 같은 Skyrim.esm 앞쪽 폼)를 포인터로 쓴 크래시는 지금처럼 `null_pointer`("이미 사라졌거나 만들어진 적 없는 오브젝트")로 나옵니다. 그 구간의 레지스터 값은 대개 개수·인덱스·오프셋 같은 작은 수이고, Skyrim.esm의 FormID가 그 구간에 빽빽해서 맞아떨어져도 근거가 되지 않기 때문입니다. 알려진 한계입니다.

## Verification
- `skydiag_form_map_tests`: 해시가 zlib CRC-32에서 구한 값, CommonLib 표의 첫·마지막 값과 같습니다. 게임처럼 넣은 가짜 표에서 자기 칸의 폼, 체인 뒤의 폼, 빈 칸, 체인 끝, 잘못된 칸 수, 없는 전역 변수, 배열 밖·엔트리 중간을 가리키는 링크, 순환하는 체인, `formID`가 다른 폼을 확인합니다.
- `skydiag_crash_object_e2e_tests` (`TestFormIdRegisterIsLookedUp`): 이 프로세스 안에 만든 표와 `TESObjectARMO`로 Helper의 실제 수집 함수를 돌립니다.
  - 찾은 조회의 바이트와 폼이 덤프용 범위에 남는지 확인합니다.
  - 그 범위만으로 같은 조회를 돌려 같은 폼과 이름("Iron Shield"), 플러그인(Skyrim.esm)을 읽는지 확인합니다.
  - 찾지 못한 조회는 아무것도 남기지 않는지, 표 주소가 없으면 조회하지 않는지도 확인합니다.
  - 그 범위만 담은 이 프로세스의 실제 미니덤프를 쓰고, 분석기의 `ResolveFormIdBaseValue`를 그대로 돌립니다. 덤프의 Address Library(형식 2, ID 400507)와 게임 모듈로 전역 변수 주소를 구해 `TESObjectARMO "Iron Shield" (FormID 0x00012EB6, Skyrim.esm)`을 붙이는지 확인합니다. 게임 모듈은 이 프로세스에 SkyrimSE.exe가 없어 전역 변수 둘레의 범위로 지어냈습니다. Helper가 찾지 못한 FormID, `null_pointer` 판정, FormID보다 넓은 값, SE 형식 파일(ID 514351을 찾음), 모듈 밖 오프셋, Address Library 없음에서는 아무것도 붙지 않는지도 확인합니다.
- `skydiag_fault_instruction_tests` (`TestFormIdAsPointer`): 실제 명령어 바이트(80 7A 1A 1A, rdx=0x12EB6)로 판정과 한국어·영어 문장, 빠른 답 문장을 확인합니다.
- `skydiag_address_library_tests`: `OffsetOf`를 확인합니다.
- 크래시 테스터: 빌드한 `CrashTester.dll`의 `ReadFormTypeThroughFormId`가 `mov rax, qword ptr [rsp+80h]`(값 0x12EB6) 다음 `movzx ebx, byte ptr [rax+1Ah]`로 빌드된 것을 디스어셈블리로 확인했습니다(2026-11-17). 실게임에서는 rax=0x12EB6의 `bad_pointer`가 나와야 합니다.
- 실제 게임 데이터:
  - 사용자 환경 1.6.1170의 Address Library에서 ID 400507은 오프셋 0x20FBB88입니다.
  - 2026-11-16 덤프에는 그 전역 변수가 없어서, 덤프로 값을 확인하지는 못했습니다.
- **실게임 확인 (2026-11-17 02:47, 1.6.1170, Crash Logger 1.27 켬, MO2 모드 약 4,380개, PR #310 브랜치 로컬 빌드).** 크래시 테스터 Ctrl+Shift+Home.
  - Helper 로그(게임에 붙을 때): `FormID lookup for crash dumps: TESForm::allForms at the game executable+0x20FBB88.`
  - 리포트의 예외 명령어 줄: `movzx byte ptr [rax+0x1a] (0F B6 58 1A), read 0x12ED0, rax=0x12EB6 -> bad_pointer (rax 값은 이 폼의 FormID: TESObjectARMO "[U] 견고한 철 방패" (FormID 0x00012EB6, Skyrim.esm -> MUNG_UBE Armor Patch 2.esp))`.
  - 근거 문장은 "이 값은 이 게임에 있는 폼 …의 FormID와 같습니다. 폼을 가리키는 포인터 자리에 FormID가 들어갔을 수 있습니다."로 끝나고, 빠른 답의 "어떻게 튕겼나"는 "폼의 번호(FormID)를 포인터처럼 쓰다가 튕긴 것으로 보입니다."였습니다.
  - 플러그인 경로(Skyrim.esm → MUNG_UBE Armor Patch 2.esp)는 houseCARL로 본 그 레코드의 최종 적용 플러그인과 같습니다. 이름은 한국어판 게임의 이름입니다.
  - 이로써 Helper의 전역 변수 주소 계산, 실제 게임의 해시 표 배치와 해시 함수, 분석기가 실제 SkyrimSE.exe 모듈과 실제 Address Library로 같은 주소를 구하는 부분이 맞는 것을 확인했습니다.
  - 같은 날 End 시나리오도 이 빌드로 돌려, 결과(`null_jump`, `call rax`, 콜스택)가 이전과 같은 것을 확인했습니다.
- **여전히 미확인.**
  - 찾은 폼을 먼저 읽는 순서(Decision 1)는 자동 테스트도 실게임도 확인하지 않았습니다. 순서가 결과를 바꾸려면 크래시 오브젝트 메모리 상한이 먼저 차야 하는데, 그런 상황을 만들지 않았습니다.
  - SE 1.5.97(ID 514351)과 1.7.x 게임에서는 돌려 보지 않았습니다.
