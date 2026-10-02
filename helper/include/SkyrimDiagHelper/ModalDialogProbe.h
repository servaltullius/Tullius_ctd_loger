#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace skydiag::helper {

// A visible standard dialog window (#32770: MessageBox, DialogBox, TaskDialog)
// owned by the target process at hang-capture time.
struct ModalDialogWindow
{
  std::uint32_t tid = 0;      // thread that owns (and pumps) the dialog
  std::string title;          // UTF-8, bounded
  std::string text;           // UTF-8, bounded; joined static-control text
  bool has_owner = false;
  bool owner_disabled = false;  // a disabled owner window indicates a modal loop
};

inline constexpr std::size_t kModalDialogMaxCount = 8;
inline constexpr std::size_t kModalDialogMaxTitleChars = 256;
inline constexpr std::size_t kModalDialogMaxTextChars = 1024;

// Reads window state only: it never sends messages to the target, so a hung
// target cannot block the helper.
std::vector<ModalDialogWindow> CaptureModalDialogs(std::uint32_t pid);

nlohmann::json ModalDialogsToJson(const std::vector<ModalDialogWindow>& dialogs);

}  // namespace skydiag::helper
