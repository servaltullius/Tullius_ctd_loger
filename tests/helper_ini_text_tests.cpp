// SkyrimDiagHelper.ini decoding and lookup (helper/include/SkyrimDiagHelper/IniText.h).
//
// Review H2 (2026-10-19): GetPrivateProfileStringW decodes a file without a
// UTF-16 BOM in the ANSI code page, so a UTF-8 BOM dropped every setting and a
// UTF-8 Korean OutputDir became mojibake on CP949. The expected values of the
// lookup cases below were probed from GetPrivateProfileStringW/IntW on
// Windows 11; the Windows config runtime test compares the two directly.

#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>

#include "SkyrimDiagHelper/IniText.h"

using skydiag::helper::DecodeIniBytes;
using skydiag::helper::IniDocument;
using skydiag::helper::IniTextEncoding;
using skydiag::helper::ReadIniProfileInt;
using skydiag::helper::ReadIniProfileString;

namespace {

// "스카이림 로그"
const std::wstring kKoreanDir = L"D:\\\uC2A4\uCE74\uC774\uB9BC \uB85C\uADF8";

constexpr std::string_view kCp949Dir = "D:\\\xBD\xBA\xC4\xAB\xC0\xCC\xB8\xB2 \xB7\xCE\xB1\xD7";

std::wstring FakeCp949(std::string_view bytes)
{
  // Stands in for MultiByteToWideChar(CP_ACP) on a Korean system: knows the
  // one CP949 path used here and passes ASCII through.
  std::wstring out;
  std::size_t i = 0;
  while (i < bytes.size()) {
    if (bytes.substr(i, kCp949Dir.size()) == kCp949Dir) {
      out += kKoreanDir;
      i += kCp949Dir.size();
    } else {
      out.push_back(static_cast<unsigned char>(bytes[i]) < 0x80 ? static_cast<wchar_t>(bytes[i]) : L'?');
      ++i;
    }
  }
  return out;
}

std::wstring Lookup(std::string_view iniBytes, const wchar_t* key, IniTextEncoding* encoding = nullptr)
{
  const auto doc = IniDocument::Parse(DecodeIniBytes(iniBytes, nullptr, encoding));
  return ReadIniProfileString(doc, L"SkyrimDiagHelper", key, L"<def>");
}

void TestUtf8BomKeepsTheFirstSection()
{
  IniTextEncoding encoding{};
  assert(Lookup("\xEF\xBB\xBF[SkyrimDiagHelper]\r\nDumpMode=2\r\n", L"DumpMode", &encoding) == L"2");
  assert(encoding == IniTextEncoding::kUtf8Bom);
}

void TestUtf8KoreanOutputDir()
{
  IniTextEncoding encoding{};
  const std::string ini =
    "[SkyrimDiagHelper]\r\nOutputDir=D:\\\xEC\x8A\xA4\xEC\xB9\xB4\xEC\x9D\xB4\xEB\xA6\xBC \xEB\xA1\x9C\xEA\xB7\xB8\r\n";
  assert(Lookup(ini, L"OutputDir", &encoding) == kKoreanDir);
  assert(encoding == IniTextEncoding::kUtf8);
}

void TestAnsiFallbackUsesTheCodePageDecoder()
{
  IniTextEncoding encoding{};
  const auto text = DecodeIniBytes(kCp949Dir, &FakeCp949, &encoding);
  assert(encoding == IniTextEncoding::kAnsi && "CP949 bytes are not UTF-8");
  assert(text == kKoreanDir);

  const std::string ini = "[SkyrimDiagHelper]\r\nOutputDir=" + std::string(kCp949Dir) + "\r\nDumpMode=0\r\n";
  const auto doc = IniDocument::Parse(DecodeIniBytes(ini, &FakeCp949, &encoding));
  assert(encoding == IniTextEncoding::kAnsi);
  assert(ReadIniProfileString(doc, L"SkyrimDiagHelper", L"OutputDir", L"") == kKoreanDir);
  assert(ReadIniProfileInt(doc, L"SkyrimDiagHelper", L"DumpMode", 1) == 0);
}

void TestUtf16()
{
  // UTF-16LE with BOM, as Notepad's "UTF-16 LE" writes it.
  std::string le = "\xFF\xFE";
  std::string be = "\xFE\xFF";
  std::string leNoBom;
  for (const wchar_t c : std::wstring(L"[SkyrimDiagHelper]\r\nOutputDir=") + kKoreanDir + L"\r\n") {
    const auto u = static_cast<unsigned>(c);
    le.push_back(static_cast<char>(u & 0xFF));
    le.push_back(static_cast<char>(u >> 8));
    leNoBom.push_back(static_cast<char>(u & 0xFF));
    leNoBom.push_back(static_cast<char>(u >> 8));
    be.push_back(static_cast<char>(u >> 8));
    be.push_back(static_cast<char>(u & 0xFF));
  }
  IniTextEncoding encoding{};
  assert(Lookup(le, L"OutputDir", &encoding) == kKoreanDir && encoding == IniTextEncoding::kUtf16LeBom);
  assert(Lookup(be, L"OutputDir", &encoding) == kKoreanDir && encoding == IniTextEncoding::kUtf16BeBom);
  assert(Lookup(leNoBom, L"OutputDir", &encoding) == kKoreanDir && encoding == IniTextEncoding::kUtf16LeNoBom);
}

void TestEmptyAndMissing()
{
  IniTextEncoding encoding{};
  assert(Lookup("", L"DumpMode", &encoding) == L"<def>" && encoding == IniTextEncoding::kEmpty);
  assert(Lookup("[Other]\r\nDumpMode=2\r\n", L"DumpMode") == L"<def>");
  assert(Lookup("DumpMode=2\r\n", L"DumpMode") == L"<def>" && "a key before any section is ignored");
}

// Each case was checked against GetPrivateProfileStringW on Windows 11.
void TestWindowsStringSemantics()
{
  static const char kIni[] =
    "  [ SkyrimDiagHelper ]  \r\n"
    "Q1=\"quoted\"\r\n"
    "Q2='single'\r\n"
    "Q3=\"mismatch'\r\n"
    "Q4=  \"  spaced  \"  \r\n"
    "Q5=\"\r\n"
    "Q6=\"\"\r\n"
    "E=\r\n"
    "NoEq\r\n"
    "  Spaced Key   =   v a l   \r\n"
    "K=a=b\r\n"
    ";Commented=1\r\n"
    "  ;Indented=1\r\n"
    "#Hash=2\r\n"
    "Dup=first\r\n"
    "Dup=second\r\n"
    "Tab\t=\tv\t\r\n"
    "Inline=1 ; note\r\n"
    "Nul=a\0b\r\n"
    "After=2\r\n"
    "[skyrimdiaghelper]\r\n"
    "Later=1\r\n";
  const std::string_view ini(kIni, sizeof(kIni) - 1);  // keeps the embedded NUL
  const auto doc = IniDocument::Parse(DecodeIniBytes(ini, nullptr));
  const auto get = [&](const wchar_t* key) { return ReadIniProfileString(doc, L"SkyrimDiagHelper", key, L"<def>"); };
  assert(get(L"Q1") == L"quoted");
  assert(get(L"Q2") == L"single");
  assert(get(L"Q3") == L"\"mismatch'");
  assert(get(L"Q4") == L"  spaced  ");
  assert(get(L"Q5") == L"\"");
  assert(get(L"Q6").empty());
  assert(get(L"E").empty() && "a present empty value is not the default");
  assert(get(L"NoEq") == L"<def>");
  assert(get(L"spaced key") == L"v a l");
  assert(get(L"K") == L"a=b");
  assert(get(L";Commented") == L"<def>");
  assert(get(L";Indented") == L"<def>");
  assert(get(L"#Hash") == L"2" && "only ';' starts a comment");
  assert(get(L"Dup") == L"first");
  assert(get(L"Tab") == L"v");
  assert(get(L"Inline") == L"1 ; note" && "an inline comment stays in a string value");
  assert(get(L"Nul") == L"a" && "a NUL ends its line");
  assert(get(L"After") == L"2");
  assert(get(L"Later") == L"<def>" && "a second section with the same name is not searched");
  assert(ReadIniProfileString(doc, L" SkyrimDiagHelper ", L"Q1", L"<def>") == L"quoted");

  const auto unclosed = IniDocument::Parse(L"[S\r\nX=1\r\n[T] trailing\r\nY=2\r\n");
  assert(ReadIniProfileString(unclosed, L"S", L"X", L"") == L"1" && "a section header without ']' still opens it");
  assert(ReadIniProfileString(unclosed, L"T", L"Y", L"") == L"2");

  const auto lineEnds = IniDocument::Parse(L"[S]\rA=1\nB=\t2\t\r\n\t[U]\r\nC=3");
  assert(ReadIniProfileString(lineEnds, L"S", L"A", L"") == L"1");
  assert(ReadIniProfileString(lineEnds, L"S", L"B", L"") == L"2");
  assert(ReadIniProfileString(lineEnds, L"U", L"C", L"") == L"3");
}

// Each case was checked against GetPrivateProfileIntW (default 77) on Windows 11.
void TestWindowsIntSemantics()
{
  const auto doc = IniDocument::Parse(
    L"[S]\r\n"
    L"I1=1 ; comment\r\nI2= 0x10\r\nI3=-5\r\nI4=abc\r\nI5=\r\nI6=+7\r\nI7=  12abc\r\n"
    L"I8=99999999999\r\nI9=010\r\nI10=0b11\r\nI11=0o17\r\nI12=\"5\"\r\nI13=4294967295\r\n"
    L"I14=0X1f\r\nI15=- 5\r\nI16=0b12\r\nI17=0x\r\nI18=  -0x10\r\nI19=0xFFFFFFFFF\r\nI20=\t3\r\nI21=1e3\r\n");
  const auto get = [&](const wchar_t* key) { return ReadIniProfileInt(doc, L"S", key, 77); };
  assert(get(L"I1") == 1);
  assert(get(L"I2") == 16);
  assert(get(L"I3") == -5);
  assert(get(L"I4") == 0);
  assert(get(L"I5") == 77);
  assert(get(L"I6") == 7);
  assert(get(L"I7") == 12);
  assert(get(L"I8") == 1215752191);
  assert(get(L"I9") == 10);
  assert(get(L"I10") == 3);
  assert(get(L"I11") == 15);
  assert(get(L"I12") == 5);
  assert(get(L"I13") == -1);
  assert(get(L"I14") == 0);
  assert(get(L"I15") == 0);
  assert(get(L"I16") == 1);
  assert(get(L"I17") == 0);
  assert(get(L"I18") == -16);
  assert(get(L"I19") == -1);
  assert(get(L"I20") == 3);
  assert(get(L"I21") == 1);
  assert(get(L"Missing") == 77);
}

void TestInvalidUtf8AfterBomIsReplacedNotDropped()
{
  const auto text = DecodeIniBytes("\xEF\xBB\xBF[S]\r\nK=a\xFF" "b\r\n", nullptr);
  const auto doc = IniDocument::Parse(text);
  assert(ReadIniProfileString(doc, L"S", L"K", L"") == L"a\uFFFDb");
}

}  // namespace

int main()
{
  TestUtf8BomKeepsTheFirstSection();
  TestUtf8KoreanOutputDir();
  TestAnsiFallbackUsesTheCodePageDecoder();
  TestUtf16();
  TestEmptyAndMissing();
  TestWindowsStringSemantics();
  TestWindowsIntSemantics();
  TestInvalidUtf8AfterBomIsReplacedNotDropped();
  std::puts("helper ini text tests passed");
  return 0;
}
