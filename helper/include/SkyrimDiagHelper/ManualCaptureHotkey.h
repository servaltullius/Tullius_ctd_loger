#pragma once

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::helper {

// RegisterHotKey's MOD_ALT / MOD_CONTROL / MOD_SHIFT and the virtual-key codes
// used here, spelled out so this header builds without <Windows.h>.
inline constexpr std::uint32_t kHotkeyModAlt = 0x0001u;
inline constexpr std::uint32_t kHotkeyModControl = 0x0002u;
inline constexpr std::uint32_t kHotkeyModShift = 0x0004u;

inline constexpr std::uint32_t kVkBack = 0x08u;
inline constexpr std::uint32_t kVkShift = 0x10u;
inline constexpr std::uint32_t kVkControl = 0x11u;
inline constexpr std::uint32_t kVkAlt = 0x12u;  // VK_MENU
inline constexpr std::uint32_t kVkF1 = 0x70u;
inline constexpr std::uint32_t kVkF12 = 0x7Bu;
inline constexpr std::uint32_t kVkF24 = 0x87u;

struct ManualCaptureHotkey
{
  std::uint32_t modifiers = 0;   // kHotkeyMod* bits
  std::uint32_t virtualKey = 0;  // the one non-modifier key

  friend constexpr bool operator==(const ManualCaptureHotkey&, const ManualCaptureHotkey&) = default;
};

// The default manual capture hotkey, Ctrl+Shift+Backspace.
//
// It was Ctrl+Shift+F12 up to v0.2.68-rc3. Crash Logger 1.25 takes the same
// keys for its thread dump: it polls GetAsyncKeyState every 50 ms and fires
// while all of its keys are down, so any chord holding them all fires it too,
// and RegisterHotKey cannot keep the keys from it. The thread dump suspends
// the game's threads one at a time, and on 2026-10-18 it deadlocked the game
// (one suspended thread held the loader lock its own LoadLibrary needed).
// Chords left out for the new default:
// - Ctrl+Shift+F12 and anything holding all three (Crash Logger);
// - Ctrl+Shift+F10 / F11 (SkyrimDiag.ini EnableTestHotkeys: intentional crash
//   and hang) and Ctrl+Shift+F1..F11 (the development CrashTester plugin),
//   which also fire on any chord holding Ctrl, Shift and their key;
// - Shift+F12, Shift+F4, Shift+Enter (ENB), Home (ReShade), End (Community
//   Shaders), F12 alone (Steam screenshot), ~ (game console);
// - Ctrl+Alt+anything (Intel graphics hotkeys; under Proton, Linux VT
//   switching) and Alt+anything (NVIDIA and AMD overlays, Alt+F11 Radeon
//   Chill, Alt+Shift keyboard layout switching);
// - Ctrl+Shift+letters (AMD Adrenalin, Discord) and Space or digits, which
//   are pressed in play while Ctrl (sneak) or Shift (run) is held.
// Backspace is on every keyboard without Fn and has no gameplay binding.
inline constexpr ManualCaptureHotkey kDefaultManualCaptureHotkey{ kHotkeyModControl | kHotkeyModShift, kVkBack };

// The manual capture hotkey up to v0.2.68-rc3, Ctrl+Shift+F12, the one the
// v0.2.67 README and Nexus page name.
inline constexpr ManualCaptureHotkey kLegacyManualCaptureHotkey{ kHotkeyModControl | kHotkeyModShift, kVkF12 };

// The old chord to watch, so that a press of it out of habit is logged as not
// taking a snapshot (LegacyManualCaptureHotkeyPressedMessage): only while the
// manual capture hotkey is on and set to another chord. A configured chord on
// F12 too (e.g. Ctrl+Shift+Alt+F12) passes through the old one while it is
// pressed, so its real snapshot would also be logged as not taken.
inline std::optional<ManualCaptureHotkey> LegacyManualCaptureChordToWatch(
  bool manualCaptureEnabled,
  const ManualCaptureHotkey& configured)
{
  if (!manualCaptureEnabled || configured.virtualKey == kLegacyManualCaptureHotkey.virtualKey) {
    return std::nullopt;
  }
  return kLegacyManualCaptureHotkey;
}

namespace manual_capture_hotkey_detail {

struct KeyName
{
  std::wstring_view name;
  std::uint32_t virtualKey;
};

// Named keys besides F1-F24, A-Z and 0-9. The first name of a key is the one
// FormatManualCaptureHotkey prints.
inline constexpr KeyName kKeyNames[] = {
  { L"Backspace", kVkBack },
  { L"Insert", 0x2Du },
  { L"Delete", 0x2Eu },
  { L"Del", 0x2Eu },
  { L"Home", 0x24u },
  { L"End", 0x23u },
  { L"PageUp", 0x21u },
  { L"PageDown", 0x22u },
};

constexpr wchar_t AsciiLower(wchar_t c) noexcept
{
  return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

inline bool EqualsAsciiInsensitive(std::wstring_view a, std::wstring_view b) noexcept
{
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) { return AsciiLower(x) == AsciiLower(y); });
}

inline bool EqualsAsciiInsensitive(std::string_view a, std::string_view b) noexcept
{
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return AsciiLower(static_cast<wchar_t>(static_cast<unsigned char>(x))) ==
                  AsciiLower(static_cast<wchar_t>(static_cast<unsigned char>(y)));
         });
}

