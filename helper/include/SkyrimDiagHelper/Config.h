#pragma once

#include <cstdint>
#include <string>

namespace skydiag::helper {

enum class DumpMode : std::uint32_t {
  kMini = 0,
  kDefault = 1,
  kFull = 2,
};

struct HelperConfig {
  std::uint32_t hangThresholdInGameSec = 10;
  std::uint32_t hangThresholdInMenuSec = 30;
  std::uint32_t hangThresholdLoadingSec = 600;
  DumpMode dumpMode = DumpMode::kDefault;
  std::wstring outputDir;  // empty => next to exe
  bool enableManualCaptureHotkey = true;
  bool autoAnalyzeDump = true;
  std::wstring dumpToolExe = L"SkyrimDiagDumpTool.exe";
  bool enableAdaptiveLoadingThreshold = true;
  std::uint32_t adaptiveLoadingMinSec = 120;
  std::uint32_t adaptiveLoadingMinExtraSec = 120;
  std::uint32_t adaptiveLoadingMaxSec = 1800;
  bool suppressHangWhenNotForeground = true;
};

HelperConfig LoadConfig(std::wstring* err);

}  // namespace skydiag::helper
