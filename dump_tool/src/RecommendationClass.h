#pragma once

#include <string_view>

namespace skydiag::dump_tool {

// Where the WinUI viewer shows a recommendation, written to the summary as
// recommendation_classes[] (parallel to recommendations[], ADR-0018) so the
// viewer does not have to match display text in two languages.
// group: "immediate" (Do This Now / 지금 바로), "verify" (Verify Next / 추가 확인)
//        or "recapture" (Recapture or Compare / 재수집 / 비교).
// kind:  id of the leading [tag] ("known_cause", "actionable_candidate", ...);
//        empty when the tag is not one the grouping looks at.
struct RecommendationClass
{
  std::string_view group;
  std::string_view kind;
};

inline constexpr std::string_view kRecommendationGroupImmediate = "immediate";
inline constexpr std::string_view kRecommendationGroupVerify = "verify";
inline constexpr std::string_view kRecommendationGroupRecapture = "recapture";

// The grouping the viewer applied to the text before the summary carried it:
// capture-again wording anywhere in the text, or a snapshot/manual tag, makes
// it a recapture line; otherwise the leading tag decides between immediate and
// verify. English tags and wording match ignoring ASCII case, Korean exactly.
RecommendationClass ClassifyRecommendation(std::wstring_view recommendation);

}  // namespace skydiag::dump_tool