template <class CharT>
std::basic_string_view<CharT> TrimSpacesAndTabs(std::basic_string_view<CharT> s) noexcept
{
  const auto isBlank = [](CharT c) { return c == CharT(' ') || c == CharT('\t'); };
  while (!s.empty() && isBlank(s.front())) {
    s.remove_prefix(1);
  }
  while (!s.empty() && isBlank(s.back())) {
    s.remove_suffix(1);
  }
  return s;
}

inline std::optional<std::uint32_t> ParseKeyName(std::wstring_view token) noexcept
{
  if (token.size() == 1) {
    const wchar_t c = AsciiLower(token[0]);
    if (c >= L'a' && c <= L'z') {
      return static_cast<std::uint32_t>(c - L'a') + 0x41u;
    }
    if (c >= L'0' && c <= L'9') {
      return static_cast<std::uint32_t>(c - L'0') + 0x30u;
    }
    return std::nullopt;
  }
  if ((token[0] == L'F' || token[0] == L'f') && token.size() <= 3) {
    std::uint32_t n = 0;
    for (std::size_t i = 1; i < token.size(); ++i) {
      if (token[i] < L'0' || token[i] > L'9') {
        return std::nullopt;
      }
      n = n * 10u + static_cast<std::uint32_t>(token[i] - L'0');
    }
    if (n >= 1u && n <= 24u && token[1] != L'0') {
      return kVkF1 + n - 1u;
    }
    return std::nullopt;
  }
  for (const auto& entry : kKeyNames) {
    if (EqualsAsciiInsensitive(token, entry.name)) {
      return entry.virtualKey;
    }
  }
  return std::nullopt;
}

// The name of a non-modifier key, or an empty string when it has none here.
inline std::wstring KeyNameOf(std::uint32_t virtualKey)
{
  if ((virtualKey >= 0x41u && virtualKey <= 0x5Au) || (virtualKey >= 0x30u && virtualKey <= 0x39u)) {
    return std::wstring(1, static_cast<wchar_t>(virtualKey));
  }
  if (virtualKey >= kVkF1 && virtualKey <= kVkF24) {
    return L"F" + std::to_wstring(virtualKey - kVkF1 + 1u);
  }
  for (const auto& entry : kKeyNames) {
    if (entry.virtualKey == virtualKey) {
      return std::wstring(entry.name);
    }
  }
  return {};
}

}  // namespace manual_capture_hotkey_detail

