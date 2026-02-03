#pragma once

#include "Analyzer.h"

namespace skydiag::dump_tool {

// Builds human-facing evidence, recommendations, and summary sentence from the parsed dump.
// Heuristic only: always treat results as "best-effort" unless confidence is explicitly high.
void BuildEvidenceAndSummary(AnalysisResult& r, i18n::Language lang);

}  // namespace skydiag::dump_tool
