#include "SignatureDatabase.h"
#include "SkyrimDiagStringUtil.h"

#include <algorithm>
#include <cctype>
#include <codecvt>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <limits>
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
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::out_of_range("hex value exceeds uint32");
  }
  return static_cast<std::uint32_t>(value);
}

std::uint64_t ParseHexU64(const std::string& s)
{
  std::size_t parsed = 0;
  const auto value = std::stoull(s, &parsed, 0);
  if (parsed != s.size()) {
    throw std::invalid_argument("trailing characters in hex value");
  }
  return static_cast<std::uint64_t>(value);
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

  std::string game_version;  // empty = any

  std::wstring fault_module;      // lowercase, empty = any
  std::uint64_t fault_offset = 0;
  bool has_fault_offset = false;
  std::string fault_offset_regex;  // empty = any
  std::optional<std::regex> fault_offset_re;

  bool fault_module_is_system = false;
  bool has_fault_module_is_system = false;

  bool access_violation_address_near_zero = false;
  bool has_access_violation_address_near_zero = false;

  std::vector<std::wstring> callstack_contains;

  std::wstring cause_ko;
  std::wstring cause_en;
  std::string scope = "mechanism";
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
    const auto schemaVersion = j["version"].get<std::uint32_t>();
    if (schemaVersion != 1u && schemaVersion != 2u) {
      // Unknown match fields must never be silently ignored: that can turn a
      // future narrow signature into a broad false-positive rule.
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
      bool hasMatchConstraint = false;

      for (auto it = m.begin(); it != m.end(); ++it) {
        const std::string& key = it.key();
        if (key != "exc_code" &&
            key != "game_version" &&
            key != "fault_module" &&
            key != "fault_offset" &&
            key != "fault_offset_regex" &&
            key != "fault_module_is_system" &&
            key != "access_violation_address_near_zero" &&
            key != "exc_address_near_zero" &&
            key != "callstack_contains") {
          // A typo or newer constraint must invalidate this entry rather than
          // being ignored and broadening its match.
          valid = false;
        }
      }

      if (m.contains("exc_code")) {
        if (m["exc_code"].is_string()) {
          try {
            sig.exc_code = ParseHexU32(m["exc_code"].get<std::string>());
            sig.has_exc_code = true;
            hasMatchConstraint = true;
          } catch (...) {
            valid = false;
          }
        } else {
          valid = false;
        }
      }
      if (m.contains("game_version")) {
        if (m["game_version"].is_string() && !m["game_version"].get_ref<const std::string&>().empty()) {
          sig.game_version = m["game_version"].get<std::string>();
          hasMatchConstraint = true;
        } else {
          valid = false;
        }
      }
      if (m.contains("fault_module")) {
        if (m["fault_module"].is_string() && !m["fault_module"].get_ref<const std::string&>().empty()) {
          sig.fault_module = WideLower(Utf8ToWidePortable(m["fault_module"].get<std::string>()));
          if (sig.fault_module.empty()) {
            valid = false;
          } else {
            hasMatchConstraint = true;
          }
        } else {
          valid = false;
        }
      }
      if (m.contains("fault_offset")) {
        if (m["fault_offset"].is_string()) {
          try {
            sig.fault_offset = ParseHexU64(m["fault_offset"].get<std::string>());
            sig.has_fault_offset = true;
            hasMatchConstraint = true;
          } catch (...) {
            valid = false;
          }
        } else {
          valid = false;
        }
      }
      if (m.contains("fault_offset_regex")) {
        if (m["fault_offset_regex"].is_string() && !m["fault_offset_regex"].get_ref<const std::string&>().empty()) {
          sig.fault_offset_regex = m["fault_offset_regex"].get<std::string>();
          try {
            sig.fault_offset_re.emplace(sig.fault_offset_regex, std::regex::icase);
            hasMatchConstraint = true;
          } catch (...) {
            valid = false;
          }
        } else {
          valid = false;
        }
      }
      if (m.contains("fault_module_is_system")) {
        if (m["fault_module_is_system"].is_boolean()) {
          sig.fault_module_is_system = m["fault_module_is_system"].get<bool>();
          sig.has_fault_module_is_system = true;
          hasMatchConstraint = true;
        } else {
          valid = false;
        }
      }
      const bool hasAccessViolationNearZero = m.contains("access_violation_address_near_zero");
      const bool hasLegacyExceptionNearZero = m.contains("exc_address_near_zero");
      if (hasAccessViolationNearZero || hasLegacyExceptionNearZero) {
        if ((hasAccessViolationNearZero && !m["access_violation_address_near_zero"].is_boolean()) ||
            (hasLegacyExceptionNearZero && !m["exc_address_near_zero"].is_boolean())) {
          valid = false;
        } else {
          const bool nearZero = hasAccessViolationNearZero
            ? m["access_violation_address_near_zero"].get<bool>()
            : m["exc_address_near_zero"].get<bool>();
          if (hasAccessViolationNearZero && hasLegacyExceptionNearZero &&
              nearZero != m["exc_address_near_zero"].get<bool>()) {
            valid = false;
          } else {
            // Schema v1 called this the exception address. Treat it as the AV
            // target address so stale data cannot broaden into every AV.
            sig.access_violation_address_near_zero = nearZero;
            sig.has_access_violation_address_near_zero = true;
            hasMatchConstraint = true;
          }
        }
      }
      if (m.contains("callstack_contains")) {
        if (m["callstack_contains"].is_array()) {
          for (const auto& v : m["callstack_contains"]) {
            if (!v.is_string()) {
              valid = false;
              continue;
            }
            const std::wstring token = WideLower(Utf8ToWidePortable(v.get<std::string>()));
            if (!token.empty()) {
              sig.callstack_contains.push_back(token);
            } else {
              valid = false;
            }
          }
          if (!sig.callstack_contains.empty()) {
            hasMatchConstraint = true;
          } else {
            valid = false;
          }
        } else {
          valid = false;
        }
      }

      if (!hasMatchConstraint) {
        valid = false;
      }

      if (sig.id == "D6DDDA_1597_AV" &&
          (!sig.has_exc_code || sig.exc_code != 0xC0000005u ||
           sig.game_version != "1.5.97.0" ||
           sig.fault_module != L"skyrimse.exe" ||
           !sig.has_fault_offset || sig.fault_offset != 0xD6DDDAull)) {
        // This high-confidence location match is version/address specific. Reject older
        // broad variants instead of silently applying them to other runtimes.
        valid = false;
      }

      if (!valid) {
        continue;
      }

      const auto& d = *itDiagnosis;
      if (d.contains("scope")) {
        if (!d["scope"].is_string()) {
          continue;
        }
        sig.scope = d["scope"].get<std::string>();
      }
      if (sig.scope != "mechanism" && sig.scope != "root_cause") {
        continue;
      }
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

    if (!sig.game_version.empty() && sig.game_version != input.game_version) {
      continue;
    }

    if (!sig.fault_module.empty() && WideLower(input.fault_module) != sig.fault_module) {
      continue;
    }

    if (sig.has_fault_offset && sig.fault_offset != input.fault_offset) {
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

    if (sig.has_access_violation_address_near_zero) {
      if (!input.access_violation_address.has_value()) {
        continue;
      }
      const bool nearZero = (*input.access_violation_address <= 0x10000ull);
      if (nearZero != sig.access_violation_address_near_zero) {
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
    result.scope = sig.scope;
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