// Parses "Ctrl+Shift+Backspace" style text: Ctrl (or Control), Shift and Alt
// in any order and case, and one key: F1-F24, A-Z, 0-9, Backspace, Insert,
// Delete, Home, End, PageUp or PageDown. At least one modifier is required: a
// bare key would be taken from every program while the helper runs and would
// start a capture on each ordinary press. For the same reason Shift alone does
// not go with A-Z or 0-9 (Shift+P types 'P', Shift+1 types '!'), and Alt+F4,
// which closes the window in front, is not taken. Returns nullopt for anything
// else.
inline std::optional<ManualCaptureHotkey> ParseManualCaptureHotkey(std::wstring_view text)
{
  using namespace manual_capture_hotkey_detail;
  ManualCaptureHotkey hotkey{};
  bool haveKey = false;
  for (;;) {
    const std::size_t plus = text.find(L'+');
    const std::wstring_view token = TrimSpacesAndTabs(text.substr(0, plus));
    if (token.empty()) {
      return std::nullopt;
    }
    std::uint32_t modifier = 0;
    if (EqualsAsciiInsensitive(token, L"Ctrl") || EqualsAsciiInsensitive(token, L"Control")) {
      modifier = kHotkeyModControl;
    } else if (EqualsAsciiInsensitive(token, L"Shift")) {
      modifier = kHotkeyModShift;
    } else if (EqualsAsciiInsensitive(token, L"Alt")) {
      modifier = kHotkeyModAlt;
    }
    if (modifier != 0) {
      if ((hotkey.modifiers & modifier) != 0) {
        return std::nullopt;
      }
      hotkey.modifiers |= modifier;
    } else {
      const auto key = ParseKeyName(token);
      if (!key || haveKey) {
        return std::nullopt;
      }
      hotkey.virtualKey = *key;
      haveKey = true;
    }
    if (plus == std::wstring_view::npos) {
      break;
    }
    text.remove_prefix(plus + 1);
  }
  if (!haveKey || hotkey.modifiers == 0) {
    return std::nullopt;
  }
  constexpr std::uint32_t kVkF4 = kVkF1 + 3u;
  const bool typesCharacter = (hotkey.virtualKey >= 0x41u && hotkey.virtualKey <= 0x5Au) ||
                              (hotkey.virtualKey >= 0x30u && hotkey.virtualKey <= 0x39u);
  if ((hotkey.modifiers == kHotkeyModShift && typesCharacter) ||
      (hotkey.modifiers == kHotkeyModAlt && hotkey.virtualKey == kVkF4)) {
    return std::nullopt;
  }
  return hotkey;
}

// "Ctrl+Shift+Backspace": modifiers in Ctrl, Shift, Alt order, then the key.
inline std::wstring FormatManualCaptureHotkey(const ManualCaptureHotkey& hotkey)
{
  std::wstring out;
  if ((hotkey.modifiers & kHotkeyModControl) != 0) {
    out += L"Ctrl+";
  }
  if ((hotkey.modifiers & kHotkeyModShift) != 0) {
    out += L"Shift+";
  }
  if ((hotkey.modifiers & kHotkeyModAlt) != 0) {
    out += L"Alt+";
  }
  std::wstring key = manual_capture_hotkey_detail::KeyNameOf(hotkey.virtualKey);
  out += key.empty() ? L"VK " + std::to_wstring(hotkey.virtualKey) : key;
  return out;
}

// True while exactly the hotkey is held: its key and modifiers down and none
// of the other of Ctrl, Shift and Alt, the way RegisterHotKey matches it.
// keyDown(virtualKey) tells whether a key is down now.
template <class KeyDown>
bool IsManualCaptureHotkeyDown(const ManualCaptureHotkey& hotkey, KeyDown&& keyDown)
{
  const auto modifierMatches = [&](std::uint32_t modifier, std::uint32_t virtualKey) {
    return static_cast<bool>(keyDown(virtualKey)) == ((hotkey.modifiers & modifier) != 0);
  };
  return static_cast<bool>(keyDown(hotkey.virtualKey)) && modifierMatches(kHotkeyModControl, kVkControl) &&
         modifierMatches(kHotkeyModShift, kVkShift) && modifierMatches(kHotkeyModAlt, kVkAlt);
}

// Crash Logger's thread dump hotkey as CrashLogger.ini sets it under [Debug]
// ("Enable Thread Dump Hotkey", "Thread Dump Hotkey"), with Crash Logger
// 1.25's defaults: enabled, keys 17, 16, 123 (Ctrl+Shift+F12).
struct CrashLoggerThreadDumpHotkey
{
  bool enabled = true;
  std::vector<int> keys{ 17, 16, 123 };
};

