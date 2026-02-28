#include "SignatureDatabase.h"
#include "SkyrimDiagStringUtil.h"

#include <algorithm>
#include <cctype>
#include <codecvt>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <locale>
#include <regex>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {
namespace {

using skydiag::WideLower;

bool ContainsInsensitive(std::wstring_view haystack, std::wstring_view needle)
{
  const std::wstring h = WideLower(haystack);
  const std::wstring n = WideLower(needle);
  return !n.empty() && h.find(n) != std::wstring::npos;
}

i18n::ConfidenceLevel ParseConfidence(std::string_view s)
{
  std::string lower(s);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "high") {
    return i18n::ConfidenceLevel::kHigh;
  }
  if (lower == "medium") {
    return i18n::ConfidenceLevel::kMedium;
  }
  if (lower == "low") {
    return i18n::ConfidenceLevel::kLow;
  }
  return i18n::ConfidenceLevel::kUnknown;
}

std::uint32_t ParseHexU32(const std::string& s)
{
  std::size_t parsed = 0;
  const auto value = std::stoul(s, &parsed, 0);
  if (parsed != s.size()) {
    throw std::invalid_argument("trailing characters in hex value");
  }
  return static_cast<std::uint32_t>(value);
}

std::wstring Utf8ToWidePortable(const std::string& s)
{
  if (s.empty()) {
    return {};
  }
  try {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    return conv.from_bytes(s);
  } catch (...) {
    // Fallback keeps ASCII diagnostics readable even when conversion fails.
    std::wstring out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
      out.push_back(static_cast<wchar_t>(c));
    }
    return out;
  }
}

}  // namespace

struct SignatureDatabase::Signature
{
  std::string id;

  std::uint32_t exc_code = 0;
  bool has_exc_code = false;

  std::wstring fault_module;      // lowercase, empty = any
  std::string fault_offset_regex;  // empty = any
  std::optional<std::regex> fault_offset_re;

  bool fault_module_is_system = false;
  bool has_fault_module_is_system = false;

  bool exc_address_near_zero = false;
  bool has_exc_address_near_zero = false;

  std::vector<std::wstring> callstack_contains;

  std::wstring cause_ko;
  std::wstring cause_en;
  i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
  std::vector<std::wstring> recommendations_ko;
  std::vector<std::wstring> recommendations_en;
};

SignatureDatabase::SignatureDatabase() = default;
SignatureDatabase::~SignatureDatabase() = default;
SignatureDatabase::SignatureDatabase(SignatureDatabase&&) noexcept = default;
SignatureDatabase& SignatureDatabase::operator=(SignatureDatabase&&) noexcept = default;

std::size_t SignatureDatabase::Size() const
{
  return m_signatures.size();
}

