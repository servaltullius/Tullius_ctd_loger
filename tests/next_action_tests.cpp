// NextAction selection, with checklists taken from the 2026-10-09 re-analysis
// of the user's real incident dumps. Taking the first entry gave
// "ExceptionCode=0xC0000005 (Access Violation) ..." or a generic signature line.

#include <cassert>
#include <string>
#include <vector>

#include "NextAction.h"

using skydiag::dump_tool::SelectNextActionIndex;

int main()
{
  // SmoothCam CTD: background first, candidate second.
  assert(SelectNextActionIndex({
           L"[Basics] ExceptionCode=0xC0000005 (Access Violation).",
           L"[Actionable candidate] Tullius callstack first points to DLL candidate SmoothCam.dll.",
           L"[Performance] PerfHitch events were recorded.",
         }) == 1u);

  // CalamityAffixes CTD: a generic signature line and load-order advice lead.
  assert(SelectNextActionIndex({
           L"[Mod check] Update/reinstall the top suspect mod and retest.",
           L"[Load order] This can be a dependency conflict; sort with LOOT and retest.",
           L"[Basics] ExceptionCode=0xC0000005 (Access Violation).",
           L"[Actionable candidate] Crash Logger frame first (direct DLL fault) ...",
         }) == 3u);

  // Game-executable CTD with an object reference as the clue.
  assert(SelectNextActionIndex({
           L"[Mod check] Update/reinstall the top suspect mod and retest.",
           L"[Basics] ExceptionCode=0xC0000005 (Access Violation).",
           L"[Object ref] The game was processing AE_StellarBlade_Doro.esp at crash time ...",
         }) == 2u);

  // A modal dialog outranks a candidate wherever it sits.
  assert(SelectNextActionIndex({
           L"[Actionable candidate] Check SmoothCam.dll first.",
           L"[Modal dialog] The game is waiting for a dialog to be closed.",
         }) == 1u);

  // Korean tags.
  assert(SelectNextActionIndex({
           L"[기본] ExceptionCode=0xC0000005(접근 위반)입니다.",
           L"[성능] PerfHitch 이벤트가 기록되었습니다.",
           L"[행동 우선 후보] SmoothCam.dll 부터 확인하세요.",
         }) == 2u);

  // Without a priority tag, skip background-only entries.
  assert(SelectNextActionIndex({
           L"[Basics] ExceptionCode=0xC0000005 (Access Violation).",
           L"[Optimization] Consider removing or merging unnecessary ESL plugins",
           L"[Check] The crash is in the game executable.",
         }) == 2u);

  // A snapshot explanation is the action for a manual capture.
  assert(SelectNextActionIndex({
           L"[Snapshot] No exception/crash info is present.",
           L"[Performance] PerfHitch events were recorded.",
         }) == 0u);

  // Only background left: fall back to the first entry rather than nothing.
  assert(SelectNextActionIndex({ L"[Basics] a", L"[Performance] b" }) == 0u);
  assert(SelectNextActionIndex({}) == std::wstring::npos);
  return 0;
}