namespace manual_capture_hotkey_detail {

// SimpleIni's GetBoolValue: the first letters decide, anything else keeps
// the default.
inline std::optional<bool> ParseSimpleIniBool(std::string_view value) noexcept
{
  if (value.empty()) {
    return std::nullopt;
  }
  switch (value[0]) {
    case 't': case 'T': case 'y': case 'Y': case '1':
      return true;
    case 'f': case 'F': case 'n': case 'N': case '0':
      return false;
    case 'o': case 'O':
      if (value.size() > 1 && (value[1] == 'n' || value[1] == 'N')) {
        return true;
      }
      if (value.size() > 1 && (value[1] == 'f' || value[1] == 'F')) {
        return false;
      }
      break;
    default:
      break;
  }
  return std::nullopt;
}

// std::stoi: optional sign and decimal digits after leading white space,
// stopping at the first other character; nothing read or out of range is
// an error (Crash Logger skips that entry).
inline std::optional<int> ParseStoiInt(std::string_view token) noexcept
{
  std::size_t i = 0;
  while (i < token.size() &&
         (token[i] == ' ' || token[i] == '\t' || token[i] == '\n' || token[i] == '\v' || token[i] == '\f' ||
          token[i] == '\r')) {
    ++i;
  }
  bool negative = false;
  if (i < token.size() && (token[i] == '+' || token[i] == '-')) {
    negative = token[i] == '-';
    ++i;
  }
  long long value = 0;
  bool anyDigit = false;
  for (; i < token.size() && token[i] >= '0' && token[i] <= '9'; ++i) {
    value = value * 10 + (token[i] - '0');
    anyDigit = true;
    if (value > static_cast<long long>(INT_MAX) + 1) {
      return std::nullopt;
    }
  }
  if (!anyDigit) {
    return std::nullopt;
  }
  value = negative ? -value : value;
  if (value > INT_MAX || value < INT_MIN) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

inline bool HotkeyHoldsKey(const ManualCaptureHotkey& hotkey, int virtualKey) noexcept
{
  if (virtualKey >= 0 && static_cast<std::uint32_t>(virtualKey) == hotkey.virtualKey) {
    return true;
  }
  switch (virtualKey) {
    case 0x10: case 0xA0: case 0xA1:  // Shift, left / right Shift
      return (hotkey.modifiers & kHotkeyModShift) != 0;
    case 0x11: case 0xA2: case 0xA3:  // Ctrl, left / right Ctrl
      return (hotkey.modifiers & kHotkeyModControl) != 0;
    case 0x12: case 0xA4: case 0xA5:  // Alt, left / right Alt
      return (hotkey.modifiers & kHotkeyModAlt) != 0;
    default:
      return false;
  }
}

// Calls fn(key, value) for each key = value line under [Debug] in
// CrashLogger.ini text, in file order, read the way Crash Logger 1.25 reads it
// (SimpleIni, UTF-8): section names ignore case and surrounding blanks, ';'
// and '#' start comment lines, and keys and values lose surrounding blanks.
template <class Fn>
void ForEachCrashLoggerDebugEntry(std::string_view iniText, Fn&& fn)
{
  if (iniText.size() >= 3 && iniText.substr(0, 3) == "\xEF\xBB\xBF") {
    iniText.remove_prefix(3);
  }
  bool inDebug = false;
  while (!iniText.empty()) {
    const std::size_t eol = iniText.find('\n');
    std::string_view line = iniText.substr(0, eol);
    iniText.remove_prefix(eol == std::string_view::npos ? iniText.size() : eol + 1);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    line = TrimSpacesAndTabs(line);
    if (line.empty() || line.front() == ';' || line.front() == '#') {
      continue;
    }
    if (line.front() == '[') {
      const std::size_t close = line.find(']');
      if (close != std::string_view::npos) {
        inDebug = EqualsAsciiInsensitive(TrimSpacesAndTabs(line.substr(1, close - 1)), std::string_view("Debug"));
      }
      continue;
    }
    const std::size_t eq = line.find('=');
    if (!inDebug || eq == std::string_view::npos) {
      continue;
    }
    fn(TrimSpacesAndTabs(line.substr(0, eq)), TrimSpacesAndTabs(line.substr(eq + 1)));
  }
}

}  // namespace manual_capture_hotkey_detail

// Reads CrashLogger.ini text the way Crash Logger 1.25 does (SimpleIni,
// UTF-8): section and key names ignore case and surrounding blanks, the last
// of duplicate keys wins, ';' and '#' start comment lines, and the hotkey is
// a comma-separated list of decimal virtual-key codes, each read like
// std::stoi. An entry that does not read is skipped, and an empty or wholly
// unreadable list keeps the default keys.
inline CrashLoggerThreadDumpHotkey ParseCrashLoggerThreadDumpHotkey(std::string_view iniText)
{
  using namespace manual_capture_hotkey_detail;
  CrashLoggerThreadDumpHotkey out;
  ForEachCrashLoggerDebugEntry(iniText, [&](std::string_view key, std::string_view value) {
    if (EqualsAsciiInsensitive(key, std::string_view("Enable Thread Dump Hotkey"))) {
      out.enabled = ParseSimpleIniBool(value).value_or(true);
    } else if (EqualsAsciiInsensitive(key, std::string_view("Thread Dump Hotkey"))) {
      std::vector<int> keys;
      std::string_view rest = value;
      while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        if (const auto vk = ParseStoiInt(TrimSpacesAndTabs(rest.substr(0, comma)))) {
          keys.push_back(*vk);
        }
        rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 1);
      }
      out.keys = keys.empty() ? CrashLoggerThreadDumpHotkey{}.keys : keys;
    }
  });
  return out;
}

