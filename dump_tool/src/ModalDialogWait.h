#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Analyzer.h"
#include "WctTypes.h"

namespace skydiag::dump_tool {

// One symbolized frame of the game main thread, top (innermost) first.
struct ModalStackFrame
{
  bool has_module = false;
  std::wstring module_filename;
  std::wstring module_path;
  std::wstring inferred_mod_name;
  std::wstring symbol;  // symbol name without module prefix; empty when unresolved
  std::uint64_t displacement = 0;
  bool is_system = false;
  bool is_game_exe = false;
  bool is_skse_runtime = false;
  bool is_hook_framework = false;
  std::uint64_t pc = 0;
  // Code bytes just before pc (the call this frame made), read from the dump's
  // own memory only; empty when the dump does not hold them.
  std::vector<std::uint8_t> code_before;
};

struct ModalStackMatch
{
  bool matched = false;
  std::size_t api_frame_index = 0;
  std::wstring wait_api;  // e.g. "user32.dll!MessageBoxW"
  bool has_caller = false;
  std::size_t caller_frame_index = 0;
};

// The modal entry point must sit near the top of the main thread.
inline constexpr std::size_t kModalApiMaxDepth = 24;
// Export-only symbols name any address after an export by that export. Small
// displacements keep a neighbouring non-modal function from matching.
inline constexpr std::uint64_t kModalApiMaxDisplacement = 0x800;

bool IsModalDialogApiFrame(const ModalStackFrame& frame);

// Matches a pure modal-wait chain: every frame above the outermost modal API
// is a system module, so no plugin code is running on top of the wait.
ModalStackMatch MatchModalDialogWaitStack(const std::vector<ModalStackFrame>& frames);

// plugin / skse_runtime / hook_framework / game_exe
std::string ClassifyModalDialogCaller(const ModalStackFrame& frame);

// Recognizes the Address Library failures that CommonLibSSE(-NG) plugins report
// through a message box: plugin_incompatible (unsupported format, missing id),
// address_library_missing (no database file for this game version), or empty.
std::string ClassifyAddressLibraryDialogText(std::wstring_view text);

struct ModalDialogWaitInput
{
  std::uint32_t main_thread_id = 0;
  const internal::WctFreezeSummary* wct = nullptr;
  const std::vector<ModalStackFrame>* main_thread_frames = nullptr;
};

ModalDialogWaitInfo ResolveModalDialogWait(const ModalDialogWaitInput& input);

}  // namespace skydiag::dump_tool
