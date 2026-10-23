// The quick answer block the report starts with (QuickAnswer.h, header-only,
// portable): a title, the headline with its likelihood, what to try and,
// only when there is one, why it is not certain.

#include <cassert>
#include <iostream>
#include <string>

#include "QuickAnswer.h"

namespace {

using skydiag::dump_tool::QuickAnswer;
using skydiag::dump_tool::QuickAnswerHeadlineLine;
using skydiag::dump_tool::QuickAnswerReportLines;

void TestEmptyAnswerHasNoBlock()
{
  const QuickAnswer empty{};
  assert(QuickAnswerReportLines(empty, true).empty());
  assert(QuickAnswerReportLines(empty, false).empty());
}

void TestFullBlock()
{
  QuickAnswer qa{};
  qa.kind = "crash";
  qa.likelihood = "medium";
  qa.likelihood_text = L"possible";
  qa.headline = L"The game crashed to desktop (CTD). Suspected mod: Example Mod (ExampleMod.dll)";
  qa.action = L"Disable or update Example Mod, then play again.";
  qa.why_not_certain = L"The game crashed inside this mod, but another mod can cause a crash that shows up here.";

  assert(QuickAnswerHeadlineLine(qa) == qa.headline + L" — possible");
  const auto en = QuickAnswerReportLines(qa, true);
  assert(en.size() == 4u);
  assert(en[0] == L"Quick answer");
  assert(en[1] == L"  " + QuickAnswerHeadlineLine(qa));
  assert(en[2] == L"  What to try: " + qa.action);
  assert(en[3] == L"  Why not certain: " + qa.why_not_certain);

  const auto ko = QuickAnswerReportLines(qa, false);
  assert(ko.size() == 4u);
  assert(ko[0] == L"빠른 답");
  assert(ko[2].rfind(L"  해볼 일: ", 0) == 0);
  assert(ko[3].rfind(L"  확실하지 않은 이유: ", 0) == 0);

  // ADR-0019: how it crashed, after the doubt line.
  qa.how_it_crashed = L"It crashed while using an object that was already gone or never created (a null pointer).";
  const auto withHow = QuickAnswerReportLines(qa, true);
  assert(withHow.size() == 5u && withHow[4] == L"  How it crashed: " + qa.how_it_crashed);
  assert(QuickAnswerReportLines(qa, false)[4].rfind(L"  어떻게 튕겼나: ", 0) == 0);
}

void TestNoLikelihoodNoDoubt()
{
  QuickAnswer qa{};
  qa.kind = "snapshot";
  qa.headline = L"This is a snapshot, not a crash or a freeze.";
  qa.action = L"Nothing to do: this is a snapshot you took, and it does not mean anything is wrong by itself.";
  assert(QuickAnswerHeadlineLine(qa) == qa.headline);
  const auto lines = QuickAnswerReportLines(qa, true);
  assert(lines.size() == 3u);
  assert(lines[1] == L"  " + qa.headline);
}

}  // namespace

int main()
{
  TestEmptyAnswerHasNoBlock();
  TestFullBlock();
  TestNoLikelihoodNoDoubt();
  std::cout << "quick_answer_format_tests: all passed\n";
  return 0;
}
