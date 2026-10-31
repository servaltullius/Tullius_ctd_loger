#pragma once

// Private header shared by EvidenceBuilder translation units.
// Not part of the public API; only used inside dump_tool.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Analyzer.h"

namespace skydiag::dump_tool::internal {

struct WctInfo
{
  int threads = 0;
  int cycles = 0;
  bool has_capture = false;
  std::string capture_kind;
  double secondsSinceHeartbeat = 0.0;
  std::uint32_t thresholdSec = 0;
  bool isLoading = false;
  bool suggestsHang = false;
};

struct HitchSummary
{
  std::uint32_t count = 0;
  std::uint64_t maxMs = 0;
  std::uint64_t p95Ms = 0;
};

struct EvidenceBuildContext
{
  bool en = false;

  bool hasException = false;
  bool isFilteredCleanExit = false;
  // ADR-0022: a crash right after the player asked the game to close. Still
  // crash-like (the analysis stays for mod authors), but never called a CTD.
  bool isForcedClose = false;
  bool isCrashLike = false;
  bool isHangLike = false;
  bool isSnapshotLike = false;
  bool isManualCapture = false;

  bool hasModule = false;
  // Also true for a GPU vendor tool or overlay (externalSoftware names it).
  bool isSystem = false;
  std::wstring externalSoftware;
  bool isGameExe = false;
  bool isHookFramework = false;
  bool hasSignatureMatch = false;

  bool wctSuggestsHang = false;

