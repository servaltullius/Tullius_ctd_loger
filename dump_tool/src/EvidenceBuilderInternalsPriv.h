#pragma once

// Private header for splitting EvidenceBuilderInternals.cpp into smaller translation units.
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
  bool isCrashLike = false;
  bool isHangLike = false;
  bool isSnapshotLike = false;
  bool isManualCapture = false;

  bool hasModule = false;
  bool isSystem = false;
  bool isGameExe = false;

  bool wctSuggestsHang = false;

  HitchSummary hitch{};
  std::optional<WctInfo> wct{};
  std::wstring suspectBasis;
};

// Shared utility helpers (moved out of the original monolithic .cpp).
std::wstring ConfidenceText(i18n::Language lang, i18n::ConfidenceLevel level);
std::wstring WideLower(std::wstring_view s);
std::wstring JoinList(const std::vector<std::wstring>& items, std::size_t maxN, std::wstring_view sep);
std::wstring ToWideAscii(std::string_view s);
std::wstring Hex64(std::uint64_t v);

std::optional<std::wstring> TryExplainExceptionInfo(const AnalysisResult& r, bool en);
bool IsSystemishModule(std::wstring_view filename);
bool IsGameExeModule(std::wstring_view filename);

std::optional<WctInfo> TrySummarizeWct(std::string_view utf8);
HitchSummary ComputeHitchSummary(const std::vector<EventRow>& events);

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

// Split parts of BuildEvidenceAndSummaryImpl.
void BuildEvidenceItems(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
void BuildRecommendations(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
std::wstring BuildSummarySentence(const AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);

}  // namespace skydiag::dump_tool::internal

