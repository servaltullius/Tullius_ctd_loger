#pragma once

#include <string>
#include <string_view>

namespace skydiag::dump_tool {

std::wstring Utf8ToWide(std::string_view s);
// Text that is not valid UTF-8 is read in the ANSI code page: the game and
// older captures hand over file names that way.
std::wstring Utf8OrAnsiToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view w);

}  // namespace skydiag::dump_tool

