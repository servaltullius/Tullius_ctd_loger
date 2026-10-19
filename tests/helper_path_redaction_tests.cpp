// SkyrimDiag_Preflight.json is attached to reports: the helper masks the
// symbol search path and cache folder there the way the dump tool masks its
// redacted summaries, and both give the same text for the same path.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "OutputWriterInternals.h"
#include "SkyrimDiagHelper/PathRedaction.h"
#include "SourceGuardTestUtils.h"

namespace {

using skydiag::helper::RedactPath;
using skydiag::helper::RedactSymbolSearchPath;
namespace dump_tool_writer = skydiag::dump_tool::internal::output_writer;

void Require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void TestRedactPath()
{
  Require(RedactPath(L"C:\\Users\\A\\AppData\\Local\\SkyrimDiag\\SymbolCache") == L"<redacted>\\SymbolCache",
    "an absolute folder keeps only its last element");
  Require(RedactPath(L"\\\\server\\share\\syms") == L"<redacted>\\syms", "a UNC folder is redacted");
  Require(RedactPath(L"D:\\") == L"<redacted>", "a drive root has no element to keep");
  Require(RedactPath(L"relative\\syms") == L"relative\\syms", "a relative path stays");
  Require(RedactPath(L"").empty(), "empty stays empty");
}

void TestRedactSymbolSearchPath()
{
  Require(RedactSymbolSearchPath(L"").empty(), "empty");
  Require(RedactSymbolSearchPath(
            L"C:\\Users\\A\\LocalSyms;srv*C:\\Users\\A\\Symbols*https://msdl.microsoft.com/download/symbols") ==
            L"<redacted>\\LocalSyms;srv*<redacted>\\Symbols*https://msdl.microsoft.com/download/symbols",
    "every directory of an env-provided symbol path is redacted, the URL stays");
  Require(RedactSymbolSearchPath(L"cache*C:\\Users\\A\\Cache;\\\\server\\share\\syms;relative") ==
            L"cache*<redacted>\\Cache;<redacted>\\syms;relative",
    "cache chains and UNC shares are redacted; relative elements stay");
  Require(RedactSymbolSearchPath(L" D:\\Sym ; srv*https://msdl.microsoft.com/download/symbols") ==
            L" <redacted>\\Sym ; srv*https://msdl.microsoft.com/download/symbols",
    "blanks around elements are kept and do not hide a path");
}

void TestSameMaskingAsTheDumpTool()
{
  const wchar_t* const paths[] = {
    L"",
    L"C:\\Users\\A\\Sym",
    L"srv*C:\\Users\\A\\AppData\\Local\\SkyrimDiag\\SymbolCache*https://msdl.microsoft.com/download/symbols",
    L"cache*C:\\Users\\A\\Cache;\\\\server\\share\\syms;relative",
    L" D:\\Sym ; srv*https://msdl.microsoft.com/download/symbols",
    L"E:/Forward/Slashes;;*",
  };
  for (const wchar_t* path : paths) {
    Require(RedactSymbolSearchPath(path) == dump_tool_writer::RedactSymbolSearchPath(path, true),
      "the helper and the dump tool mask a symbol search path alike");
    Require(RedactPath(path) == dump_tool_writer::MaybeRedactPath(path, true),
      "the helper and the dump tool mask a path alike");
  }
}

void TestPreflightMasksTheSymbolPaths()
{
  const auto text = skydiag::tests::source_guard::ReadAllText(
    skydiag::tests::source_guard::ProjectRoot() / "helper" / "src" / "CompatibilityPreflight.cpp");
  Require(text.find("{ \"search_path\", WideToUtf8(skydiag::helper::RedactSymbolSearchPath(") != std::string::npos,
    "SkyrimDiag_Preflight.json masks the symbol search path");
  Require(text.find("{ \"cache_path\", WideToUtf8(skydiag::helper::RedactPath(") != std::string::npos,
    "SkyrimDiag_Preflight.json masks the symbol cache folder");
}

}  // namespace

int main()
{
  try {
    TestRedactPath();
    TestRedactSymbolSearchPath();
    TestSameMaskingAsTheDumpTool();
    TestPreflightMasksTheSymbolPaths();
    std::puts("helper path redaction tests passed");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "%s\n", ex.what());
    return 1;
  }
}
