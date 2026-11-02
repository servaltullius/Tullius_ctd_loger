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
  // A known root cause names the fix itself and leads even a candidate line
  // (2026-10-13 ContentCatalog.txt CTD).
  assert(SelectNextActionIndex({
           L"[Basics] ExceptionCode=0xE06D7363.",
           L"[Actionable candidate] Tullius callstack first points to DLL candidate X.dll.",
           L"[알려진 원인] ContentCatalog.txt를 휴지통으로 옮긴 뒤 다시 실행하세요.",
         }) == 2u);
  assert(SelectNextActionIndex({
           L"[Known cause] Move ContentCatalog.txt to the Recycle Bin.",
           L"[Modal dialog] Read the dialog.",
         }) == 0u);

  // A forced close (ADR-0022) has nothing to do, ahead of a known cause or a
  // candidate the analysis below still names for mod authors.
  assert(SelectNextActionIndex({
           L"[Basics] ExceptionCode=0xC0000005 (Access Violation).",
           L"[Known cause] Move ContentCatalog.txt to the Recycle Bin.",
           L"[Actionable candidate] Check PrismaUI.dll first.",
           L"[Forced close] This error happened while you force-quit the game (Alt+F4). Nothing to do.",
         }) == 3u);
  assert(SelectNextActionIndex({
           L"[행동 우선 후보] PrismaUI.dll 부터 확인하세요.",
           L"[강제 종료] Alt+F4로 게임을 강제 종료하는 과정에서 난 오류입니다.",
         }) == 1u);

  // One save failing to load in different places (ADR-0025) comes before the
  // modules this dump shows; a known cause still comes first.
  assert(SelectNextActionIndex({
           L"[Basics] ExceptionCode=0xC0000005 (Access Violation).",
           L"[Actionable candidate] Check LocationalEncounterZones.dll first.",
           L"[Damaged save] Loading the same save has crashed or frozen 2 times so far.",
           L"[Save] The game crashed while loading a save.",
         }) == 2u);
  assert(SelectNextActionIndex({
           L"[세이브 손상] 같은 세이브를 불러오다 이번까지 2번 튕기거나 멈췄습니다.",
           L"[알려진 원인] ContentCatalog.txt를 휴지통으로 옮기세요.",
         }) == 1u);

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

  // Also when a file among its recent loads has two providers, as on most
  // large load orders (a real manual capture of 2026-10-30 picked [충돌]).
  assert(SelectNextActionIndex({
           L"[정상/스냅샷] 예외(크래시) 정보가 없습니다.",
           L"[정상/스냅샷] 문제 상황에서 캡처해야 진단이 가능합니다.",
           L"[메쉬/애니] 최근 로드된 리소스 기록이 포함되어 있습니다.",
           L"[충돌] 같은 파일을 제공하는 모드가 2개 이상이면 충돌 가능성이 큽니다.",
         }) == 0u);
  // A conflict still leads a crash without a stronger clue.
  assert(SelectNextActionIndex({
           L"[Basics] ExceptionCode=0xC0000005 (Access Violation).",
           L"[Conflict] If multiple mods provide the same file, conflicts are common.",
         }) == 1u);

  // The tag kinds come from ClassifyRecommendation, so each tier the list
  // names is reached: main thread with modal dialog, top suspect and
  // synchronization stall with the candidates, Crash Logger frame before an
  // object ref, a manual capture with snapshots.
  assert(SelectNextActionIndex({
           L"[Actionable candidate] Check X.dll first.",
           L"[메인 스레드] 메인 스레드가 엔진 내부에서 기다리고 있습니다.",
         }) == 1u);
  assert(SelectNextActionIndex({
           L"[Crash Logger frame] Crash Logger names X.dll first.",
           L"[Top suspect] X.dll.",
         }) == 1u);
  assert(SelectNextActionIndex({
           L"[Crash Logger 프레임] X.dll.",
           L"[동기화 정지] 여러 스레드가 같은 잠금을 기다립니다.",
         }) == 1u);
  assert(SelectNextActionIndex({
           L"[Object ref] Y.esp.",
           L"[Crash Logger frame] X.dll.",
         }) == 1u);
  assert(SelectNextActionIndex({
           L"[Conflict] Two mods provide the same file.",
           L"[수동] 수동 캡처입니다.",
         }) == 1u);
  // The same match the viewer section uses: English tags ignore case.
  assert(SelectNextActionIndex({ L"[Basics] a", L"[actionable candidate] b" }) == 1u);
  assert(SelectNextActionIndex({ L"[Basics] a", L"[Mod check] b", L"[Conflict] c" }) == 2u);

  // Only background left: fall back to the first entry rather than nothing.
  assert(SelectNextActionIndex({ L"[Basics] a", L"[Performance] b" }) == 0u);
  assert(SelectNextActionIndex({}) == std::wstring::npos);
  return 0;
}
