#pragma once

#include "EvidenceBuilderPrivate.h"

namespace skydiag::dump_tool::internal {

std::wstring DescribeCaptureProfileEvidence(const AnalysisResult& r, bool en);
std::wstring DescribeRecaptureEvaluationEvidence(const AnalysisResult& r, bool en);
std::wstring DescribeSymbolRuntimeEvidence(const AnalysisResult& r, bool en);
std::wstring DescribeFreezeSupportQuality(std::string_view supportQuality, bool en);
bool CandidateHasFamily(const ActionableCandidate& candidate, std::string_view familyId);
bool HasScorableFirstChanceContext(const FirstChanceSummary& summary);
std::wstring DescribeFirstChanceSupport(const FirstChanceSummary& summary, bool en);

void BuildCrashLoggerEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
void BuildSuspectEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
void BuildResourceEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
void BuildHitchAndFreezeEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
void BuildModuleClassificationEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
void BuildWctEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);
void BuildHistoryEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx);

}  // namespace skydiag::dump_tool::internal
