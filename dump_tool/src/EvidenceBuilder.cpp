#include "EvidenceBuilder.h"

#include "EvidenceBuilderInternals.h"

namespace skydiag::dump_tool {

void BuildEvidenceAndSummary(AnalysisResult& r, i18n::Language lang)
{
  internal::BuildEvidenceAndSummaryImpl(r, lang);
}

}  // namespace skydiag::dump_tool
