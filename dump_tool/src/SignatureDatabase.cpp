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

#include "SkyrimDiagGameLocks.h"

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

std::vector<std::wstring> ReadStringArray(const nlohmann::json& obj, const char* key)
{
  std::vector<std::wstring> out;
  const auto it = obj.find(key);
  if (it != obj.end() && it->is_array()) {
    for (const auto& v : *it) {
      if (v.is_string() && !v.get_ref<const std::string&>().empty()) {
        out.push_back(Utf8ToWidePortable(v.get<std::string>()));
      }
    }
  }
  return out;
}

using GameFrame = std::pair<std::wstring, std::uint64_t>;

// "SkyrimSE.exe+0x132C5A": the module (lowercased) and the offset.
bool ParseGameFrame(const nlohmann::json& v, GameFrame& out)
{
  if (!v.is_string()) {
    return false;
  }
  const auto& text = v.get_ref<const std::string&>();
  const auto plus = text.rfind('+');
  if (plus == std::string::npos || plus == 0 || plus + 1 >= text.size()) {
    return false;
  }
  try {
    out.first = WideLower(Utf8ToWidePortable(text.substr(0, plus)));
    out.second = ParseHexU64(text.substr(plus + 1));
  } catch (...) {
    return false;
  }
  return !out.first.empty();
}

struct GameLockConstraint
{
  std::uint64_t rva = 0;
  bool rw = false;
  int owner_thread = -1;  // index into game_lock_threads; -1 for a lock held for read
};

// The thread's frames start at the first anchor, and the other anchors follow
// in order further down (other frames may sit between them).
bool GameLockAnchorsMatch(const std::vector<GameFrame>& anchors, const SignatureGameLockThread& thread)
{
  const auto same = [](const GameFrame& anchor, const GameFrame& frame) {
    return anchor.second == frame.second && anchor.first == WideLower(frame.first);
  };
  if (anchors.empty() || thread.frames.empty() || !same(anchors.front(), thread.frames.front())) {
    return false;
  }
  std::size_t at = 1;
  for (std::size_t i = 1; i < anchors.size(); ++i) {
    while (at < thread.frames.size() && !same(anchors[i], thread.frames[at])) {
      ++at;
    }
    if (at >= thread.frames.size()) {
      return false;
    }
    ++at;
  }
  return true;
}

// Every lock reads from the dump as the signature says; a lock the dump does
// not hold matches nothing.
bool GameLocksHold(
  const std::vector<GameLockConstraint>& locks,
  const std::vector<std::uint32_t>& tids,
  const SignatureMatchInput& input)
{
  for (const auto& lock : locks) {
    std::uint32_t word0 = 0;
    std::uint32_t word1 = 0;
    if (!input.read_game_lock || !input.read_game_lock(lock.rva, word0, word1)) {
      return false;
    }
    if (lock.owner_thread < 0) {
      if (!skydiag::game_locks::RwReaders(word0, word1)) {
        return false;
      }
      continue;
    }
    const auto held = skydiag::game_locks::DecodeHeldLock(word0, word1);
    const auto want = lock.rw ? skydiag::game_locks::HeldKind::kRwWrite : skydiag::game_locks::HeldKind::kSpin;
    if (held.kind != want || held.owner != tids[static_cast<std::size_t>(lock.owner_thread)]) {
      return false;
    }
  }
  return true;
}

