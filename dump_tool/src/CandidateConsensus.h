#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Analyzer.h"

namespace skydiag::dump_tool {

struct CandidateSignal
{
  std::string family_id;
  std::wstring candidate_key;
  std::wstring display_name;
  std::wstring plugin_name;
  std::wstring mod_name;
  std::wstring module_filename;
  std::wstring detail;
  std::uint32_t weight = 0;
};

std::wstring CanonicalCandidateKey(std::wstring_view value);
std::vector<ActionableCandidate> BuildCandidateConsensus(const std::vector<CandidateSignal>& signals, i18n::Language language);

}  // namespace skydiag::dump_tool
