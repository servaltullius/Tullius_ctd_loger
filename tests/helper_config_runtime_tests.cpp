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
#include <utility>
#include <vector>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/IniText.h"

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

// "스카이림 로그"
const std::wstring kKoreanLeaf = L"\uC2A4\uCE74\uC774\uB9BC \uB85C\uADF8";

// Review H2: GetPrivateProfileStringW read a UTF-8 BOM as part of the first
// section header (every setting fell back to its default) and decoded UTF-8
// in the ANSI code page (a Korean OutputDir became mojibake on CP949).
void TestUtf8BomAndUtf8ValuesAreRead(const IniFile& ini)
{
  ini.Write(
    "\xEF\xBB\xBF[SkyrimDiagHelper]\r\nHangThresholdInGameSec=15\r\nDumpMode=2\r\n"
    "OutputDir=C:\\\xEC\x8A\xA4\xEC\xB9\xB4\xEC\x9D\xB4\xEB\xA6\xBC \xEB\xA1\x9C\xEA\xB7\xB8\r\n");
  const HelperConfig cfg = LoadConfig(nullptr);
  Require(cfg.hangThresholdInGameSec == 15u, "a UTF-8 BOM must not hide the first section");
  Require(cfg.dumpMode == skydiag::helper::DumpMode::kFull, "every key of a BOM file is read");
  Require(cfg.outputDir == L"C:\\" + kKoreanLeaf, "a UTF-8 OutputDir is decoded as UTF-8");
}

void TestUtf16OutputDirIsRead(const IniFile& ini)
{
  const std::wstring text = L"[SkyrimDiagHelper]\r\nOutputDir=C:\\" + kKoreanLeaf + L"\r\nMaxCrashDumps=7\r\n";
  std::string bytes = "\xFF\xFE";
  for (const wchar_t c : text) {
    bytes.push_back(static_cast<char>(c & 0xFF));
    bytes.push_back(static_cast<char>((c >> 8) & 0xFF));
  }
  ini.Write(bytes);
  const HelperConfig cfg = LoadConfig(nullptr);
  Require(cfg.outputDir == L"C:\\" + kKoreanLeaf, "a UTF-16LE OutputDir is read");
  Require(cfg.maxCrashDumps == 7u, "UTF-16LE values are read");
}

// A file saved in the ANSI code page (CP949 bytes here) is still read the way
// the Windows API read it: through CP_ACP.
void TestAnsiOutputDirUsesTheCodePage(const IniFile& ini)
{
  const std::string ansiLeaf = "\xBD\xBA\xC4\xAB\xC0\xCC\xB8\xB2 \xB7\xCE\xB1\xD7";
  ini.Write("[SkyrimDiagHelper]\r\nOutputDir=C:\\" + ansiLeaf + "\r\nMaxHangDumps=9\r\n");
  wchar_t wide[64]{};
  const int n = MultiByteToWideChar(CP_ACP, 0, ansiLeaf.data(), static_cast<int>(ansiLeaf.size()), wide, 64);
  Require(n > 0, "CP_ACP decoding of the test bytes");
  const HelperConfig cfg = LoadConfig(nullptr);
  Require(cfg.outputDir == L"C:\\" + std::wstring(wide, static_cast<std::size_t>(n)), "non-UTF-8 bytes use CP_ACP");
  Require(cfg.maxHangDumps == 9u, "ANSI values are read");
}