bool SignatureDatabase::LoadFromJson(const std::filesystem::path& jsonPath)
{
  try {
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
      return false;
    }
    const auto j = nlohmann::json::parse(f, nullptr, true);
    if (!j.is_object() || !j.contains("signatures") || !j["signatures"].is_array()) {
      return false;
    }
    if (!j.contains("version") || !j["version"].is_number_unsigned()) {
      return false;
    }

    std::vector<Signature> loaded;
    loaded.reserve(j["signatures"].size());

    for (const auto& s : j["signatures"]) {
      if (!s.is_object()) {
        continue;
      }

      Signature sig{};
      sig.id = s.value("id", "");
      if (sig.id.empty()) {
        continue;
      }

      const auto itMatch = s.find("match");
      const auto itDiagnosis = s.find("diagnosis");
      if (itMatch == s.end() || !itMatch->is_object() ||
          itDiagnosis == s.end() || !itDiagnosis->is_object()) {
        continue;
      }
      const auto& m = *itMatch;
      bool valid = true;

      if (m.contains("exc_code") && m["exc_code"].is_string()) {
        try {
          sig.exc_code = ParseHexU32(m["exc_code"].get<std::string>());
          sig.has_exc_code = true;
        } catch (...) {
          valid = false;
        }
      }
      if (m.contains("fault_module") && m["fault_module"].is_string()) {
        sig.fault_module = WideLower(Utf8ToWidePortable(m["fault_module"].get<std::string>()));
      }
      if (m.contains("fault_offset_regex") && m["fault_offset_regex"].is_string()) {
        sig.fault_offset_regex = m["fault_offset_regex"].get<std::string>();
        if (!sig.fault_offset_regex.empty()) {
          try {
            sig.fault_offset_re.emplace(sig.fault_offset_regex, std::regex::icase);
          } catch (...) {
            valid = false;
          }
        }
      }
      if (m.contains("fault_module_is_system") && m["fault_module_is_system"].is_boolean()) {
        sig.fault_module_is_system = m["fault_module_is_system"].get<bool>();
        sig.has_fault_module_is_system = true;
      }
      if (m.contains("exc_address_near_zero") && m["exc_address_near_zero"].is_boolean()) {
        sig.exc_address_near_zero = m["exc_address_near_zero"].get<bool>();
        sig.has_exc_address_near_zero = true;
      }
      if (m.contains("callstack_contains") && m["callstack_contains"].is_array()) {
        for (const auto& v : m["callstack_contains"]) {
          if (v.is_string()) {
            const std::wstring token = WideLower(Utf8ToWidePortable(v.get<std::string>()));
            if (!token.empty()) {
              sig.callstack_contains.push_back(token);
            }
          }
        }
      }

      if (!valid) {
        continue;
      }

      const auto& d = *itDiagnosis;
      sig.cause_ko = Utf8ToWidePortable(d.value("cause_ko", ""));
      sig.cause_en = Utf8ToWidePortable(d.value("cause_en", ""));
      sig.confidence_level = ParseConfidence(d.value("confidence", ""));

      if (d.contains("recommendations_ko") && d["recommendations_ko"].is_array()) {
        for (const auto& r : d["recommendations_ko"]) {
          if (r.is_string()) {
            sig.recommendations_ko.push_back(Utf8ToWidePortable(r.get<std::string>()));
          }
        }
      }
      if (d.contains("recommendations_en") && d["recommendations_en"].is_array()) {
        for (const auto& r : d["recommendations_en"]) {
          if (r.is_string()) {
            sig.recommendations_en.push_back(Utf8ToWidePortable(r.get<std::string>()));
          }
        }
      }

      loaded.push_back(std::move(sig));
    }

    m_signatures = std::move(loaded);
    return !m_signatures.empty();
  } catch (...) {
    return false;
  }
}

std::optional<SignatureMatch> SignatureDatabase::Match(const SignatureMatchInput& input, bool useKorean) const
{
  for (const auto& sig : m_signatures) {
    if (sig.has_exc_code && sig.exc_code != input.exc_code) {
      continue;
    }

    if (!sig.fault_module.empty() && WideLower(input.fault_module) != sig.fault_module) {
      continue;
    }

    if (sig.fault_offset_re) {
      char offsetHex[32]{};
      std::snprintf(offsetHex, sizeof(offsetHex), "%llX", static_cast<unsigned long long>(input.fault_offset));
      if (!std::regex_search(offsetHex, *sig.fault_offset_re)) {
        continue;
      }
    }

    if (sig.has_fault_module_is_system && sig.fault_module_is_system != input.fault_module_is_system) {
      continue;
    }

    if (sig.has_exc_address_near_zero) {
      const bool nearZero = (input.exc_address <= 0x10000ull);
      if (nearZero != sig.exc_address_near_zero) {
        continue;
      }
    }

    if (!sig.callstack_contains.empty()) {
      bool allMatched = true;
      for (const auto& token : sig.callstack_contains) {
        bool foundThisToken = false;
        for (const auto& m : input.callstack_modules) {
          if (ContainsInsensitive(m, token)) {
            foundThisToken = true;
            break;
          }
        }
        if (!foundThisToken) {
          allMatched = false;
          break;
        }
      }
      if (!allMatched) {
        continue;
      }
    }

    SignatureMatch result{};
    result.id = sig.id;
    result.cause = useKorean ? sig.cause_ko : sig.cause_en;
    result.confidence_level = sig.confidence_level;
    result.confidence = std::wstring(i18n::ConfidenceLabel(
      useKorean ? i18n::Language::kKorean : i18n::Language::kEnglish,
      sig.confidence_level));
    result.recommendations = useKorean ? sig.recommendations_ko : sig.recommendations_en;
    return result;
  }
  return std::nullopt;
}

}  // namespace skydiag::dump_tool
