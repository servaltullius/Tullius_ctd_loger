#pragma once

#include <string>
#include <vector>

namespace skydiag::dump_tool {

// A plain-language answer for players, read from the finished analysis
// (internal::BuildQuickAnswer). It never changes the candidates or their
// confidence; it only says the lead the summary sentence follows in everyday
// words, and why it is not certain.
struct QuickAnswer
{
  // crash / freeze / loading / snapshot / clean_exit / unknown; empty: not built.
  std::string kind;
  // Which part of the analysis the answer comes from, for tests and tools:
  // candidate / suspect / fault_module / object_ref / hook_stub_race /
  // hook_framework / external_software / known_issue / modal_dialog /
  // crash_logger_thread_dump / gpu_driver / lock_owner / blocked_thread /
  // thread_group / engine_spin_nan / snapshot / manual_snapshot / clean_exit / none.
  std::string case_id;
  // high / medium / low; "none" when nothing is named.
  std::string likelihood = "none";
  std::wstring suspect_mod;   // MO2 mod name when known, else a readable file or program name
  std::wstring suspect_file;  // the DLL or plugin file name (never a path); may be empty
  std::wstring likelihood_text;  // "가능성 중간" / "possible"; empty with likelihood "none"
  std::wstring headline;         // what happened and who is suspected
  std::wstring action;           // one thing to try
  std::wstring why_not_certain;  // empty when likelihood is high or nothing is named
};

// The headline with its likelihood: "... — 가능성 중간" / "... — possible".
inline std::wstring QuickAnswerHeadlineLine(const QuickAnswer& answer)
{
  if (answer.likelihood_text.empty()) {
    return answer.headline;
  }
  return answer.headline + L" — " + answer.likelihood_text;
}

// The report's first block: a title and up to three indented lines.
inline std::vector<std::wstring> QuickAnswerReportLines(const QuickAnswer& answer, bool en)
{
  std::vector<std::wstring> lines;
  if (answer.headline.empty()) {
    return lines;
  }
  lines.emplace_back(en ? L"Quick answer" : L"빠른 답");
  lines.push_back(L"  " + QuickAnswerHeadlineLine(answer));
  if (!answer.action.empty()) {
    lines.push_back((en ? L"  What to try: " : L"  해볼 일: ") + answer.action);
  }
  if (!answer.why_not_certain.empty()) {
    lines.push_back((en ? L"  Why not certain: " : L"  확실하지 않은 이유: ") + answer.why_not_certain);
  }
  return lines;
}

}  // namespace skydiag::dump_tool
