#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "I18nCore.h"

namespace skydiag::dump_tool {

struct GraphicsEnvironment
{
  bool enb_detected = false;
  bool reshade_detected = false;
  bool dxvk_detected = false;
  std::vector<std::wstring> injection_modules;
};

struct GraphicsDiagResult
{
  std::string rule_id;
  i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
  std::wstring confidence;
  std::wstring cause;
  std::vector<std::wstring> recommendations;
};

class GraphicsInjectionDiag
{
public:
  bool LoadRules(const std::filesystem::path& jsonPath);

  GraphicsEnvironment DetectEnvironment(const std::vector<std::wstring>& moduleFilenames) const;

  std::optional<GraphicsDiagResult> Diagnose(
    const std::vector<std::wstring>& moduleFilenames,
    const std::wstring& faultModuleFilename,
    bool useKorean) const;

  std::size_t RuleCount() const;

private:
  struct DetectionGroup
  {
    std::wstring name;
    std::vector<std::wstring> dlls;
  };

  struct Rule
  {
    std::string id;
    std::vector<std::wstring> modules_any;
    std::vector<std::wstring> modules_all;
    std::vector<std::wstring> fault_module_any;
    std::wstring cause_ko;
    std::wstring cause_en;
    std::string confidence;
    std::vector<std::wstring> recommendations_ko;
    std::vector<std::wstring> recommendations_en;
  };

  std::vector<DetectionGroup> m_groups;
  std::vector<Rule> m_rules;
};

}  // namespace skydiag::dump_tool
