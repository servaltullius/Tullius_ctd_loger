// Paths and names outside ASCII (audit F16, review #13): the game hands over
// loose-file names in the ANSI code page, and ModOrganizer.ini writes a
// Korean profile name as \xNN escapes inside @ByteArray(...).

#include <Windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "Mo2Index.h"
#include "SkyrimDiagQSettings.h"
#include "SkyrimDiagResourcePath.h"
#include "Utf.h"

namespace {

using skydiag::DecodeQSettingsIniValue;
using skydiag::NormalizeResourcePath;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string Encode(std::wstring_view text, UINT codePage)
{
  const int n = WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
  return out;
}

bool IsValidUtf8(std::string_view s)
{
  return s.empty() ||
         MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0) > 0;
}

std::string Normalize(std::string_view in, UINT codePage, std::size_t cap = 260)
{
  std::vector<char> buf(cap);
  std::size_t len = 0;
  NormalizeResourcePath(in, codePage, buf.data(), buf.size(), len);
  return std::string(buf.data(), len);
}

std::uint64_t Fnv1a(std::string_view s)
{
  std::uint64_t h = 14695981039346656037ull;
  for (const char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  return h;
}

void TestResourcePaths()
{
  // U+AC02 is CP949 0x81 0x41: its trail byte is 'A', which lower-casing
  // used to turn into 'a' (another character).
  const std::wstring korean = L"Meshes\\\uAC02\uD55C\\Body.NIF";
  const std::string cp949 = Encode(korean, 949);
  Require(cp949.find('A') != std::string::npos, "the CP949 bytes hold an 'A' trail byte");
  Require(Normalize("Data\\" + cp949, 949) == Encode(L"meshes\\\uAC02\uD55C\\body.nif", CP_UTF8),
    "CP949 becomes UTF-8 and only ASCII is lower-cased");

  Require(Normalize(Encode(L"C:\\Games\\Skyrim\\Data\\Meshes\\Caf\u00E9.nif", 1252), 1252) ==
            Encode(L"meshes\\caf\u00e9.nif", CP_UTF8),
    "CP1252 after the absolute Data folder");

  const std::string ascii = Normalize("data/Meshes/Actors/Body.NIF", 1252);
  Require(ascii == "meshes\\actors\\body.nif", "ASCII as before");
  std::vector<char> buf(260);
  std::size_t len = 0;
  Require(NormalizeResourcePath("data/Meshes/Actors/Body.NIF", 1252, buf.data(), buf.size(), len) == Fnv1a(ascii),
    "the hash is over the stored bytes, as before");

  Require(Normalize(Encode(korean, CP_UTF8), CP_UTF8) == Encode(L"meshes\\\uAC02\uD55C\\body.nif", CP_UTF8),
    "a UTF-8 code page is kept");

  // 12 bytes fit with the terminator: "meshes\" (7) and one 3-byte character;
  // the 2 bytes of the next one that fit are dropped.
  const std::string cut = Normalize(Encode(L"meshes\\\uD55C\uD55C\uD55C.nif", 949), 949, 13);
  Require(IsValidUtf8(cut) && cut == Encode(L"meshes\\\uD55C", CP_UTF8), "cut at a character boundary");
}

void TestQSettingsValues()
{
  Require(DecodeQSettingsIniValue("@ByteArray(Default)") == "Default", "plain profile");
  Require(DecodeQSettingsIniValue("@ByteArray(\\xed\\x95\\x9c\\xea\\xb8\\x80)") == Encode(L"\uD55C\uAE00", CP_UTF8),
    "Korean profile written as byte escapes");
  Require(DecodeQSettingsIniValue("\"My \\\"Profile\\\"\"") == "My \"Profile\"", "quoted value");
  Require(DecodeQSettingsIniValue("\\xd55c") == Encode(L"\uD55C", CP_UTF8), "a QString escape is one character");
}

void TestUtf8OrAnsi()
{
  using skydiag::dump_tool::Utf8OrAnsiToWide;
  Require(Utf8OrAnsiToWide(Encode(L"\uD55C.nif", CP_UTF8)) == L"\uD55C.nif", "UTF-8 stays");
  const std::string ansi = "caf\xE9.nif";
  std::wstring expected(16, L'\0');
  expected.resize(static_cast<std::size_t>(
    MultiByteToWideChar(CP_ACP, 0, ansi.data(), static_cast<int>(ansi.size()), expected.data(), 16)));
  Require(Utf8OrAnsiToWide(ansi) == expected, "invalid UTF-8 is read in the ANSI code page");
}

void TestMo2KoreanProfile()
{
  const auto base = std::filesystem::temp_directory_path() /
                    (L"skydiag_non_ascii_" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  const std::wstring profile = L"\uC2A4\uCE74\uC774\uB9BC";
  std::filesystem::create_directories(base / L"mods" / L"Example" / L"SKSE" / L"Plugins");
  std::filesystem::create_directories(base / L"profiles" / profile);
  std::filesystem::create_directories(base / L"profiles" / L"Default");
  {
    std::ofstream ini(base / L"ModOrganizer.ini", std::ios::binary);
    ini << "[General]\r\nselected_profile=@ByteArray(";
    for (const unsigned char c : Encode(profile, CP_UTF8)) {
      char hex[8];
      std::snprintf(hex, sizeof(hex), "\\x%02x", c);
      ini << hex;
    }
    ini << ")\r\n";
  }
  {
    std::ofstream modlist(base / L"profiles" / profile / L"modlist.txt", std::ios::binary);
    modlist << "+Example\r\n";
  }

  const auto index = skydiag::dump_tool::TryBuildMo2IndexFromModulePaths(
    { (base / L"mods" / L"Example" / L"SKSE" / L"Plugins" / L"Example.dll").wstring() });
  std::filesystem::remove_all(base, ec);
  Require(index.has_value(), "the MO2 instance is found");
  Require(index->profileName == profile, "the Korean profile is selected, not another one");
}

}  // namespace

int main()
{
  try {
    TestResourcePaths();
    TestQSettingsValues();
    TestUtf8OrAnsi();
    TestMo2KoreanProfile();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "non_ascii_path_tests failed: %s\n", e.what());
    return 1;
  }
  return 0;
}
