#pragma once

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace skydiag::tests::source_guard {

inline std::string ReadAllText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline void AssertContains(const std::string& haystack, const char* needle, const char* message)
{
  assert(haystack.find(needle) != std::string::npos && message);
}

inline std::string ExtractFunctionBody(const std::string& source, const char* signature)
{
  const auto sigPos = source.find(signature);
  assert(sigPos != std::string::npos && "Function signature not found");

  const auto bracePos = source.find('{', sigPos);
  assert(bracePos != std::string::npos && "Function body start not found");

  bool inLineComment = false;
  bool inBlockComment = false;
  bool inString = false;
  bool inChar = false;
  bool escaping = false;
  std::size_t i = bracePos + 1;
  int depth = 1;
  for (; i < source.size(); ++i) {
    const char c = source[i];
    const char next = (i + 1 < source.size()) ? source[i + 1] : '\0';

    if (inLineComment) {
      if (c == '\n') {
        inLineComment = false;
      }
      continue;
    }
    if (inBlockComment) {
      if (c == '*' && next == '/') {
        inBlockComment = false;
        ++i;
      }
      continue;
    }
    if (inString) {
      if (escaping) {
        escaping = false;
      } else if (c == '\\') {
        escaping = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (inChar) {
      if (escaping) {
        escaping = false;
      } else if (c == '\\') {
        escaping = true;
      } else if (c == '\'') {
        inChar = false;
      }
      continue;
    }

    if (c == '/' && next == '/') {
      inLineComment = true;
      ++i;
      continue;
    }
    if (c == '/' && next == '*') {
      inBlockComment = true;
      ++i;
      continue;
    }
    if (c == '"') {
      inString = true;
      continue;
    }
    if (c == '\'') {
      inChar = true;
      continue;
    }
    if (c == '{') {
      ++depth;
      continue;
    }
    if (c == '}') {
      --depth;
      if (depth == 0) {
        return source.substr(bracePos + 1, i - bracePos - 1);
      }
    }
  }

  assert(false && "Function body end not found");
  return {};
}

inline void AssertOrdered(const std::string& haystack, const char* first, const char* second, const char* message)
{
  const auto firstPos = haystack.find(first);
  const auto secondPos = haystack.find(second);
  assert(firstPos != std::string::npos && secondPos != std::string::npos && message);
  assert(firstPos < secondPos && message);
}

}  // namespace skydiag::tests::source_guard
