#include "NextAction.h"

#include <string_view>

namespace skydiag::dump_tool {
namespace {

template <std::size_t N>
bool StartsWithAny(const std::wstring& text, const std::wstring_view (&tags)[N])
{
  for (const auto tag : tags) {
    if (text.rfind(tag, 0) == 0) {
      return true;
    }
  }
  return false;
}

template <std::size_t N>
std::size_t FindFirstTagged(const std::vector<std::wstring>& recommendations, const std::wstring_view (&tags)[N])
{
  for (std::size_t i = 0; i < recommendations.size(); ++i) {
    if (StartsWithAny(recommendations[i], tags)) {
      return i;
    }
  }
  return std::wstring::npos;
}

// Same priority tags the WinUI viewer uses for its next action, plus the other
// candidate-level tags the checklist emits.
constexpr std::wstring_view kModalDialog[] = { L"[Modal dialog]", L"[Modal 대화상자]" };
constexpr std::wstring_view kCandidate[] = {
  L"[Actionable candidate]", L"[행동 우선 후보]", L"[Top suspect]", L"[유력 후보]",
  L"[Synchronization stall]", L"[동기화 정지]",
};
constexpr std::wstring_view kCrashLoggerFrame[] = { L"[Crash Logger frame]", L"[Crash Logger 프레임]" };
constexpr std::wstring_view kObjectRef[] = { L"[Object ref]", L"[오브젝트 참조]" };
constexpr std::wstring_view kConflict[] = { L"[Conflict]", L"[충돌]" };

// Explanations and upkeep that never make a first action on their own.
constexpr std::wstring_view kBackground[] = {
  L"[Basics]", L"[기본]", L"[Performance]", L"[성능]", L"[Optimization]", L"[최적화]",
  L"[Troubleshooting]", L"[트러블슈팅]", L"[SKSE]", L"[Symbols]", L"[심볼]",
};

}  // namespace

std::size_t SelectNextActionIndex(const std::vector<std::wstring>& recommendations)
{
  if (recommendations.empty()) {
    return std::wstring::npos;
  }
  for (const std::size_t found : {
         FindFirstTagged(recommendations, kModalDialog),
         FindFirstTagged(recommendations, kCandidate),
         FindFirstTagged(recommendations, kCrashLoggerFrame),
         FindFirstTagged(recommendations, kObjectRef),
         FindFirstTagged(recommendations, kConflict),
       }) {
    if (found != std::wstring::npos) {
      return found;
    }
  }
  for (std::size_t i = 0; i < recommendations.size(); ++i) {
    if (!StartsWithAny(recommendations[i], kBackground)) {
      return i;
    }
  }
  return 0;
}

}  // namespace skydiag::dump_tool
