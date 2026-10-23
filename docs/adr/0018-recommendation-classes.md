# ADR-0018: 권장 조치의 묶음을 분석기가 정한다

## Status
Accepted

## Context
WinUI 뷰어는 권장 조치를 "지금 바로 / 추가 확인 / 재수집 / 비교"로 나눠 보여 줍니다. 지금까지는 뷰어가 문구를 보고 나눴습니다. `[Actionable candidate]`·`[행동 우선 후보]` 같은 머리 태그를 영어(대소문자 무시)와 한국어(정확히)로 맞춰 보고, "capture another"·"다시 캡처" 같은 말이 들어 있으면 재수집으로 보냈습니다.

- 태그 이름을 바꾸거나 번역을 고치면, 아무 오류 없이 그 줄이 다른 묶음으로 옮겨 갑니다.
- 같은 판단을 분석기(`NextAction.cpp`)와 뷰어가 따로 들고 있습니다.
- 같은 사고라도 언어에 따라 묶음이 다를 수 있습니다. 보관 덤프 5건(프리징)에서 `[프리징] … 다시 캡처하세요`는 재수집, 영어 `[Freeze] … repeat capture during the issue.`는 추가 확인으로 나뉩니다.

## Decision
1. 분석기가 권장 조치마다 묶음을 정해 요약 JSON에 `recommendation_classes`로 씁니다. `recommendations`와 같은 길이·순서의 배열이고, 항목은 `{ "group": …, "kind": … }`입니다.
   - `group`: `immediate`(지금 바로), `verify`(추가 확인), `recapture`(재수집 / 비교)
   - `kind`: 묶음을 정한 머리 태그의 id(`known_cause`, `actionable_candidate`, `modal_dialog`, `snapshot` …). 묶음에 영향이 없는 태그는 빈 문자열입니다.
   - 기존 `recommendations` 문자열 배열은 그대로 두므로, 이전 뷰어도 지금처럼 동작합니다. 스키마 버전은 올리지 않습니다(필드 추가).
2. 분류 규칙은 `dump_tool/src/RecommendationClass.cpp` 한 곳에 둡니다. 규칙은 뷰어가 하던 것과 같습니다. 재수집 문구나 `[Snapshot]`·`[Manual]` 태그가 있으면 재수집, 아니면 태그 표에 있는 태그는 그 묶음, 나머지는 추가 확인입니다. 문구를 만드는 분석기와 같은 빌드에서 분류하므로, 문구와 분류가 어긋날 수 없습니다.
3. 뷰어는 `recommendation_classes`가 있으면 그것만 봅니다. 이 필드가 없는 이전 요약, 또는 이 뷰어가 모르는 `group` 값일 때만 예전의 문구 비교로 나눕니다. 다음 조치 카드에서 "후보 줄"인지 가리는 것도 `kind`(`actionable_candidate`, `top_suspect`, `synchronization_stall`, `crash_logger_frame`)로 합니다.
4. 이 변경은 묶음을 바꾸지 않습니다. 위의 언어별 차이도 그대로 둡니다. 고치려면 이제 `RecommendationClass.cpp`의 규칙 한 줄을 바꾸면 됩니다.

## Consequences
- 태그를 바꾸거나 새 태그를 쓰면 `skydiag_recommendation_class_tests`가 실패합니다. 이 테스트는 `EvidenceBuilderRecommendations.cpp`와 데이터 파일(`crash_signatures.json`, `graphics_injection_rules.json`, `plugin_rules.json`)의 모든 머리 태그가 분류 표에 있거나 "추가 확인으로 남는 태그" 목록에 있는지 봅니다. 새 태그를 쓸 때 어느 묶음인지 정해야 합니다.
- 분류는 아직 문구에서 합니다(분석기 안에서). 권장 조치를 만드는 곳마다 묶음을 직접 붙이면 더 단단하지만, 100곳이 넘는 호출부와 데이터 파일 형식을 바꿔야 해서 이번에는 하지 않았습니다.
- 경로 가리기(redaction) 전의 문구로 분류하므로, 가리기 설정과 상관없이 묶음이 같습니다.

## Verification
- `skydiag_recommendation_class_tests`: 태그별 묶음과 kind(영어 대소문자 무시, 한국어 정확히), 재수집 문구가 태그보다 우선, 공백 정리, 소스·데이터의 모든 머리 태그 점검.
- `skydiag_summary_writer_runtime_tests`: `BuildSummaryJson`이 `recommendation_classes`를 `recommendations`와 같은 길이로 쓰고, `[Check]` 줄이 `immediate`/`check`.
- `skydiag_share_text_fixture_tests`: 분류가 문구와 다를 때 뷰어가 분류를 따르고, 모르는 group과 필드가 없는 요약은 문구로 나누며, 다음 조치가 `kind`로 후보 줄을 가린다.
- `skydiag_winui_xaml_tests`: 뷰어가 `recommendation_classes`를 읽고 그것으로 묶는다.
- 보관 덤프 19건 × 영어·한국어: 이전 분석기 요약을 이전 뷰어로 본 묶음·다음 조치와, 새 분석기 요약을 새 뷰어로 본 것(분류 사용), 분류를 지운 요약(문구 비교), 새 요약을 이전 뷰어로 본 것이 38건 모두 같고, 권장 조치 문구도 같다.
