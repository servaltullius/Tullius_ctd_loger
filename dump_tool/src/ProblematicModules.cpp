#include "ProblematicModules.h"

namespace skydiag::dump_tool {
namespace {

constexpr ProblematicModuleRule kRules[] = {
  {
    L"skyrimcrashguard.dll",
    L"SkyrimCrashGuard",
    L"it catches crashes and tries to keep the game running, which can corrupt game state, hide the real cause or "
    L"turn it into a later crash or freeze; its exception handler can also keep this logger and Crash Logger from "
    L"seeing a crash at all",
    L"크래시를 가로채 게임을 계속 돌리려 해서 게임 상태를 망가뜨릴 수 있고, 원래 원인을 숨기거나 나중의 다른 "
    L"크래시·프리징으로 바꿀 수 있습니다. 자체 예외 처리기 때문에 이 로거와 Crash Logger가 크래시를 아예 못 잡을 "
    L"수도 있습니다",
    L"https://www.nexusmods.com/skyrimspecialedition/mods/172082",
  },
};

}  // namespace

const ProblematicModuleRule* FindProblematicModuleRule(std::wstring_view filename)
{
  const std::wstring lower = minidump::WideLower(filename);
  for (const auto& rule : kRules) {
    if (lower == rule.filename) {
      return &rule;
    }
  }
  return nullptr;
}

std::vector<ProblematicModule> FindProblematicModules(const std::vector<minidump::ModuleInfo>& modules)
{
  std::vector<ProblematicModule> found;
  for (const auto& module : modules) {
    const auto* rule = FindProblematicModuleRule(module.filename);
    if (!rule) {
      continue;
    }
    bool seen = false;
    for (const auto& existing : found) {
      seen = seen || existing.name == rule->name;
    }
    if (seen) {
      continue;
    }
    ProblematicModule hit{};
    hit.filename = module.filename;
    hit.name = rule->name;
    hit.mod_name = module.inferred_mod_name;
    found.push_back(std::move(hit));
  }
  return found;
}

std::wstring DescribeProblematicModule(const ProblematicModule& module, bool en)
{
  std::wstring text = module.filename + L" (" + module.name;
  if (!module.mod_name.empty() && module.mod_name != module.name) {
    text += (en ? L", mod: " : L", 모드: ") + module.mod_name;
  }
  text += L")";
  return text;
}

}  // namespace skydiag::dump_tool
