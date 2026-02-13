#include "HelperLog.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "HelperCommon.h"
#include "SkyrimDiagHelper/Retention.h"

namespace skydiag::helper::internal {
namespace {

std::uint64_t g_maxHelperLogBytes = 0;
std::uint32_t g_maxHelperLogFiles = 0;

}  // namespace

void SetHelperLogRotation(std::uint64_t maxBytes, std::uint32_t maxFiles)
{
  g_maxHelperLogBytes = maxBytes;
  g_maxHelperLogFiles = maxFiles;
}

void ClearLog(const std::filesystem::path& outBase)
{
  std::error_code ec;
  const auto path = outBase / L"SkyrimDiagHelper.log";
  if (std::filesystem::exists(path, ec)) {
    // Truncate: open in non-append mode and immediately close.
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
  }
}

void AppendLogLine(const std::filesystem::path& outBase, std::wstring_view line)
{
  std::error_code ec;
  std::filesystem::create_directories(outBase, ec);

  const auto path = outBase / L"SkyrimDiagHelper.log";
  skydiag::helper::RotateLogFileIfNeeded(path, g_maxHelperLogBytes, g_maxHelperLogFiles);
  std::ofstream f(path, std::ios::binary | std::ios::app);
  if (!f) {
    return;
  }

  std::wstring msg(line);
  msg += L"\r\n";
  const auto utf8 = WideToUtf8(msg);
  if (!utf8.empty()) {
    f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
  }
}

}  // namespace skydiag::helper::internal

