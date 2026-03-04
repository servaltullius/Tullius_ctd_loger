#pragma once

#include "I18nCore.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace skydiag::dump_tool {

struct TroubleshootingMatchInput
{
  std::uint32_t exc_code = 0;
  std::string signature_id;
  bool is_hang = false;
  bool is_loading = false;
  bool is_snapshot = false;
};

struct TroubleshootingResult
{
  std::wstring title;
  std::vector<std::wstring> steps;
};

class TroubleshootingGuideDatabase
{
public:
  TroubleshootingGuideDatabase();
  ~TroubleshootingGuideDatabase();
  TroubleshootingGuideDatabase(TroubleshootingGuideDatabase&&) noexcept;
  TroubleshootingGuideDatabase& operator=(TroubleshootingGuideDatabase&&) noexcept;

  bool LoadFromJson(const std::filesystem::path& jsonPath);
  std::optional<TroubleshootingResult> Match(const TroubleshootingMatchInput& input, i18n::Language lang) const;
  std::size_t Size() const;

private:
  struct Guide;
  std::vector<Guide> m_guides;
};

}  // namespace skydiag::dump_tool
