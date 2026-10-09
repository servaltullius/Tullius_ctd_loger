# ADR-0008: SKSE Refused Plugins From skse64.log

## Status
Accepted

## Context
SKSE 플러그인 DLL이 게임 버전과 맞지 않는 경우를 보고서에 알리고 싶었습니다. 처음에는
houseCARL처럼 각 DLL의 `SKSEPlugin_Version` export를 직접 읽어 판정하는 방법을
검토했습니다.

조사 결과는 다음과 같습니다.

- SKSE 로더는 로드 전에 같은 검사(런타임 목록, Address Library 독립 플래그, 1.6.629
  구조체 규칙, 필요 SKSE 버전 등)를 이미 하고, 맞지 않는 DLL은 로드하지 않습니다.
  거부 사유는 정해진 문구로 skse64.log에 남습니다.
- 거부된 DLL은 게임 안에서 실행되지 않으므로 그 실행의 CTD나 프리징 원인이 될 수
  없습니다. 다만 "crashed during postload", "reported as incompatible during load",
  "fatal error occurred while loading plugin"은 DLL 코드가 이미 실행된 경우라서
  "로드하지 않음"이 아니라 "정상적으로 로드되지 못함"입니다. 이 경우도 그 실행 시작 때의
  일이므로 원인 근거로 쓰지 않는 원칙은 같습니다.
- v0.2.59 실게임의 SmoothCam 대화상자는 SKSE 검사를 통과해 "loaded correctly"였던
  플러그인이 실행 중 CommonLib Address Library 오류를 띄운 경우였습니다. 선언 검사로는
  잡히지 않으며 ADR-0006의 대화상자 문구 분류로 다룹니다.
- DLL을 직접 파싱하면 SKSE 판정 규칙을 다시 구현하고 따라가야 하며, 신뢰할 수 없는
  바이너리 파서를 하나 더 갖게 됩니다.
- skse64.log에는 시각 정보가 없고 게임을 실행할 때마다 덮어씁니다. 나중에 읽으면 다른
  실행의 로그일 수 있습니다.

## Decision

1. DLL을 직접 파싱하지 않고 SKSE가 남긴 판정(skse64.log)을 그대로 사용한다.
2. 헬퍼는 플러그인 스캔 입력을 모을 때(게임 프로세스가 살아 있을 때) 게임 EXE의 기준
   주소를 함께 읽는다. `Documents\My Games\Skyrim Special Edition*\SKSE\skse64.log`
   중 첫머리 `imagebase =` 값이 그 주소와 같은 로그만 이번 실행의 로그로 인정한다.
   여러 개가 맞으면 가장 최근에 쓰인 로그를 쓴다. 맞는 로그가 없으면
   `no_matching_log`만 기록하고 내용은 쓰지 않는다.
3. 파서는 `plugin <dll> (<dataVersion> <name> <pluginVersion>) <상태> [<코드>] (handle N)`
   줄을 읽고, DLL마다 마지막 줄로 판정한다(로드 후 충돌은 "loaded correctly" 뒤에 남는다).
   마지막 상태가 "loaded correctly"가 아닌 DLL만 목록에 넣는다.
4. SKSE는 DLL 이름을 시스템 ANSI 코드 페이지로 쓰므로, 유효한 UTF-8이 아닌 로그는
   `CP_ACP`에서 UTF-8로 바꾼 뒤 파싱한다.
5. 결과는 플러그인 스캔 JSON의 `skse_log`(`status`, `skse_version`, 개수, 최대 64개의
   `issues`)로 덤프 또는 사이드카에 실린다.
6. 분석기는 `matched`이고 거부된 DLL이 있을 때만 `Low` 근거 항목과 체크리스트 끝의
   `[SKSE]` 안내를 만든다. 이번 사고의 원인 근거가 아니라고 명시하며 후보, 신뢰도,
   요약 문장에는 쓰지 않는다.

## Output Contract

- 플러그인 스캔 JSON(요약 JSON에서는 `plugin_scan.skse_log`):
  `status` (`matched` / `no_matching_log` / `not_found` / `no_image_base` / `error`),
  `matched`일 때 `skse_version`, `checked_count`, `loaded_count`, `issue_count`,
  `issues[] {dll, name, status, code}`. `error`는 로그를 읽다 예외가 난 경우이며 플러그인
  스캔 결과는 그대로 남는다.
- 보고서: `SkseLog: status=... skse=... checked=... loaded=... not_loaded=...`(한국어
  보고서는 `SKSE 로그:`)와 정상적으로 로드되지 못한 DLL별 줄
- 헬퍼 로그: `SKSE log: <status> (checked=..., loaded=..., not_loaded=...)`

## Consequences

### Positive

- SKSE와 판정이 어긋나지 않고, SKSE 버전이 규칙을 바꿔도 따라갈 필요가 없다.
- 공유된 보고서만 보고도 그 실행에서 빠진 플러그인을 알 수 있다.

### Limitations

- 같은 부팅에서 EXE가 같은 주소에 다시 매핑될 수 있어 기준 주소만으로는 연속된 두
  실행을 구분하지 못한다. 폴더가 하나인 일반 설치에서는 현재 실행의 SKSE가 이미 로그를
  덮어썼으므로 문제가 없고, 여러 스토어 폴더에 맞는 로그가 있으면 가장 최근 로그를
  고르는 것으로 줄인다.
- SKSE 플러그인이 아닌 보조 DLL(예: `msdia140.dll`)도 "no version data"로 나온다.
  구형 플러그인과 구분할 수 없어 안내 문구에 두 가능성을 함께 적는다.
- 이 기능 이전의 캡처에는 `skse_log`가 없다.
- 실제 로그로 확인한 형식은 1.6.1170의 SKSE 2.2.6뿐이다. 1.5.97용 SKSE 2.0.x가 다른
  줄 형식을 쓰면 그 줄은 건너뛰어 목록에 나오지 않는다. 샘플 로그를 얻으면 확인한다.
- 플러그인이 자기 로드 도중 게임을 멈추거나 죽이면 결과 줄이 남지 않아 목록에 나오지
  않는다.

## Verification

- `skydiag_skse_log_parser_tests`(Linux): 실제 2.2.6 로그 형식, 공백과 괄호가 든 이름,
  로드 후 충돌, 오류 코드, 깨진 줄.
- `skydiag_plugin_rules_logic_tests`(Linux): `skse_log` JSON 파싱, 상태 문구, 요약.
- `skydiag_plugin_scanner_runtime_tests`: 기준 주소 매칭, 다른 실행의 로그 거부,
  ANSI 이름의 UTF-8 직렬화.
- `skydiag_skse_log_report_tests`: 근거 항목과 `[SKSE]` 안내의 위치와 조건.
- `fuzz_skse_log_parser`: Linux 퍼즈 스모크.

## References

- `helper/src/SkseLogParser.cpp`
- `helper/src/PluginScanner.cpp` (`CollectSkseLogBestEffort`, `MatchSkseLogFiles`)
- `dump_tool/src/PluginRules.cpp` (`DescribeSkseLoadStatus`, `SummarizeSkseLogIssues`)
- `docs/adr/0006-modal-dialog-hang-classification.md`
- SKSE64 `skse64/PluginManager.cpp`
