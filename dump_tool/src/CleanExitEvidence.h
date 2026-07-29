#pragma once

#include <string>

namespace skydiag::dump_tool {

struct AnalysisResult;

// Recognizes only a finalized helper record that is bound to this exact dump
// and agrees with both the minidump exception stream and blackbox snapshot.
// Missing, partial, stale, or ambiguous evidence fails closed.
bool TryConsumeCleanExitEvidence(
  const std::wstring& dumpPath,
  AnalysisResult& result);

}  // namespace skydiag::dump_tool
