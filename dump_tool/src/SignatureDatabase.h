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
  std::string id;  // e.g. D6DDDA_1597_AV
  std::string scope = "mechanism";  // mechanism / root_cause
  std::wstring cause;  // localized mechanism or root-cause description
  i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
  std::wstring confidence;  // localized match-confidence label
  std::vector<std::wstring> recommendations;
};

struct SignatureMatchInput
{
  std::uint32_t exc_code = 0;
  std::string game_version;  // exact executable file version; empty = unavailable
  std::wstring fault_module;
  std::uint64_t fault_offset = 0;
  // MINIDUMP_EXCEPTION.ExceptionInformation[1]; nullopt when the dump did not provide it.
  std::optional<std::uint64_t> access_violation_address;
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
