#include "RecommendationClass.h"

namespace skydiag::dump_tool {
namespace {

struct TagRule
{
  std::string_view kind;
  std::string_view group;
  std::wstring_view en;  // matched ignoring ASCII case
  std::wstring_view ko;  // matched exactly; empty when both languages use en
};

// The leading tags the grouping looks at. Every other tag ([Basics], [Symbols],
// [Freeze], [Load order], ...) is a verify line with no kind.
// tests/recommendation_class_tests.cpp checks that each tag the analyzer and
// its data files write is either listed here or known to stay in verify.
constexpr TagRule kTagRules[] = {
  { "known_cause", kRecommendationGroupImmediate, L"[Known cause]", L"[알려진 원인]" },
  { "known_crash_site", kRecommendationGroupImmediate, L"[Known crash site]", L"[알려진 크래시 지점]" },
  { "modal_dialog", kRecommendationGroupImmediate, L"[Modal dialog]", L"[Modal 대화상자]" },
  { "main_thread", kRecommendationGroupImmediate, L"[Main thread]", L"[메인 스레드]" },
  { "synchronization_stall", kRecommendationGroupImmediate, L"[Synchronization stall]", L"[동기화 정지]" },
  { "actionable_candidate", kRecommendationGroupImmediate, L"[Actionable candidate]", L"[행동 우선 후보]" },
  { "crash_logger_frame", kRecommendationGroupImmediate, L"[Crash Logger frame]", L"[Crash Logger 프레임]" },
  { "top_suspect", kRecommendationGroupImmediate, L"[Top suspect]", L"[유력 후보]" },
  { "esp_esm", kRecommendationGroupImmediate, L"[ESP/ESM]", L"" },
  { "object_ref", kRecommendationGroupImmediate, L"[Object ref]", L"[오브젝트 참조]" },
  { "conflict", kRecommendationGroupImmediate, L"[Conflict]", L"[충돌]" },
  { "problem_module", kRecommendationGroupImmediate, L"[Problem module]", L"[문제 모듈]" },
  { "masters", kRecommendationGroupImmediate, L"[Masters]", L"[마스터]" },
  { "bees", kRecommendationGroupImmediate, L"[BEES]", L"" },
  { "check", kRecommendationGroupImmediate, L"[Check]", L"[점검]" },
  { "hook_framework", kRecommendationGroupImmediate, L"[Hook framework]", L"[훅 프레임워크]" },
  { "external_software", kRecommendationGroupImmediate, L"[External software]", L"[외부 프로그램]" },
  { "snapshot", kRecommendationGroupRecapture, L"[Snapshot]", L"[정상/스냅샷]" },
  { "manual", kRecommendationGroupRecapture, L"[Manual]", L"[수동]" },
  { "recapture", kRecommendationGroupRecapture, L"[Recapture]", L"[재수집]" },
};

// Capture-again wording anywhere in the text outranks the tag: a known cause
// or a candidate line that asks for another dump is a recapture line.
constexpr std::wstring_view kRecaptureWordingEn[] = {
  L"DumpMode=2", L"capture another", L"recapture", L"capture taken during", L"second signal", L"break the tie",
};
constexpr std::wstring_view kRecaptureWordingKo[] = {
  L"다시 캡처", L"재수집", L"한 번 더", L"두 번째 신호",
};

wchar_t AsciiLower(wchar_t c)
{
  return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

bool EqualsAsciiIgnoreCase(std::wstring_view a, std::wstring_view b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (AsciiLower(a[i]) != AsciiLower(b[i])) {
      return false;
    }
  }
  return true;
}

bool StartsWithIgnoreCase(std::wstring_view text, std::wstring_view prefix)
{
  return text.size() >= prefix.size() && EqualsAsciiIgnoreCase(text.substr(0, prefix.size()), prefix);
}

bool ContainsIgnoreCase(std::wstring_view text, std::wstring_view needle)
{
  if (needle.size() > text.size()) {
    return false;
  }
  for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
    if (EqualsAsciiIgnoreCase(text.substr(i, needle.size()), needle)) {
      return true;
    }
  }
  return false;
}

// The viewer trims each entry before grouping it.
bool IsTrimmedSpace(wchar_t c)
{
  return c == L' ' || (c >= L'\t' && c <= L'\r') || c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
         c == 0x3000;
}

std::wstring_view Trim(std::wstring_view text)
{
  while (!text.empty() && IsTrimmedSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsTrimmedSpace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

const TagRule* FindTagRule(std::wstring_view text)
{
  for (const auto& rule : kTagRules) {
    if (StartsWithIgnoreCase(text, rule.en) || (!rule.ko.empty() && text.rfind(rule.ko, 0) == 0)) {
      return &rule;
    }
  }
  return nullptr;
}

bool HasRecaptureWording(std::wstring_view text)
{
  for (const auto needle : kRecaptureWordingEn) {
    if (ContainsIgnoreCase(text, needle)) {
      return true;
    }
  }
  for (const auto needle : kRecaptureWordingKo) {
    if (text.find(needle) != std::wstring_view::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

RecommendationClass ClassifyRecommendation(std::wstring_view recommendation)
{
  const auto text = Trim(recommendation);
  const TagRule* rule = FindTagRule(text);
  RecommendationClass result{ kRecommendationGroupVerify, rule ? rule->kind : std::string_view{} };
  if (HasRecaptureWording(text) || (rule && rule->group == kRecommendationGroupRecapture)) {
    result.group = kRecommendationGroupRecapture;
  } else if (rule) {
    result.group = rule->group;
  }
  return result;
}

}  // namespace skydiag::dump_tool
