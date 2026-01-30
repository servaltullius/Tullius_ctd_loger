#pragma once

#include <string>
#include <string_view>

namespace skydiag::dump_tool {

std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view w);

}  // namespace skydiag::dump_tool

