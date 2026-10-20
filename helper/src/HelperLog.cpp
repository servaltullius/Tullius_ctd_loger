#include "HelperLog.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "HelperCommon.h"
#include "SkyrimDiagHelper/Retention.h"

namespace skydiag::helper::internal {
namespace {

std::uint64_t g_maxHelperLogBytes = 0;
std::uint32_t g_maxHelperLogFiles = 0;
std::uint32_t g_helperSessionLogsToKeep = kDefaultHelperSessionLogsToKeep;
std::vector<std::wstring> g_sessionStartLines;

void RotateAndStartSession(const std::filesystem::path& outBase)
{
  std::error_code ec;
  const auto path = outBase / L"SkyrimDiagHelper.log";
  if (!std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0) {
    return;
  }
  // Earlier sessions shift down: previous.log -> previous.2.log -> ... A crash
  // is often followed by several relaunches before anyone reads the logs.
  const auto keep = std::max<std::uint32_t>(1u, g_helperSessionLogsToKeep);
  std::filesystem::remove(HelperSessionLogPath(outBase, keep), ec);
  for (std::uint32_t i = keep; i > 1u; --i) {
    const auto src = HelperSessionLogPath(outBase, i - 1u);
    if (std::filesystem::exists(src, ec)) {
      std::filesystem::rename(src, HelperSessionLogPath(outBase, i), ec);
    }
  }
  const auto previous = HelperSessionLogPath(outBase, 1u);
  std::filesystem::remove(previous, ec);
  ec.clear();
  std::filesystem::rename(path, previous, ec);
  if (ec) {
    ClearLog(outBase);  // a locked log is still restarted, as before
  }
}

}  // namespace

void QueueSessionStartLogLine(std::wstring line)
{
  if (std::find(g_sessionStartLines.begin(), g_sessionStartLines.end(), line) == g_sessionStartLines.end()) {
    g_sessionStartLines.push_back(std::move(line));
  }
}

void SetHelperLogRotation(std::uint64_t maxBytes, std::uint32_t maxFiles)
{
  g_maxHelperLogBytes = maxBytes;
  g_maxHelperLogFiles = maxFiles;
}

void SetHelperSessionLogsToKeep(std::uint32_t sessions)
{
  g_helperSessionLogsToKeep = sessions;
}

std::filesystem::path HelperSessionLogPath(const std::filesystem::path& outBase, std::uint32_t age)
{
  if (age <= 1u) {
    return outBase / L"SkyrimDiagHelper.previous.log";
  }
  return outBase / (L"SkyrimDiagHelper.previous." + std::to_wstring(age) + L".log");
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

void StartHelperLogSession(const std::filesystem::path& outBase)
{
  RotateAndStartSession(outBase);
  for (const auto& line : g_sessionStartLines) {
    AppendLogLine(outBase, line);
  }
  g_sessionStartLines.clear();
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

std::wstring AnsiExceptionText(const std::exception& ex)
{
  const std::string what = ex.what();
  if (what.empty() || what.size() > 0x7FFFFFFFu) {
    return {};
  }
  const int len = static_cast<int>(what.size());
  const int wideLen = MultiByteToWideChar(CP_ACP, 0, what.data(), len, nullptr, 0);
  if (wideLen <= 0) {
    return std::wstring(what.begin(), what.end());
  }
  std::wstring wide(static_cast<std::size_t>(wideLen), wchar_t{});
  MultiByteToWideChar(CP_ACP, 0, what.data(), len, wide.data(), wideLen);
  return wide;
}

}  // namespace skydiag::helper::internal

