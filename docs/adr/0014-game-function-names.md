# ADR-0014: Game Function Names

## Status
Accepted

## Context
게임 EXE에는 PDB가 없어서 콜스택의 게임 프레임이 `SkyrimSE.exe+0xcc90cf`처럼 주소로만 나옵니다. 이전의 `address_db/skyrimse_functions.json`에는 1.5.97과 1.6.640의 손으로 적은 항목 14개만 있었습니다. 또 그 항목은 주소에서 0x100 안쪽에 있는 이름을 붙였는데, 표에 없는 함수가 그 범위에 있으면 엉뚱한 이름이 붙을 수 있었습니다. 1.6.1170 항목은 아예 없었습니다.

함수 이름 출처는 Codex(gpt-6.1-sol) 조사(2026-10-13)로 비교했습니다.
- CommonLibSSE-NG: alandtse 포크는 2026-08-20에 GPL-3.0-or-later로 바뀌었고, 그 직전 커밋 `e34246283a75fdd7009108476f0c646038f71f3c`까지는 MIT입니다. `RELOCATION_ID(se, ae)`가 들어 있는 함수와 상수의 이름은 약 1,000개입니다.
- meh321/AddressLibraryDatabase의 `skyrimae.rename`: 이름이 약 42,000개지만 저장소에 라이선스가 없어서, 재배포 허락이 확인되지 않습니다.
- Address Library(Nexus 32444): 권한 문구가 "You are allowed to use the assets in this file without permission or crediting me"입니다. ID와 오프셋 표에는 이름이 없습니다.

## Decision
1. 이름은 MIT 리비전의 CommonLibSSE-NG에서만 가져옵니다. 오프셋은 각 게임 버전의 Address Library 파일로 구합니다. zip에는 생성한 `오프셋 → 이름` 표만 넣고, Address Library 파일은 넣지 않습니다.
2. 표는 `scripts/generate_game_function_names.py`로 만들고, 손으로 고치지 않습니다.
   - 이름은 `RELOCATION_ID`가 들어 있는 함수 이름입니다. 함수 밖에 있으면 상수 이름을 씁니다. 예: `BSReadWriteLock::LockForRead`.
   - CommonLib이 한 ID를 두 이름으로 쓰면 그 ID는 뺍니다. 2026-10 기준 6개입니다.
   - 실제 1.6.1170 SkyrimSE.exe에서 .pdata의 주 항목(chained가 아닌 것) 시작 주소인 ID만 남깁니다. 같은 AE ID는 다른 AE 버전에서도 같은 함수라서, 1.6.640에도 같은 ID를 씁니다. 1.5.97에는 CommonLib이 짝지은 SE ID를 씁니다.
   - MIT 고지문은 JSON의 `name_license`에 함께 넣습니다.
3. 분석기는 프레임이 속한 .pdata 함수의 시작 주소를 구해, 그 주소와 정확히 같은 항목만 이름으로 씁니다.
   - .pdata는 스택 해제와 같은 출처를 씁니다. 일치하는 로컬 이미지가 우선이고, 없으면 덤프에 든 것을 씁니다.
   - chained와 indirect 항목은 주 항목까지 따라갑니다.
   - 호출자 프레임은 리턴 주소에서 1을 뺀 곳으로 찾습니다. 리턴 주소가 함수 바로 다음 바이트일 수 있기 때문입니다.
   - 주소가 가까운 이름을 대신 붙이지는 않습니다.
4. 표시 위치
   - 콜스택: `SkyrimSE.exe+0x2ea36b (TESObjectREFR::GetDisplayFullName+0x2b)`. 덤프에서 이미 심볼을 찾은 프레임은 그대로 둡니다.
   - 근거 항목 "게임 함수 식별": 예외 주소의 함수와 "원인은 대개 잘못된 데이터로 이 함수를 부른 모드"라는 안내.
   - 요약 JSON의 `resolved_functions`: 이름이 붙은 프레임의 모듈 오프셋과 이름.
5. 후보, 버킷, 이력은 바꾸지 않습니다. 게임 함수는 터진 위치일 뿐 원인이 아니기 때문입니다.
6. Crash Logger 로그에 적힌 PDB 함수 이름을 읽어 오는 것은 이번 범위에서 뺍니다. 그 출력 형식을 실제 로그로 확인할 수 없었습니다(사용자 환경에 Crash Logger용 게임 PDB가 없음).

## Consequences
- 표의 크기: 1.6.1170 737개, 1.6.640 733개, 1.5.97 735개.
- 크래시 테스트의 게임 EXE 내부 CTD(2026-10-12)에서는 예외 함수와 호출 함수 모두 이름이 붙습니다.
  - `BSReadWriteLock::LockForRead+0xf`
  - `TESObjectREFR::GetDisplayFullName+0x2b`, Crash Logger의 `19781+0x2B`와 같습니다.
- CommonLib 이름은 모드 제작자가 직접 부르는 엔진 함수 위주라서, 실사고 CTD의 깊은 엔진 프레임에는 대부분 이름이 없습니다. 2026-10-07 덤프에서는 게임 프레임 12개 중 0개에 이름이 붙었습니다.
- meh321의 허락을 받으면 생성기에 이름 출처를 하나 더 붙여 넓힐 수 있습니다. 표 형식과 분석기는 그대로 둡니다.

## Verification
- `skydiag_game_function_names_generator_tests`: 함수 이름과 상수 이름 추출, 생성자, 클래스 템플릿, 주석과 문자열 처리, RTTI·VTABLE 파일 제외, 두 이름을 가진 ID 제외, Address Library 압축 형식 해독, 저장소 표의 출처와 MIT 고지문.
- `skydiag_stackwalk_e2e_tests`: 합성 PE에서 주 항목, chained 조각, indirect 항목의 함수 시작 주소와 leaf 함수.
- `skydiag_analysis_engine_runtime_tests`, `skydiag_address_resolver_tests`: 1.6.1170의 `0xCC90C0`, 정확 일치만 허용, 세 버전 로드.
- 보관 덤프 재분석(크래시 9건): 위 결과.
