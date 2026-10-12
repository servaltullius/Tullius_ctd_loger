#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Analyzer.h"
#include "MinidumpUtil.h"

namespace skydiag::dump_tool {

// A DLL that, merely by being loaded, makes crash evidence unreliable: it
// intercepts exceptions and tries to continue, so the game runs on in a broken
// state and the real cause is hidden or turns into a later crash or freeze.
// Matched by exact file name, as Crash Logger's ProblematicModules.cpp does.
struct ProblematicModuleRule
{
  std::wstring_view filename;  // lower case
  std::wstring_view name;
  std::wstring_view reason_en;
  std::wstring_view reason_ko;
  std::wstring_view link;
};

const ProblematicModuleRule* FindProblematicModuleRule(std::wstring_view filename);

std::vector<ProblematicModule> FindProblematicModules(const std::vector<minidump::ModuleInfo>& modules);

// "SkyrimCrashGuard.dll (SkyrimCrashGuard, mod: Skyrim Crash Guard)"
std::wstring DescribeProblematicModule(const ProblematicModule& module, bool en);

}  // namespace skydiag::dump_tool