  HitchSummary hitch{};
  std::optional<WctInfo> wct{};
  std::wstring suspectBasis;
};

// ConfidenceText is now defined in I18nCore.h as i18n::ConfidenceText.
// It's found automatically via ADL when called with i18n::Language/ConfidenceLevel arguments.

std::wstring JoinList(const std::vector<std::wstring>& items, std::size_t maxN, std::wstring_view sep);
std::wstring ToWideAscii(std::string_view s);
// printf's "%.*f" without a fixed buffer: a value read from the dump (a WCT
// heartbeat age of 1e308) can be hundreds of digits long.
std::wstring FormatDecimal(double value, int decimals);
// "12 seconds" / "84 minutes" (Korean: "12초" / "84분").
std::wstring DescribeDuration(double seconds, bool en);
// Crash Logger's top modules other than the fault module and the SKSE runtime
// (the path plugin tasks take): DLLs worth comparing with the fault module.
bool HasNearbyProbableDll(const AnalysisResult& r);
// The game state the blackbox recorded at the capture (open menus, a long
// pause before it), as sentences; empty when there is nothing to say.
std::wstring DescribeGameStateAtCapture(const BlackboxFreezeSummary& blackbox, bool en);
std::wstring Hex64(std::uint64_t v);

std::optional<std::wstring> TryExplainExceptionInfo(const AnalysisResult& r, bool en);

std::optional<WctInfo> TrySummarizeWct(std::string_view utf8);
HitchSummary ComputeHitchSummary(const std::vector<EventRow>& events);
HitchSummary ComputeHitchSummaryInRange(const std::vector<EventRow>& events, double fromMs, double toMs);

bool IsKeyResourceKind(std::wstring_view kind);
std::optional<double> FindLastEventTimeMsByType(const std::vector<EventRow>& events, std::uint16_t type);
std::optional<double> InferCaptureAnchorMs(const AnalysisResult& r);
std::optional<double> InferHeartbeatAgeFromResultSec(const AnalysisResult& r);

std::wstring FormatResourceHitLine(const ResourceRow& rr, double anchorMs);
std::vector<const ResourceRow*> FindResourcesNearAnchor(
  const std::vector<ResourceRow>& resources,
  double anchorMs,
  double windowBeforeMs,
  double windowAfterMs);
std::vector<std::wstring> InferProviderScoresFromResources(const std::vector<const ResourceRow*>& hits);
std::vector<std::wstring> InferPerfSuspectsFromResourceCorrelation(
  const std::vector<EventRow>& events,
  const std::vector<ResourceRow>& resources);
// Appended to evidence naming MO2 providers that were read from disk at
// analysis time instead of captured with the dump (audit F05); empty otherwise.
std::wstring Mo2ProvidersCaveat(const AnalysisResult& r, bool en);
// Provider-based evidence is not trusted beyond kLow without the crash-time profile.
i18n::ConfidenceLevel CapForMo2ProvidersSource(const AnalysisResult& r, i18n::ConfidenceLevel level);

std::wstring BuildPreFreezeContextLine(const std::vector<EventRow>& events, bool en);

// The most relevant Crash Logger object ref that can name a culprit plugin.
// Cells and worldspaces only say where the player was, so they are skipped
// here as they are for candidates and history; evidence still lists them.
// None from a log that may be another crash's (crash_logger_pairing_ambiguous).
const AnalysisResult::CrashLoggerModReference* FirstCulpritObjectRef(const AnalysisResult& r);

// ADR-0019: the CPU was sent to run code where there is none (null_jump /
// bad_jump). The fault address is then in no module because there is no code
// there, not because the dump lacks the module list.
bool FaultIsJumpToNoCode(const AnalysisResult& r);

// A jump into memory that had been freed (target_state "freed"), such as a
// hook stub a plugin removed while another one still held its address as the
// original function (the 2026-10 exit crashes, called from PrismaUI.dll). The
// caller only used that address, and the dump does not show who freed it, so
// the caller is no suspect on that alone (as with ADR-0016's hook stubs).
bool FaultJumpedIntoFreedCode(const AnalysisResult& r);
// The module of the decoded call site ("PrismaUI.dll" of "PrismaUI.dll+0x59ce2"),
// or empty.
std::wstring FaultCallSiteModule(const AnalysisResult& r);

// The blackbox saw a save or cell finish loading in this session, so the game
// got past loading its plugins (and their masters) at startup.
bool GameReachedWorld(const AnalysisResult& r);
// The plugin_rules.json diagnosis for missing masters, if it fired.
const PluginRuleDiagnosis* FindMissingMasterDiagnosis(const AnalysisResult& r);

// True if a suspect is neither a hook framework, system DLL, nor game exe.
bool IsActionableSuspect(const SuspectItem& s);
// The mod DLL a modal dialog names: "SmoothCam (SmoothCam.dll)" or the file.
std::wstring DialogNamedModuleDisplay(const ModalDialogWaitInfo& modal);
// When the dialog names no single loaded mod DLL: the DLL names it does give,
// as one plain sentence (several loaded ones, or ones the game did not load);
// empty when it names none.
std::wstring DescribeDialogNamedFiles(const ModalDialogWaitInfo& modal, bool en);
// minidump::IsSystemishModuleAt for a module known here only by file name (an
// actionable candidate): the path is taken from the dump's fault module or a
// stack suspect of that name, so an ENB/ReShade d3d11.dll in the game folder
// stays a mod.
bool DumpModuleIsSystemish(const AnalysisResult& r, std::wstring_view filename);
// The candidate rests on the Tullius callstack alone: a real stack walk (not
// the pointer scan) put it first with Medium weight (>= 5), and no Crash Logger
// frame or object ref, nearby resource or thread group backs it. History and
// first-chance context do not count toward that weight.
bool CandidateHasStandaloneCallstackSupport(const AnalysisResult& r, const ActionableCandidate& candidate);
// Where a fault outside the mods happened, for the summary sentences:
// "a Windows system DLL", or "external software (NVIDIA, NvTelemetryBridge64.dll)".
std::wstring FaultPlace(const AnalysisResult& r, const EvidenceBuildContext& ctx);
bool IsWeakFaultLocationOnlySuspect(const AnalysisResult& r, const EvidenceBuildContext& ctx);
bool IsWeakFaultLocationActionableCandidate(
  const AnalysisResult& r,
  const ActionableCandidate& candidate,
  const EvidenceBuildContext& ctx);

// ADR-0022: "Alt+F4로 게임을 강제 종료하는 과정에서 난 오류입니다. 플레이 중에 튕긴
// CTD가 아닙니다." / "This error happened while you force-quit the game
// (Alt+F4). It is not a crash during play.", by the close request's kind.
std::wstring ForcedCloseLead(const ForcedCloseInfo& forcedClose, bool en);

// Split parts of BuildEvidenceAndSummaryImpl.
void BuildActionableCandidates(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
void BuildEvidenceItems(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
void BuildRecommendations(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
std::wstring BuildSummarySentence(const AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
// The lead BuildSummarySentence follows, in everyday words (QuickAnswer.h).
QuickAnswer BuildQuickAnswer(const AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);

}  // namespace skydiag::dump_tool::internal
