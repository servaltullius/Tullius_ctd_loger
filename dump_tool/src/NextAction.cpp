#include "NextAction.h"

#include <string_view>

#include "RecommendationClass.h"

namespace skydiag::dump_tool {
namespace {

struct PriorityKind
{
  std::string_view kind;  // a leading-tag kind from RecommendationClass.cpp
  int rank;               // lower acts first
};

// Same priority the WinUI viewer uses for its next action, plus the other
// candidate-level tags the checklist emits.
// - A forced close (ADR-0022) says there is nothing to do, before any module.
// - A known root cause (crash_signatures.json) names the fix itself.
// - One save that keeps failing to load in different places (ADR-0025) says
//   more than the modules this dump shows.
// - What the main thread was doing (modal dialog, engine or driver wait) comes
//   before any module triage, and so does a graphics driver error moments
//   before a crash (ADR-0029): with the graphics device gone, the module the
//   crash is in is where it showed, not why.
// - A snapshot of a running game has nothing to fix (its quick answer says so);
//   a file two mods provide among its recent loads is no reason to act.
constexpr PriorityKind kPriorityKinds[] = {
  { "forced_close", 0 },
  { "known_cause", 1 },
  { "damaged_save", 2 },
  { "modal_dialog", 3 },
  { "main_thread", 3 },
  { "driver_error_before_crash", 3 },
  { "actionable_candidate", 4 },
  { "top_suspect", 4 },
  { "synchronization_stall", 4 },
  { "crash_logger_frame", 5 },
  { "object_ref", 6 },
  { "snapshot", 7 },
  { "manual", 7 },
  { "conflict", 8 },
};
constexpr int kNoPriority = 1000;

int PriorityRank(std::string_view kind)
{
  for (const auto& entry : kPriorityKinds) {
    if (!kind.empty() && entry.kind == kind) {
      return entry.rank;
    }
  }
  return kNoPriority;
}

// Explanations and upkeep that never make a first action on their own (the
// classifier gives these tags no kind).
constexpr std::wstring_view kBackground[] = {
  L"[Basics]", L"[기본]", L"[Performance]", L"[성능]", L"[Optimization]", L"[최적화]",
  L"[Troubleshooting]", L"[트러블슈팅]", L"[SKSE]", L"[Symbols]", L"[심볼]",
};

bool IsBackground(const std::wstring& text)
{
  for (const auto tag : kBackground) {
    if (text.rfind(tag, 0) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::size_t SelectNextActionIndex(const std::vector<std::wstring>& recommendations)
{
  if (recommendations.empty()) {
    return std::wstring::npos;
  }
  std::size_t best = std::wstring::npos;
  int bestRank = kNoPriority;
  for (std::size_t i = 0; i < recommendations.size(); ++i) {
    const int rank = PriorityRank(ClassifyRecommendation(recommendations[i]).kind);
    if (rank < bestRank) {
      best = i;
      bestRank = rank;
    }
  }
  if (best != std::wstring::npos) {
    return best;
  }
  for (std::size_t i = 0; i < recommendations.size(); ++i) {
    if (!IsBackground(recommendations[i])) {
      return i;
    }
  }
  return 0;
}

}  // namespace skydiag::dump_tool
