#pragma once

#include <string>

#include "I18nCore.h"

namespace skydiag::dump_tool {

struct DumpToolConfig {
  i18n::Language language = i18n::DefaultLanguage();
};

std::wstring DumpToolIniPath();
DumpToolConfig LoadDumpToolConfig(std::wstring* err);
bool SaveDumpToolConfig(const DumpToolConfig& cfg, std::wstring* err);

}  // namespace skydiag::dump_tool