// The in-process reader must answer exactly what GetPrivateProfile*W answers
// for an ASCII file, which both decode the same way.
void TestLookupsMatchTheWindowsApi()
{
  static const char kText[] =
    "Pre=0\r\n  [ S ]  \r\nQ1=\"quoted\"\r\nQ2='single'\r\nQ3=\"mismatch'\r\nQ4=  \"  spaced  \"  \r\nQ5=\"\r\n"
    "E=\r\nNoEq\r\n  Spaced Key   =   v a l   \r\nK=a=b\r\n;C1=1\r\n  ;C2=1\r\n#C3=2\r\nDup=first\r\nDup=second\r\n"
    "Tab\t=\tv\t\r\nI1=1 ; comment\r\nI2= 0x10\r\nI3=-5\r\nI4=abc\r\nI6=+7\r\nI7=  12abc\r\nI8=99999999999\r\n"
    "I9=010\r\nI10=0b11\r\nI11=0o17\r\nI12=\"5\"\r\nI13=4294967295\r\nI14=0X1f\r\nI15=- 5\r\nI16=0b12\r\n"
    "I17=0x\r\nI18=  -0x10\r\nI19=0xFFFFFFFFF\r\nI20=\t3\r\nI21=1e3\r\n[s]\r\nLater=1\r\n[T] trailing\r\nY=2\r\n"
    "[U\r\nZ=3\r\n";
  const auto path = std::filesystem::temp_directory_path() /
    (L"skydiag_ini_equivalence_" + std::to_wstring(GetCurrentProcessId()) + L".ini");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(kText, static_cast<std::streamsize>(sizeof(kText) - 1));
    Require(static_cast<bool>(out), "writing the equivalence ini failed");
  }
  const auto doc = skydiag::helper::IniDocument::Parse(
    skydiag::helper::DecodeIniBytes(std::string_view(kText, sizeof(kText) - 1), nullptr));

  const std::pair<const wchar_t*, const wchar_t*> lookups[] = {
    { L"S", L"Q1" }, { L"S", L"Q2" }, { L"S", L"Q3" }, { L"S", L"Q4" }, { L"S", L"Q5" }, { L"S", L"E" },
    { L"S", L"NoEq" }, { L"S", L"Spaced Key" }, { L"s", L"spaced key" }, { L"S", L"K" }, { L"S", L";C1" },
    { L"S", L";C2" }, { L"S", L"#C3" }, { L"S", L"Dup" }, { L"S", L"Tab" }, { L"S", L"Later" }, { L"S", L"Missing" },
    { L" S ", L"Q1" }, { L"T", L"Y" }, { L"U", L"Z" }, { L"", L"Pre" }, { L"Nope", L"Q1" },
    { L"S", L"I1" }, { L"S", L"I2" }, { L"S", L"I3" }, { L"S", L"I4" }, { L"S", L"I6" }, { L"S", L"I7" },
    { L"S", L"I8" }, { L"S", L"I9" }, { L"S", L"I10" }, { L"S", L"I11" }, { L"S", L"I12" }, { L"S", L"I13" },
    { L"S", L"I14" }, { L"S", L"I15" }, { L"S", L"I16" }, { L"S", L"I17" }, { L"S", L"I18" }, { L"S", L"I19" },
    { L"S", L"I20" }, { L"S", L"I21" },
  };
  std::string mismatch;
  for (const auto& [section, key] : lookups) {
    wchar_t buf[512]{};
    const DWORD n = GetPrivateProfileStringW(section, key, L"<def>", buf, 512, path.c_str());
    const std::wstring api(buf, n);
    const std::wstring ours = skydiag::helper::ReadIniProfileString(doc, section, key, L"<def>");
    const int apiInt = static_cast<int>(GetPrivateProfileIntW(section, key, 77, path.c_str()));
    const int oursInt = skydiag::helper::ReadIniProfileInt(doc, section, key, 77);
    if (api != ours || apiInt != oursInt) {
      char line[160]{};
      std::snprintf(line, sizeof(line), "[%ls] %ls: api='%ls'/%d ours='%ls'/%d; ",
        section, key, api.c_str(), apiInt, ours.c_str(), oursInt);
      mismatch += line;
    }
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (!mismatch.empty()) {
    throw std::runtime_error("ini lookups differ from GetPrivateProfile*W: " + mismatch);
  }
}

// Review M11: a custom OutputDir that cannot be written falls back to the
// default folder next to the helper, then to %LOCALAPPDATA%.
void TestOutputDirFallbacks(const IniFile& ini)
{
  ini.Write("[SkyrimDiagHelper]\r\nOutputDir=Z:\\Custom Logs\r\n");
  const HelperConfig custom = LoadConfig(nullptr);
  const auto exeDir = IniBesideExe().parent_path();
  Require(custom.outputDirFallbacks.size() == 2u, "a custom OutputDir has two fallbacks");
  Require(std::filesystem::path(custom.outputDirFallbacks[0]) == exeDir / L"Tullius Ctd Logs",
    "the first fallback is the default folder next to the helper");
  Require(custom.outputDirFallbacks[1].find(L"\\SkyrimDiag\\Tullius Ctd Logs") != std::wstring::npos,
    "the second fallback is under %LOCALAPPDATA%");

  ini.Write("[SkyrimDiagHelper]\r\nOutputDir=\r\n");
  const HelperConfig blank = LoadConfig(nullptr);
  Require(blank.outputDirFallbacks.size() == 1u, "the default folder is not its own fallback");
}

}  // namespace

int main()
{
  try {
    const IniFile ini;
    TestValidHotkeyIsUsed(ini);
    TestInvalidHotkeyKeepsTheDefault(ini);
    TestMissingHotkeyIsTheDefault(ini);
    TestUtf8BomAndUtf8ValuesAreRead(ini);
    TestUtf16OutputDirIsRead(ini);
    TestAnsiOutputDirUsesTheCodePage(ini);
    TestLookupsMatchTheWindowsApi();
    TestOutputDirFallbacks(ini);
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "FAILED: %s\n", ex.what());
    return 1;
  }
  std::puts("helper config runtime tests passed");
  return 0;
}
