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
const AnalysisResult::CrashLoggerModReference* FirstCulpritObjectRef(const AnalysisResult& r);

// The blackbox saw a save or cell finish loading in this session, so the game
// got past loading its plugins (and their masters) at startup.
bool GameReachedWorld(const AnalysisResult& r);
// The plugin_rules.json diagnosis for missing masters, if it fired.
const PluginRuleDiagnosis* FindMissingMasterDiagnosis(const AnalysisResult& r);

// True if a suspect is neither a hook framework, system DLL, nor game exe.
bool IsActionableSuspect(const SuspectItem& s);
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

// Split parts of BuildEvidenceAndSummaryImpl.
void BuildActionableCandidates(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
void BuildEvidenceItems(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
void BuildRecommendations(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
std::wstring BuildSummarySentence(const AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
// The lead BuildSummarySentence follows, in everyday words (QuickAnswer.h).
QuickAnswer BuildQuickAnswer(const AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);

}  // namespace skydiag::dump_tool::internal
