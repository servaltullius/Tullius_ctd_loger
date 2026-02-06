#pragma once

#include <Windows.h>

#include <string>

#include "Analyzer.h"

namespace skydiag::dump_tool {

struct GuiOptions
{
  bool debug = false;
  bool beginnerMode = true;
};

bool TryReuseExistingViewerForDump(
  const std::wstring& dumpPath,
  const AnalyzeOptions& analyzeOpt,
  const GuiOptions& guiOpt,
  std::wstring* err);

int RunGuiViewer(HINSTANCE hInst, const GuiOptions& opt, const AnalyzeOptions& analyzeOpt, AnalysisResult initial, std::wstring* err);

}  // namespace skydiag::dump_tool
