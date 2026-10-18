// ManualCaptureHotkey parsing and formatting, and the check for Crash Logger's
// thread dump hotkey firing on the manual capture hotkey (CrashLogger.ini read
// the way Crash Logger 1.25 reads it).

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "SkyrimDiagHelper/ManualCaptureHotkey.h"

using skydiag::helper::CheckCrashLoggerThreadDumpHotkey;
using skydiag::helper::CrashLoggerHotkeyCollisionWarning;
using skydiag::helper::CrashLoggerThreadDumpFiresOnHotkey;
using skydiag::helper::CrashLoggerThreadDumpHotkey;
using skydiag::helper::FormatManualCaptureHotkey;
using skydiag::helper::FormatVirtualKeyList;
using skydiag::helper::kDefaultManualCaptureHotkey;
using skydiag::helper::kHotkeyModAlt;
using skydiag::helper::kHotkeyModControl;
using skydiag::helper::kHotkeyModShift;
using skydiag::helper::kLegacyManualCaptureHotkey;
using skydiag::helper::LegacyManualCaptureChordToWatch;
using skydiag::helper::LegacyManualCaptureHotkeyPressedMessage;
using skydiag::helper::ManualCaptureHotkey;
using skydiag::helper::ParseCrashLoggerThreadDumpHotkey;
using skydiag::helper::ParseManualCaptureHotkey;

