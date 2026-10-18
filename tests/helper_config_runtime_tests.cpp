// LoadConfig reading ManualCaptureHotkey from SkyrimDiagHelper.ini beside the
// running executable, as the helper does: a valid chord is used, and an
// invalid one keeps the default and comes back in err for the helper log.
// This executable is built into its own directory (tests/CMakeLists.txt) so
// the ini it writes there is nobody else's.

#include <Windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "SkyrimDiagHelper/Config.h"

using skydiag::helper::HelperConfig;
using skydiag::helper::kDefaultManualCaptureHotkey;
using skydiag::helper::kHotkeyModAlt;
using skydiag::helper::kHotkeyModControl;
using skydiag::helper::LoadConfig;

namespace {

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::filesystem::path IniBesideExe()
{
  std::vector<wchar_t> buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  Require(n > 0 && n < buf.size(), "GetModuleFileNameW failed");
  return std::filesystem::path(std::wstring(buf.data(), n)).parent_path() / L"SkyrimDiagHelper.ini";
}

class IniFile
{
public:
  IniFile() : path_(IniBesideExe()) {}
  ~IniFile()
  {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  IniFile(const IniFile&) = delete;
  IniFile& operator=(const IniFile&) = delete;

  void Write(std::string_view text) const
  {
    std::ofstream out(path_, std::ios::out | std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    Require(static_cast<bool>(out), "writing SkyrimDiagHelper.ini failed");
  }

private:
  std::filesystem::path path_;
};

void TestValidHotkeyIsUsed(const IniFile& ini)
{
  ini.Write("[SkyrimDiagHelper]\r\nManualCaptureHotkey=Ctrl+Alt+F11\r\n");
  std::wstring err = L"stale";
  const HelperConfig cfg = LoadConfig(&err);
  Require(cfg.manualCaptureHotkey.modifiers == (kHotkeyModControl | kHotkeyModAlt), "Ctrl+Alt is read");
  Require(cfg.manualCaptureHotkey.virtualKey == 0x7Au, "F11 is read");
  Require(err.empty(), "a valid hotkey gives no warning");
}

void TestInvalidHotkeyKeepsTheDefault(const IniFile& ini)
{
  for (const char* value : { "Ctrl+Shift+F99", "Shift+P" }) {
    ini.Write(std::string("[SkyrimDiagHelper]\r\nManualCaptureHotkey=") + value + "\r\n");
    std::wstring err;
    const HelperConfig cfg = LoadConfig(&err);
    Require(cfg.manualCaptureHotkey == kDefaultManualCaptureHotkey, "an invalid hotkey keeps the default");
    Require(err.find(L"is not a valid hotkey") != std::wstring::npos, "an invalid hotkey is reported");
    Require(err.find(L"Ctrl+Shift+Backspace") != std::wstring::npos, "the warning names the default in use");
  }
}

void TestMissingHotkeyIsTheDefault(const IniFile& ini)
{
  ini.Write("[SkyrimDiagHelper]\r\nEnableManualCaptureHotkey=1\r\n");
  std::wstring err = L"stale";
  const HelperConfig cfg = LoadConfig(&err);
  Require(cfg.manualCaptureHotkey == kDefaultManualCaptureHotkey, "no ManualCaptureHotkey means the default");
  Require(err.empty(), "no ManualCaptureHotkey gives no warning");
}

}  // namespace

int main()
{
  try {
    const IniFile ini;
    TestValidHotkeyIsUsed(ini);
    TestInvalidHotkeyKeepsTheDefault(ini);
    TestMissingHotkeyIsTheDefault(ini);
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "FAILED: %s\n", ex.what());
    return 1;
  }
  std::puts("helper config runtime tests passed");
  return 0;
}
