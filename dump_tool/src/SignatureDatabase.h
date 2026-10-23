#pragma once

#include "I18nCore.h"

#include <cstdint>
#include <filesystem>
#include <functional>
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
  // Optional quick-answer lines of a known freeze (the generic known-issue
  // lines otherwise).
  std::wstring headline;
  std::wstring why_not_certain;
  // The fix's DLL, and whether this dump had it loaded (then the
  // recommendations are the fix-loaded ones).
  std::wstring fix_module;
  bool fix_module_loaded = false;
  // ADR-0021: the threads a signature's game_lock_threads matched, in its
  // order.
  std::vector<std::uint32_t> game_lock_tids;
};

// ADR-0021: a game thread waiting in a delay under game code, with its frames
// from the one that called the sleep down (module + offset, innermost first).
struct SignatureGameLockThread
{
  std::uint32_t tid = 0;
  std::vector<std::pair<std::wstring, std::uint64_t>> frames;
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
  // Crash Logger's C++ exception message ("invalid stoull argument").
  std::wstring cpp_exception_info;
  // Object types near the crash: Crash Logger's POSSIBLE RELEVANT OBJECTS and
  // REGISTERS, and the dump objects ("InstalledContent", "Character").
  std::vector<std::wstring> object_types;
  // A freeze: its state (FreezeAnalysisResult::state_id, e.g. engine_spin_nan)
  // and the engine loop it spun in. A freeze input matches only signatures
  // with a freeze_state, and those match nothing else.
  std::string freeze_state;
  std::wstring freeze_loop_module;
  std::uint64_t freeze_loop_offset = 0;
  // File names of the modules loaded in the dump.
  std::vector<std::wstring> loaded_modules;
  // ADR-0021, freezes only: the game threads waiting in a delay, the threads
  // of a wait-for cycle on engine locks (empty without one), and a reader of
  // the two 32-bit words at a game-executable RVA from the game's writable
  // data the dump holds (false when it does not hold them).
  std::vector<SignatureGameLockThread> game_lock_threads;
  std::vector<std::uint32_t> game_lock_cycle_tids;
  std::function<bool(std::uint64_t rva, std::uint32_t& word0, std::uint32_t& word1)> read_game_lock;
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