// CrashLogger.ini's "Crashlog Directory" under [Debug], read like the hotkey
// above (a pair of surrounding double quotes is dropped): the folder Crash
// Logger writes its crash and thread dump logs to, relative to the game's
// folder unless absolute. Empty when unset: Crash Logger then writes them to
// the SKSE log folder.
inline std::string ParseCrashLoggerCrashlogDirectory(std::string_view iniText)
{
  using namespace manual_capture_hotkey_detail;
  std::string out;
  ForEachCrashLoggerDebugEntry(iniText, [&](std::string_view key, std::string_view value) {
    if (EqualsAsciiInsensitive(key, std::string_view("Crashlog Directory"))) {
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = TrimSpacesAndTabs(value.substr(1, value.size() - 2));
      }
      out.assign(value);
    }
  });
  return out;
}

// Crash Logger fires its thread dump while all of its keys are down, so it
// fires on the manual capture hotkey when the hotkey holds every one of them
// (a left or right modifier key counting as that modifier).
inline bool CrashLoggerThreadDumpFiresOnHotkey(
  const CrashLoggerThreadDumpHotkey& crashLogger,
  const ManualCaptureHotkey& hotkey) noexcept
{
  return crashLogger.enabled && !crashLogger.keys.empty() &&
         std::all_of(crashLogger.keys.begin(), crashLogger.keys.end(), [&](int vk) {
           return manual_capture_hotkey_detail::HotkeyHoldsKey(hotkey, vk);
         });
}

// True while Crash Logger's thread dump hotkey is held: it is enabled and all
// of its keys are down, extra keys or not, the way Crash Logger matches them.
// keyDown(virtualKey) tells whether a key is down now; a code outside 1-255
// is never down (GetAsyncKeyState).
template <class KeyDown>
bool IsCrashLoggerThreadDumpHotkeyDown(const CrashLoggerThreadDumpHotkey& crashLogger, KeyDown&& keyDown)
{
  return crashLogger.enabled && !crashLogger.keys.empty() &&
         std::all_of(crashLogger.keys.begin(), crashLogger.keys.end(), [&](int vk) {
           return vk > 0 && vk <= 0xFF && static_cast<bool>(keyDown(static_cast<std::uint32_t>(vk)));
         });
}