// Distinct input threads for the signature's threads (in its order) that
// match their anchors, hold the locks and, when asked, are all in the cycle.
bool AssignGameLockThreads(
  const std::vector<std::vector<GameFrame>>& anchors,
  const std::vector<GameLockConstraint>& locks,
  bool inCycle,
  const SignatureMatchInput& input,
  std::vector<std::uint32_t>& tids)
{
  if (tids.size() == anchors.size()) {
    if (inCycle && !std::all_of(tids.begin(), tids.end(), [&](std::uint32_t tid) {
          return std::find(input.game_lock_cycle_tids.begin(), input.game_lock_cycle_tids.end(), tid) !=
                 input.game_lock_cycle_tids.end();
        })) {
      return false;
    }
    return GameLocksHold(locks, tids, input);
  }
  const auto& want = anchors[tids.size()];
  for (const auto& thread : input.game_lock_threads) {
    if (thread.tid == 0u || std::find(tids.begin(), tids.end(), thread.tid) != tids.end() ||
        !GameLockAnchorsMatch(want, thread)) {
      continue;
    }
    tids.push_back(thread.tid);
    if (AssignGameLockThreads(anchors, locks, inCycle, input, tids)) {
      return true;
    }
    tids.pop_back();
  }
  return false;
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

  // The exact address the access violation read or wrote (a known null
  // dereference reads one fixed offset from the null pointer).
  std::uint64_t access_violation_address = 0;
  bool has_access_violation_address = false;

  std::vector<std::wstring> callstack_contains;

  std::wstring cpp_exception_info_contains;  // lowercase, empty = any
  std::vector<std::wstring> object_type_any;  // lowercase, empty = any

  std::string freeze_state;           // empty = a crash signature
  std::wstring freeze_loop_module;    // lowercase, empty = any
  std::uint64_t freeze_loop_offset = 0;
  bool has_freeze_loop_offset = false;

  // ADR-0021: game threads waiting in a delay, each matched by ordered frame
  // anchors (the first is the frame that called the sleep, the rest follow
  // in order further down), on distinct threads; engine locks at RVAs, held
  // by one of those threads (spin, or read/write for write) or held for read;
  // and whether those threads must be in the wait-for cycle.
  std::vector<std::vector<GameFrame>> game_lock_threads;
  std::vector<GameLockConstraint> game_locks;
  bool game_lock_cycle = false;

  std::wstring cause_ko;
  std::wstring cause_en;
  std::string scope = "mechanism";
  i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
  std::vector<std::wstring> recommendations_ko;
  std::vector<std::wstring> recommendations_en;
  std::wstring headline_ko;
  std::wstring headline_en;
  std::wstring why_not_certain_ko;
  std::wstring why_not_certain_en;
  std::wstring fix_module;  // as written, e.g. "TMS_AIPathingFreezeFix.dll"
  std::vector<std::wstring> recommendations_fix_loaded_ko;
  std::vector<std::wstring> recommendations_fix_loaded_en;
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
            key != "access_violation_address" &&
            key != "exc_address_near_zero" &&
            key != "callstack_contains" &&
            key != "cpp_exception_info_contains" &&
            key != "object_type_any" &&
            key != "freeze_state" &&
            key != "freeze_loop_module" &&
            key != "freeze_loop_offset" &&
            key != "game_lock_threads" &&
            key != "game_locks" &&
            key != "game_lock_cycle") {
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
      if (m.contains("access_violation_address")) {
        if (m["access_violation_address"].is_string()) {
          try {
            sig.access_violation_address = ParseHexU64(m["access_violation_address"].get<std::string>());
            sig.has_access_violation_address = true;
            hasMatchConstraint = true;
          } catch (...) {
            valid = false;
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

      if (m.contains("cpp_exception_info_contains")) {
        const auto& v = m["cpp_exception_info_contains"];
        sig.cpp_exception_info_contains =
          v.is_string() ? WideLower(Utf8ToWidePortable(v.get<std::string>())) : std::wstring{};
        if (sig.cpp_exception_info_contains.empty()) {
          valid = false;
        } else {
          hasMatchConstraint = true;
        }
      }
      if (m.contains("object_type_any")) {
        const auto& v = m["object_type_any"];
        if (v.is_array()) {
          for (const auto& type : v) {
            const std::wstring token =
              type.is_string() ? WideLower(Utf8ToWidePortable(type.get<std::string>())) : std::wstring{};
            if (token.empty()) {
              valid = false;
            } else {
              sig.object_type_any.push_back(token);
            }
          }
        }
        if (sig.object_type_any.empty()) {
          valid = false;
        } else {
          hasMatchConstraint = true;
        }
      }
      if (m.contains("freeze_state")) {
        if (m["freeze_state"].is_string() && !m["freeze_state"].get_ref<const std::string&>().empty()) {
          sig.freeze_state = m["freeze_state"].get<std::string>();
          hasMatchConstraint = true;
        } else {
          valid = false;
        }
      }
      if (m.contains("freeze_loop_module")) {
        if (m["freeze_loop_module"].is_string()) {
          sig.freeze_loop_module = WideLower(Utf8ToWidePortable(m["freeze_loop_module"].get<std::string>()));
        }
        if (sig.freeze_loop_module.empty()) {
          valid = false;
        }
      }
      if (m.contains("freeze_loop_offset")) {
        if (m["freeze_loop_offset"].is_string()) {
          try {
            sig.freeze_loop_offset = ParseHexU64(m["freeze_loop_offset"].get<std::string>());
            sig.has_freeze_loop_offset = true;
          } catch (...) {
            valid = false;
          }
        } else {
          valid = false;
        }
      }
      if (m.contains("game_lock_threads")) {
        const auto& v = m["game_lock_threads"];
        constexpr std::size_t kMaxGameLockThreads = 4;
        if (!v.is_array() || v.empty() || v.size() > kMaxGameLockThreads) {
          valid = false;
        } else {
          for (const auto& thread : v) {
            const auto frames = thread.is_object() ? thread.find("frames") : thread.end();
            if (!thread.is_object() || thread.size() != 1u || frames == thread.end() || !frames->is_array() ||
                frames->empty()) {
              valid = false;
              break;
            }
            std::vector<GameFrame> anchors;
            for (const auto& frame : *frames) {
              GameFrame anchor;
              if (!ParseGameFrame(frame, anchor)) {
                valid = false;
                break;
              }
              anchors.push_back(std::move(anchor));
            }
            sig.game_lock_threads.push_back(std::move(anchors));
          }
          hasMatchConstraint = true;
        }
      }
      if (m.contains("game_locks")) {
        const auto& v = m["game_locks"];
        if (!v.is_array() || v.empty()) {
          valid = false;
        } else {
          for (const auto& lock : v) {
            if (!lock.is_object()) {
              valid = false;
              break;
            }
            for (auto it = lock.begin(); it != lock.end(); ++it) {
              if (it.key() != "rva" && it.key() != "kind" && it.key() != "owner_thread" && it.key() != "state") {
                valid = false;
              }
            }
            GameLockConstraint parsed{};
            const auto rva = lock.find("rva");
            const auto kind = lock.find("kind");
            const auto owner = lock.find("owner_thread");
            const auto state = lock.find("state");
            if (rva == lock.end() || !rva->is_string() || kind == lock.end() || !kind->is_string()) {
              valid = false;
              break;
            }
            try {
              parsed.rva = ParseHexU64(rva->get<std::string>());
            } catch (...) {
              valid = false;
              break;
            }
            const auto& kindText = kind->get_ref<const std::string&>();
            parsed.rw = kindText == "rw";
            if (kindText != "spin" && !parsed.rw) {
              valid = false;
              break;
            }
            const bool hasOwner = owner != lock.end();
            const bool readHeld = state != lock.end();
            if (hasOwner) {
              if (!owner->is_number_unsigned() ||
                  owner->get<std::uint64_t>() >= static_cast<std::uint64_t>(sig.game_lock_threads.size())) {
                valid = false;
                break;
              }
              parsed.owner_thread = static_cast<int>(owner->get<std::uint64_t>());
            }
            // A spin lock is held by a thread; a read/write lock either by a
            // writer thread or for read (which names no thread).
            if (readHeld && (!parsed.rw || hasOwner || !state->is_string() || state->get<std::string>() != "read")) {
              valid = false;
              break;
            }
            if (!hasOwner && !readHeld) {
              valid = false;
              break;
            }
            sig.game_locks.push_back(parsed);
          }
          hasMatchConstraint = true;
        }
      }
      if (m.contains("game_lock_cycle")) {
        if (m["game_lock_cycle"].is_boolean() && m["game_lock_cycle"].get<bool>()) {
          sig.game_lock_cycle = true;
          hasMatchConstraint = true;
        } else {
          valid = false;
        }
      }
      // Engine-lock facts describe threads of one freeze of one game version:
      // locks and the cycle need the threads, and the threads need a freeze
      // state and the version their anchors are RVAs of.
      if ((!sig.game_locks.empty() || sig.game_lock_cycle) && sig.game_lock_threads.empty()) {
        valid = false;
      }
      if (!sig.game_lock_threads.empty() && (sig.freeze_state.empty() || sig.game_version.empty())) {
        valid = false;
      }

      // The loop is an exact instruction of one module, and only a freeze has
      // one: both parts, and a freeze state, or the entry is dropped.
      if ((!sig.freeze_loop_module.empty() || sig.has_freeze_loop_offset) &&
          (sig.freeze_loop_module.empty() || !sig.has_freeze_loop_offset || sig.freeze_state.empty())) {
        valid = false;
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
      sig.headline_ko = Utf8ToWidePortable(d.value("headline_ko", ""));
      sig.headline_en = Utf8ToWidePortable(d.value("headline_en", ""));
      sig.why_not_certain_ko = Utf8ToWidePortable(d.value("why_not_certain_ko", ""));
      sig.why_not_certain_en = Utf8ToWidePortable(d.value("why_not_certain_en", ""));
      if (d.contains("fix_module")) {
        // A fix that is already loaded must not be recommended again, so the
        // entry needs the advice for that case.
        sig.fix_module = d["fix_module"].is_string() ? Utf8ToWidePortable(d["fix_module"].get<std::string>())
                                                     : std::wstring{};
        sig.recommendations_fix_loaded_ko = ReadStringArray(d, "recommendations_fix_loaded_ko");
        sig.recommendations_fix_loaded_en = ReadStringArray(d, "recommendations_fix_loaded_en");
        if (sig.fix_module.empty() || sig.recommendations_fix_loaded_ko.empty() ||
            sig.recommendations_fix_loaded_en.empty()) {
          continue;
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
    // Crash and freeze signatures never match each other's input.
    if (sig.freeze_state != input.freeze_state) {
      continue;
    }

    if (!sig.freeze_loop_module.empty() &&
        (WideLower(input.freeze_loop_module) != sig.freeze_loop_module ||
         input.freeze_loop_offset != sig.freeze_loop_offset)) {
      continue;
    }

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

    if (sig.has_access_violation_address &&
        (!input.access_violation_address || *input.access_violation_address != sig.access_violation_address)) {
      continue;
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

    if (!sig.cpp_exception_info_contains.empty() &&
        !ContainsInsensitive(input.cpp_exception_info, sig.cpp_exception_info_contains)) {
      continue;
    }

    if (!sig.object_type_any.empty()) {
      // Exact type names: "Actor" must not match "ActorValueOwner".
      const bool anyType = std::any_of(input.object_types.begin(), input.object_types.end(), [&](const std::wstring& t) {
        return std::find(sig.object_type_any.begin(), sig.object_type_any.end(), WideLower(t)) !=
               sig.object_type_any.end();
      });
      if (!anyType) {
        continue;
      }
    }

    std::vector<std::uint32_t> gameLockTids;
    if (!sig.game_lock_threads.empty() &&
        !AssignGameLockThreads(sig.game_lock_threads, sig.game_locks, sig.game_lock_cycle, input, gameLockTids)) {
      continue;
    }

    SignatureMatch result{};
    result.id = sig.id;
    result.game_lock_tids = std::move(gameLockTids);
    result.scope = sig.scope;
    result.cause = useKorean ? sig.cause_ko : sig.cause_en;
    result.confidence_level = sig.confidence_level;
    result.confidence = std::wstring(i18n::ConfidenceLabel(
      useKorean ? i18n::Language::kKorean : i18n::Language::kEnglish,
      sig.confidence_level));
    result.recommendations = useKorean ? sig.recommendations_ko : sig.recommendations_en;
    result.headline = useKorean ? sig.headline_ko : sig.headline_en;
    result.why_not_certain = useKorean ? sig.why_not_certain_ko : sig.why_not_certain_en;
    if (!sig.fix_module.empty()) {
      const std::wstring fix = WideLower(sig.fix_module);
      result.fix_module = sig.fix_module;
      result.fix_module_loaded =
        std::any_of(input.loaded_modules.begin(), input.loaded_modules.end(), [&](const std::wstring& m) {
          return WideLower(m) == fix;
        });
      if (result.fix_module_loaded) {
        result.recommendations = useKorean ? sig.recommendations_fix_loaded_ko : sig.recommendations_fix_loaded_en;
      }
    }
    return result;
  }
  return std::nullopt;
}

}  // namespace skydiag::dump_tool
