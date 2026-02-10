#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace skydiag::helper::internal {

std::wstring QuoteArg(std::wstring_view s);

bool RunHiddenProcessAndWait(std::wstring cmdLine, const std::filesystem::path& cwd, DWORD timeoutMs, std::wstring* err);

}  // namespace skydiag::helper::internal

