#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace skydiag::helper::internal {

std::wstring QuoteArg(std::wstring_view s);

// CreateProcessW refuses a working directory of about MAX_PATH characters in
// either form (ERROR_DIRECTORY), so a long output folder is not handed to the
// programs started there: they get absolute paths, and an empty result keeps
// the helper's own working directory.
std::wstring WorkingDirectoryFor(const std::filesystem::path& dir);

bool RunHiddenProcessAndWait(
  const std::wstring& applicationPath,
  std::wstring cmdLine,
  const std::filesystem::path& cwd,
  DWORD timeoutMs,
  std::wstring* err);

bool RunHiddenProcessAndWait(std::wstring cmdLine, const std::filesystem::path& cwd, DWORD timeoutMs, std::wstring* err);

}  // namespace skydiag::helper::internal
