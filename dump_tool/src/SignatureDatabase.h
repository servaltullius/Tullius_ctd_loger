#pragma once

#include "I18nCore.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace skydiag::dump_tool {

struct SignatureMatch
{
  std::string id;  // e.g. D6DDDA_VRAM
  std::wstring cause;
  i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
  std::wstring confidence;  // localized confidence label
  std::vector<std::wstring> recommendations;
};

struct SignatureMatchInput
{
  std::uint32_t exc_code = 0;
  std::wstring fault_module;
  std::uint64_t fault_offset = 0;
  std::uint64_t exc_address = 0;
  bool fault_module_is_system = false;
  std::vector<std::wstring> callstack_modules;
};

class SignatureDatabase
{
public:
  SignatureDatabase();
  ~SignatureDatabase();
  SignatureDatabase(SignatureDatabase&&) noexcept;
  SignatureDatabase& operator=(SignatureDatabase&&) noexcept;
  SignatureDatabase(const SignatureDatabase&) = delete;
  SignatureDatabase& operator=(const SignatureDatabase&) = delete;

  bool LoadFromJson(const std::filesystem::path& jsonPath);
  std::optional<SignatureMatch> Match(const SignatureMatchInput& input, bool useKorean) const;
  std::size_t Size() const;

private:
  struct Signature;
  std::vector<Signature> m_signatures;
};

}  // namespace skydiag::dump_tool
