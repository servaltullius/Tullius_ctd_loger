// The helper must not end on a filesystem error: the one-argument
// std::filesystem queries throw on ERROR_NOT_READY, ERROR_BAD_NET_NAME and
// similar errors from an unready drive or an unreachable share named in the
// ini, and a range-for over directory_iterator throws from operator++ when
// the folder goes away mid-scan. Every helper production file uses the
// error_code overloads instead.

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "SourceGuardTestUtils.h"

using skydiag::tests::source_guard::ProjectRoot;
using skydiag::tests::source_guard::ReadAllText;

namespace {

int g_failures = 0;

void Fail(const std::filesystem::path& file, std::size_t line, const std::string& what)
{
  std::fprintf(stderr, "%s:%zu: %s\n", file.generic_string().c_str(), line, what.c_str());
  ++g_failures;
}

std::size_t LineOf(const std::string& text, std::size_t pos)
{
  std::size_t line = 1;
  for (std::size_t i = 0; i < pos && i < text.size(); ++i) {
    if (text[i] == '\n') {
      ++line;
    }
  }
  return line;
}

bool InLineComment(const std::string& text, std::size_t pos)
{
  const std::size_t lineStart = text.rfind('\n', pos);
  const std::size_t from = lineStart == std::string::npos ? 0 : lineStart + 1;
  const std::size_t comment = text.find("//", from);
  return comment != std::string::npos && comment < pos;
}

// The argument list of the call whose '(' is at openPos; empty when unbalanced.
std::string_view CallArguments(const std::string& text, std::size_t openPos)
{
  int depth = 0;
  for (std::size_t i = openPos; i < text.size(); ++i) {
    if (text[i] == '(') {
      ++depth;
    } else if (text[i] == ')') {
      if (--depth == 0) {
        return std::string_view(text).substr(openPos + 1, i - openPos - 1);
      }
    }
  }
  return {};
}

bool HasTopLevelComma(std::string_view args)
{
  int depth = 0;
  for (const char c : args) {
    if (c == '(' || c == '[' || c == '{') {
      ++depth;
    } else if (c == ')' || c == ']' || c == '}') {
      --depth;
    } else if (c == ',' && depth == 0) {
      return true;
    }
  }
  return false;
}

void CheckFile(const std::filesystem::path& file)
{
  const std::string text = ReadAllText(file);
  static const char* const kQueries[] = {
    "std::filesystem::exists(",
    "std::filesystem::is_regular_file(",
    "std::filesystem::is_directory(",
    "std::filesystem::create_directories(",
    "std::filesystem::create_directory(",
    "std::filesystem::file_size(",
    "std::filesystem::last_write_time(",
  };
  for (const char* query : kQueries) {
    const std::string_view q(query);
    for (std::size_t pos = text.find(q); pos != std::string::npos; pos = text.find(q, pos + q.size())) {
      if (InLineComment(text, pos)) {
        continue;
      }
      const std::string_view args = CallArguments(text, pos + q.size() - 1);
      // A query on a std::filesystem::file_status value never throws.
      if (HasTopLevelComma(args) || args == "status") {
        continue;
      }
      Fail(file, LineOf(text, pos), std::string(q) + std::string(args) + ") has no error_code argument");
    }
  }
  const std::string_view rangeFor = ": std::filesystem::directory_iterator(";
  for (std::size_t pos = text.find(rangeFor); pos != std::string::npos; pos = text.find(rangeFor, pos + 1)) {
    if (!InLineComment(text, pos)) {
      Fail(file, LineOf(text, pos), "range-for over directory_iterator (operator++ throws); use increment(ec)");
    }
  }
}

}  // namespace

int main()
{
  const auto root = ProjectRoot();
  std::vector<std::filesystem::path> files;
  for (const auto& dir : { root / "helper" / "src", root / "helper" / "include" / "SkyrimDiagHelper" }) {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      const auto ext = entry.path().extension();
      if (ext == ".cpp" || ext == ".h") {
        files.push_back(entry.path());
      }
    }
  }
  if (files.size() < 10) {
    std::fprintf(stderr, "helper sources not found under %s\n", root.generic_string().c_str());
    return 1;
  }
  for (const auto& file : files) {
    CheckFile(file);
  }
  if (g_failures != 0) {
    return 1;
  }
  std::puts("helper nothrow filesystem guard tests passed");
  return 0;
}