// "Ctrl+Shift+F12" for Crash Logger's virtual-key list.
inline std::wstring FormatVirtualKeyList(const std::vector<int>& keys)
{
  std::wstring out;
  static constexpr manual_capture_hotkey_detail::KeyName kModifierNames[] = {
    { L"Shift", 0x10u }, { L"Ctrl", 0x11u }, { L"Alt", 0x12u },
    { L"LShift", 0xA0u }, { L"RShift", 0xA1u }, { L"LCtrl", 0xA2u },
    { L"RCtrl", 0xA3u }, { L"LAlt", 0xA4u }, { L"RAlt", 0xA5u },
  };
  for (const int vk : keys) {
    if (!out.empty()) {
      out += L'+';
    }
    std::wstring name;
    if (vk > 0) {
      const auto key = static_cast<std::uint32_t>(vk);
      const auto* modifier = std::find_if(std::begin(kModifierNames), std::end(kModifierNames), [&](const auto& m) {
        return m.virtualKey == key;
      });
      name = modifier != std::end(kModifierNames) ? std::wstring(modifier->name)
                                                  : manual_capture_hotkey_detail::KeyNameOf(key);
    }
    out += name.empty() ? L"VK " + std::to_wstring(vk) : name;
  }
  return out;
}

// Crash Logger (CrashLogger.dll, or CrashLoggerSSE.dll) in the game's
// Data\SKSE\Plugins folder, its thread dump hotkey from CrashLogger.ini there
// (the file it reads), and whether that fires on the manual capture hotkey.
// Also the Crashlog Directory set there (UTF-8, empty when unset).
struct CrashLoggerHotkeyCheck
{
  bool crashLoggerFound = false;
  CrashLoggerThreadDumpHotkey threadDump;
  bool firesOnManualCapture = false;
  std::string crashlogDirectory;
};

inline CrashLoggerHotkeyCheck CheckCrashLoggerThreadDumpHotkey(
  const std::filesystem::path& sksePluginsDir,
  const ManualCaptureHotkey& hotkey)
{
  CrashLoggerHotkeyCheck out;
  std::error_code ec;
  out.crashLoggerFound = std::filesystem::exists(sksePluginsDir / L"CrashLogger.dll", ec) ||
                         std::filesystem::exists(sksePluginsDir / L"CrashLoggerSSE.dll", ec);
  if (!out.crashLoggerFound) {
    return out;
  }
  std::ifstream in(sksePluginsDir / L"CrashLogger.ini", std::ios::in | std::ios::binary);
  if (in) {
    std::ostringstream text;
    text << in.rdbuf();
    out.threadDump = ParseCrashLoggerThreadDumpHotkey(text.str());
    out.crashlogDirectory = ParseCrashLoggerCrashlogDirectory(text.str());
  }
  out.firesOnManualCapture = CrashLoggerThreadDumpFiresOnHotkey(out.threadDump, hotkey);
  return out;
}

// The warning for a Crash Logger thread dump hotkey that fires on the manual
// capture hotkey.
inline std::wstring CrashLoggerHotkeyCollisionWarning(
  const CrashLoggerHotkeyCheck& check,
  const ManualCaptureHotkey& hotkey)
{
  return L"Crash Logger's thread dump hotkey (" + FormatVirtualKeyList(check.threadDump.keys) +
         L") also fires on the manual capture hotkey (" + FormatManualCaptureHotkey(hotkey) +
         L"). Its thread dump suspends the game's threads one at a time and can freeze the game; set "
         L"ManualCaptureHotkey in SkyrimDiagHelper.ini to another chord, or set Enable Thread Dump Hotkey = false "
         L"under [Debug] in CrashLogger.ini.";
}

// The helper log line for a press of the old Ctrl+Shift+F12 while the manual
// capture hotkey is set to another chord, configured.
inline std::wstring LegacyManualCaptureHotkeyPressedMessage(const ManualCaptureHotkey& configured)
{
  return FormatManualCaptureHotkey(kLegacyManualCaptureHotkey) +
         L" was pressed, but it is no longer the manual capture hotkey (now " + FormatManualCaptureHotkey(configured) +
         L"); no snapshot was taken. With Crash Logger's thread dump hotkey on these keys, the press starts Crash "
         L"Logger's thread dump, which suspends the game's threads one at a time and can freeze the game.";
}

}  // namespace skydiag::helper
