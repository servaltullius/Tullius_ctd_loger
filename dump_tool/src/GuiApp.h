#pragma once

#include <Windows.h>

#include <string>

#include "Analyzer.h"

namespace skydiag::dump_tool {

struct GuiOptions
{
  bool debug = false;
};

int RunGuiViewer(HINSTANCE hInst, const GuiOptions& opt, const AnalyzeOptions& analyzeOpt, AnalysisResult initial, std::wstring* err);

}  // namespace skydiag::dump_tool

