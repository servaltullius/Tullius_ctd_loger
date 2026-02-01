#pragma once

#include <string>

#include "Analyzer.h"

namespace skydiag::dump_tool {

bool WriteOutputs(const AnalysisResult& r, std::wstring* err);

}  // namespace skydiag::dump_tool