namespace {

ManualCaptureHotkey Hotkey(std::wstring_view text)
{
  const auto hotkey = ParseManualCaptureHotkey(text);
  assert(hotkey && "fixture hotkey must parse");
  return *hotkey;
}

const ManualCaptureHotkey kOldCtrlShiftF12{ kHotkeyModControl | kHotkeyModShift, 0x7Bu };

void TestParseAcceptsHotkeys()
{
  assert(Hotkey(L"Ctrl+Shift+Backspace") == kDefaultManualCaptureHotkey);
  assert(Hotkey(L"  shift + CTRL +backspace\t") == kDefaultManualCaptureHotkey);
  assert(Hotkey(L"Ctrl+Shift+F12") == kOldCtrlShiftF12);

  const auto ctrlAltF11 = Hotkey(L"Control+Alt+F11");
  assert(ctrlAltF11.modifiers == (kHotkeyModControl | kHotkeyModAlt));
  assert(ctrlAltF11.virtualKey == 0x7Au);

  assert(Hotkey(L"Alt+a").virtualKey == 0x41u);
  assert(Hotkey(L"Ctrl+0").virtualKey == 0x30u);
  assert(Hotkey(L"Ctrl+F1").virtualKey == 0x70u);
  assert(Hotkey(L"Ctrl+F24").virtualKey == 0x87u);
  assert(Hotkey(L"Ctrl+Shift+Del") == Hotkey(L"Ctrl+Shift+Delete"));
  assert(Hotkey(L"Shift+Insert").virtualKey == 0x2Du);
  assert(Hotkey(L"Shift+PageDown").virtualKey == 0x22u);
  // Letters and digits need Ctrl or Alt besides Shift; F4 goes with more than Alt.
  assert(Hotkey(L"Shift+F9").virtualKey == 0x78u);
  assert(Hotkey(L"Ctrl+Shift+P").virtualKey == 0x50u);
  assert(Hotkey(L"Alt+Shift+1").virtualKey == 0x31u);
  assert(Hotkey(L"Ctrl+Alt+F4").virtualKey == 0x73u);
  assert(Hotkey(L"Shift+F4").virtualKey == 0x73u);
}

void TestParseRejectsOtherText()
{
  for (const std::wstring_view text : {
         L"",
         L"F11",                    // no modifier: a bare key would be taken from every program
         L"Ctrl+Shift",             // no key
         L"Ctrl+Shift+F11+F12",     // two keys
         L"Ctrl+Ctrl+F11",          // repeated modifier
         L"Ctrl++F11",
         L"Ctrl+Shift+",
         L"Ctrl+F0",
         L"Ctrl+F01",
         L"Ctrl+F25",
         L"Win+F11",
         L"Ctrl+Shift+Enter",
         L"Ctrl+Shift+F11 ; note",
         L"17,16,123",
         L"Shift+P",                // types 'P' in every program, like a bare key
         L"shift+z",
         L"Shift+1",                // types '!'
         L"Shift+0",
         L"Alt+F4",                 // closes the window in front
         L"alt+f4",
       }) {
    assert(!ParseManualCaptureHotkey(text));
  }
}

void TestFormat()
{
  assert(FormatManualCaptureHotkey(kDefaultManualCaptureHotkey) == L"Ctrl+Shift+Backspace");
  assert(FormatManualCaptureHotkey(Hotkey(L"alt+shift+ctrl+f11")) == L"Ctrl+Shift+Alt+F11");
  assert(FormatManualCaptureHotkey(Hotkey(L"Ctrl+Del")) == L"Ctrl+Delete");
  assert(FormatManualCaptureHotkey(Hotkey(L"Alt+z")) == L"Alt+Z");
  for (const std::wstring_view text : { L"Ctrl+Shift+F12", L"Shift+PageUp", L"Alt+9", L"Ctrl+Home", L"Ctrl+End" }) {
    assert(FormatManualCaptureHotkey(Hotkey(text)) == text);
  }

  assert(FormatVirtualKeyList({ 17, 16, 123 }) == L"Ctrl+Shift+F12");
  assert(FormatVirtualKeyList({ 162, 18, 8 }) == L"LCtrl+Alt+Backspace");
  assert(FormatVirtualKeyList({ 0, 300 }) == L"VK 0+VK 300");
}

// The old Ctrl+Shift+F12 is watched only while the manual capture hotkey is on
// and set to another chord; a press of it is logged naming the configured one.
void TestLegacyManualCaptureChord()
{
  assert(kLegacyManualCaptureHotkey == kOldCtrlShiftF12);
  assert(FormatManualCaptureHotkey(kLegacyManualCaptureHotkey) == L"Ctrl+Shift+F12");

  assert(LegacyManualCaptureChordToWatch(true, kDefaultManualCaptureHotkey) == kLegacyManualCaptureHotkey);
  assert(LegacyManualCaptureChordToWatch(true, Hotkey(L"Shift+F9")) == kLegacyManualCaptureHotkey);
  // A chord that also holds Alt is another chord: the exact old one is still watched.
  assert(LegacyManualCaptureChordToWatch(true, Hotkey(L"Ctrl+Shift+Alt+F12")) == kLegacyManualCaptureHotkey);
  // Still set to the old chord: it is the manual capture hotkey itself.
  assert(!LegacyManualCaptureChordToWatch(true, kOldCtrlShiftF12));
  // Manual capture hotkey off: nothing to point the press at.
  assert(!LegacyManualCaptureChordToWatch(false, kDefaultManualCaptureHotkey));
  assert(!LegacyManualCaptureChordToWatch(false, kOldCtrlShiftF12));

  const std::wstring message = LegacyManualCaptureHotkeyPressedMessage(kDefaultManualCaptureHotkey);
  assert(message.rfind(L"Ctrl+Shift+F12 was pressed", 0) == 0);
  assert(message.find(L"no longer the manual capture hotkey (now Ctrl+Shift+Backspace)") != std::wstring::npos);
  assert(message.find(L"no snapshot was taken") != std::wstring::npos);
  assert(message.find(L"Crash Logger's thread dump") != std::wstring::npos);
  assert(message.find(L"can freeze the game") != std::wstring::npos);
  assert(LegacyManualCaptureHotkeyPressedMessage(Hotkey(L"Shift+F9")).find(L"(now Shift+F9)") != std::wstring::npos);
}

void TestCrashLoggerIniDefaults()
{
  const auto empty = ParseCrashLoggerThreadDumpHotkey("");
  assert(empty.enabled);
  assert((empty.keys == std::vector<int>{ 17, 16, 123 }));

  // The file Crash Logger 1.25 writes on its first run: UTF-8 BOM, comment
  // lines, the hotkey left empty.
  const auto written = ParseCrashLoggerThreadDumpHotkey(
    "\xEF\xBB\xBF; ==========\r\n"
    "; Crash Log Settings\r\n"
    "\r\n"
    "[Debug]\r\n"
    ";Enable thread dump hotkey for diagnosing hangs/deadlocks. Default: true\r\n"
    "Enable Thread Dump Hotkey = true\r\n"
    ";Hotkey combination (VK codes): Ctrl=17, Shift=16, F12=123. Default: 17, 16, 123\r\n"
    "Thread Dump Hotkey = \r\n"
    "Log Level = 2\r\n");
  assert(written.enabled);
  assert((written.keys == std::vector<int>{ 17, 16, 123 }));
}

void TestCrashLoggerIniEnableFlag()
{
  struct Case
  {
    const char* value;
    bool enabled;
  };
  for (const Case& c : {
         Case{ "false", false }, Case{ "False", false }, Case{ "0", false }, Case{ "no", false }, Case{ "off", false },
         Case{ "true", true }, Case{ "1", true }, Case{ "yes", true }, Case{ "On", true },
         Case{ "", true }, Case{ "maybe", true }, Case{ "o", true },
       }) {
    const std::string ini = std::string("[Debug]\nEnable Thread Dump Hotkey = ") + c.value + "\n";
    assert(ParseCrashLoggerThreadDumpHotkey(ini).enabled == c.enabled);
  }
}

void TestCrashLoggerIniHotkeyList()
{
  assert((ParseCrashLoggerThreadDumpHotkey("[Debug]\nThread Dump Hotkey = 17, 16, 122\n").keys ==
          std::vector<int>{ 17, 16, 122 }));
  // std::stoi per entry: unreadable entries are skipped, a leading number is
  // read up to the first other character ("0x7A" is 0).
  assert((ParseCrashLoggerThreadDumpHotkey("[Debug]\nThread Dump Hotkey = 17,abc, 0x7A ,122junk,,99999999999\n").keys ==
          std::vector<int>{ 17, 0, 122 }));
  // Nothing readable keeps the default.
  assert((ParseCrashLoggerThreadDumpHotkey("[Debug]\nThread Dump Hotkey = abc, ,\n").keys ==
          std::vector<int>{ 17, 16, 123 }));
  // Section and key names ignore case and surrounding blanks; the last
  // duplicate wins.
  assert((ParseCrashLoggerThreadDumpHotkey("[ debug ]\nThread Dump Hotkey = 16, 8\n").keys ==
          std::vector<int>{ 16, 8 }));
  assert((ParseCrashLoggerThreadDumpHotkey(
            "[Debug]\nThread Dump Hotkey=18,123\n[ debug ]\nTHREAD DUMP HOTKEY = 16, 123\n").keys ==
          std::vector<int>{ 16, 123 }));
  assert(!ParseCrashLoggerThreadDumpHotkey("[DEBUG]\nenable thread dump hotkey = false\n").enabled);
  // A UTF-8 BOM right before the section header.
  assert((ParseCrashLoggerThreadDumpHotkey("\xEF\xBB\xBF[Debug]\r\nThread Dump Hotkey = 16, 8\r\n").keys ==
          std::vector<int>{ 16, 8 }));
  // Other sections and commented-out lines do not count.
  const auto other = ParseCrashLoggerThreadDumpHotkey(
    "[Other]\nEnable Thread Dump Hotkey = false\nThread Dump Hotkey = 1\n"
    "[Debug]\n;Enable Thread Dump Hotkey = false\n#Thread Dump Hotkey = 2\n");
  assert(other.enabled);
  assert((other.keys == std::vector<int>{ 17, 16, 123 }));
}

void TestCrashLoggerFiresOnHotkey()
{
  const CrashLoggerThreadDumpHotkey defaults;
  assert(CrashLoggerThreadDumpFiresOnHotkey(defaults, kOldCtrlShiftF12));
  assert(!CrashLoggerThreadDumpFiresOnHotkey(defaults, kDefaultManualCaptureHotkey));
  // Crash Logger fires while all of its keys are down, extra keys or not.
  assert(CrashLoggerThreadDumpFiresOnHotkey(defaults, Hotkey(L"Ctrl+Shift+Alt+F12")));
  assert(!CrashLoggerThreadDumpFiresOnHotkey(defaults, Hotkey(L"Ctrl+Alt+F12")));

  CrashLoggerThreadDumpHotkey disabled;
  disabled.enabled = false;
  assert(!CrashLoggerThreadDumpFiresOnHotkey(disabled, kOldCtrlShiftF12));

  CrashLoggerThreadDumpHotkey leftCtrl;
  leftCtrl.keys = { 162, 16, 123 };
  assert(CrashLoggerThreadDumpFiresOnHotkey(leftCtrl, kOldCtrlShiftF12));

  // Left / right Shift, Ctrl and Alt count as that modifier.
  struct SideCase
  {
    int modifierKey;
    const wchar_t* holding;
    const wchar_t* notHolding;
  };
  for (const SideCase& c : {
         SideCase{ 160, L"Ctrl+Shift+Backspace", L"Ctrl+Alt+Backspace" },
         SideCase{ 161, L"Shift+Alt+Backspace", L"Ctrl+Backspace" },
         SideCase{ 162, L"Ctrl+Backspace", L"Shift+Alt+Backspace" },
         SideCase{ 163, L"Ctrl+Alt+Backspace", L"Shift+Backspace" },
         SideCase{ 164, L"Ctrl+Alt+Backspace", L"Ctrl+Shift+Backspace" },
         SideCase{ 165, L"Alt+Backspace", L"Ctrl+Shift+Backspace" },
       }) {
    CrashLoggerThreadDumpHotkey side;
    side.keys = { c.modifierKey, 8 };
    assert(CrashLoggerThreadDumpFiresOnHotkey(side, Hotkey(c.holding)));
    assert(!CrashLoggerThreadDumpFiresOnHotkey(side, Hotkey(c.notHolding)));
  }

  CrashLoggerThreadDumpHotkey f11;
  f11.keys = { 17, 16, 122 };
  assert(!CrashLoggerThreadDumpFiresOnHotkey(f11, kOldCtrlShiftF12));
  assert(CrashLoggerThreadDumpFiresOnHotkey(f11, Hotkey(L"Ctrl+Shift+F11")));

  CrashLoggerThreadDumpHotkey f12Only;
  f12Only.keys = { 123 };
  assert(CrashLoggerThreadDumpFiresOnHotkey(f12Only, kOldCtrlShiftF12));

  CrashLoggerThreadDumpHotkey neverDown;
  neverDown.keys = { 0 };
  assert(!CrashLoggerThreadDumpFiresOnHotkey(neverDown, kOldCtrlShiftF12));
}

// A Data\SKSE\Plugins folder holding Crash Logger and, optionally, its ini.
class PluginsDirFixture
{
public:
  PluginsDirFixture()
  {
    static int s_count = 0;
    dir_ = std::filesystem::temp_directory_path() /
           ("skydiag_crashlogger_hotkey_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(++s_count));
    std::filesystem::create_directories(dir_);
  }
  ~PluginsDirFixture()
  {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  PluginsDirFixture(const PluginsDirFixture&) = delete;
  PluginsDirFixture& operator=(const PluginsDirFixture&) = delete;

  const std::filesystem::path& Path() const { return dir_; }

  void Write(const char* name, std::string_view text) const
  {
    std::ofstream out(dir_ / name, std::ios::out | std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
  }

private:
  std::filesystem::path dir_;
};

constexpr std::string_view kIniEnabledDefault =
  "[Debug]\r\nEnable Thread Dump Hotkey = true\r\nThread Dump Hotkey = \r\n";

void TestPreflightCheckFixtures()
{
  {
    // No Crash Logger: nothing to warn about, whatever the hotkey.
    PluginsDirFixture plugins;
    plugins.Write("CrashLogger.ini", kIniEnabledDefault);
    const auto check = CheckCrashLoggerThreadDumpHotkey(plugins.Path(), kOldCtrlShiftF12);
    assert(!check.crashLoggerFound);
    assert(!check.firesOnManualCapture);
  }
  {
    // Default / enabled ini and a manual capture hotkey holding Ctrl+Shift+F12: warn.
    PluginsDirFixture plugins;
    plugins.Write("CrashLogger.dll", "");
    plugins.Write("CrashLogger.ini", kIniEnabledDefault);
    const auto check = CheckCrashLoggerThreadDumpHotkey(plugins.Path(), kOldCtrlShiftF12);
    assert(check.crashLoggerFound);
    assert(check.firesOnManualCapture);
    const std::wstring warning = CrashLoggerHotkeyCollisionWarning(check, kOldCtrlShiftF12);
    assert(warning.find(L"thread dump hotkey (Ctrl+Shift+F12)") != std::wstring::npos);
    assert(warning.find(L"manual capture hotkey (Ctrl+Shift+F12)") != std::wstring::npos);
    assert(warning.find(L"ManualCaptureHotkey") != std::wstring::npos);
    assert(warning.find(L"Enable Thread Dump Hotkey = false") != std::wstring::npos);

    // The same Crash Logger and the new default hotkey: no warning.
    assert(!CheckCrashLoggerThreadDumpHotkey(plugins.Path(), kDefaultManualCaptureHotkey).firesOnManualCapture);
  }
  {
    // No ini yet: Crash Logger runs with its defaults.
    PluginsDirFixture plugins;
    plugins.Write("CrashLoggerSSE.dll", "");
    const auto check = CheckCrashLoggerThreadDumpHotkey(plugins.Path(), kOldCtrlShiftF12);
    assert(check.crashLoggerFound);
    assert(check.firesOnManualCapture);
  }
  {
    // Disabled thread dump hotkey: no warning.
    PluginsDirFixture plugins;
    plugins.Write("CrashLogger.dll", "");
    plugins.Write("CrashLogger.ini", "[Debug]\nEnable Thread Dump Hotkey = false\nThread Dump Hotkey = \n");
    assert(!CheckCrashLoggerThreadDumpHotkey(plugins.Path(), kOldCtrlShiftF12).firesOnManualCapture);
  }
  {
    // A custom Crash Logger hotkey the manual capture hotkey does not hold: no warning.
    PluginsDirFixture plugins;
    plugins.Write("CrashLogger.dll", "");
    plugins.Write("CrashLogger.ini", "[Debug]\nEnable Thread Dump Hotkey = true\nThread Dump Hotkey = 17, 18, 123\n");
    assert(!CheckCrashLoggerThreadDumpHotkey(plugins.Path(), kOldCtrlShiftF12).firesOnManualCapture);
    assert(!CheckCrashLoggerThreadDumpHotkey(plugins.Path(), kDefaultManualCaptureHotkey).firesOnManualCapture);
  }
  {
    // A custom Crash Logger hotkey the manual capture hotkey holds: warn.
    PluginsDirFixture plugins;
    plugins.Write("CrashLogger.dll", "");
    plugins.Write("CrashLogger.ini", "[Debug]\nEnable Thread Dump Hotkey = true\nThread Dump Hotkey = 17, 16, 8\n");
    const auto check = CheckCrashLoggerThreadDumpHotkey(plugins.Path(), kDefaultManualCaptureHotkey);
    assert(check.firesOnManualCapture);
    assert(CrashLoggerHotkeyCollisionWarning(check, kDefaultManualCaptureHotkey).find(L"(Ctrl+Shift+Backspace)") !=
           std::wstring::npos);
  }
}

}  // namespace

int main()
{
  TestParseAcceptsHotkeys();
  TestParseRejectsOtherText();
  TestFormat();
  TestLegacyManualCaptureChord();
  TestCrashLoggerIniDefaults();
  TestCrashLoggerIniEnableFlag();
  TestCrashLoggerIniHotkeyList();
  TestCrashLoggerFiresOnHotkey();
  TestPreflightCheckFixtures();
  std::puts("manual capture hotkey tests passed");
  return 0;
}
