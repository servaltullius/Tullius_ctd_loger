#pragma once

#include "Analyzer.h"
#include "WctTypes.h"

#include <optional>

namespace skydiag::dump_tool {

struct FreezeSignalInput
{
  bool is_hang_like = false;
  bool is_snapshot_like = false;
  bool is_manual_capture = false;
  bool loading_context = false;
  std::optional<internal::WctFreezeSummary> wct;
  std::optional<BlackboxFreezeSummary> blackbox;
  std::vector<ActionableCandidate> actionable_candidates;
};

FreezeAnalysisResult BuildFreezeCandidateConsensus(const FreezeSignalInput& input, i18n::Language language);

}  // namespace skydiag::dump_tool
