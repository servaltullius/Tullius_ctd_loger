#pragma once

#include <cstdlib>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>

namespace skydiag::tests::source_guard {

inline std::filesystem::path ProjectRoot()
{
  const char* root = std::getenv("SKYDIAG_PROJECT_ROOT");
  if (root && *root) {
    return std::filesystem::path(root);
  }
  return std::filesystem::path(__FILE__).parent_path().parent_path();
}

inline std::string ReadAllText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::in | std::ios::binary);
  assert(in && "Failed to open file");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline std::string ReadConcatenatedText(std::initializer_list<std::filesystem::path> paths)
{
  std::ostringstream ss;
  for (const auto& path : paths) {
    ss << ReadAllText(path);
  }
  return ss.str();
}

inline bool TryReadAllText(const std::filesystem::path& path, std::string* out)
{
  if (out) {
    out->clear();
  }

  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in) {
    return false;
  }

  std::ostringstream ss;
  ss << in.rdbuf();
  if (out) {
    *out = ss.str();
  }
  return true;
}

inline std::string ReadSplitAwareText(const std::filesystem::path& path)
{
  std::ostringstream ss;
  ss << ReadAllText(path);

  const auto name = path.filename().wstring();
  const auto dir = path.parent_path();
  if (name == L"Analyzer.cpp") {
    ss << ReadAllText(dir / "Analyzer.CaptureInputs.cpp");
    ss << ReadAllText(dir / "Analyzer.History.cpp");
  } else if (name == L"OutputWriter.cpp") {
    ss << ReadAllText(dir / "OutputWriter.Summary.cpp");
    ss << ReadAllText(dir / "OutputWriter.Report.cpp");
  } else if (name == L"EvidenceBuilderEvidence.cpp") {
    ss << ReadAllText(dir / "EvidenceBuilderEvidence.Context.cpp");
    ss << ReadAllText(dir / "EvidenceBuilderEvidence.Crash.cpp");
    ss << ReadAllText(dir / "EvidenceBuilderEvidence.Freeze.cpp");
  } else if (name == L"main.cpp") {
    ss << ReadAllText(dir / "HelperMain.Startup.cpp");
    ss << ReadAllText(dir / "HelperMain.Process.cpp");
    ss << ReadAllText(dir / "HelperMain.Loop.cpp");
  }

  return ss.str();
}

inline bool TryReadSplitAwareText(const std::filesystem::path& path, std::string* out)
{
  std::ostringstream ss;
  std::string chunk;
  if (!TryReadAllText(path, &chunk)) {
    return false;
  }
  ss << chunk;

  const auto name = path.filename().wstring();
  const auto dir = path.parent_path();
  if (name == L"Analyzer.cpp") {
    if (!TryReadAllText(dir / "Analyzer.CaptureInputs.cpp", &chunk)) {
      return false;
    }
    ss << chunk;
    if (!TryReadAllText(dir / "Analyzer.History.cpp", &chunk)) {
      return false;
    }
    ss << chunk;
  } else if (name == L"OutputWriter.cpp") {
    if (!TryReadAllText(dir / "OutputWriter.Summary.cpp", &chunk)) {
      return false;
    }
    ss << chunk;
    if (!TryReadAllText(dir / "OutputWriter.Report.cpp", &chunk)) {
      return false;
    }
    ss << chunk;
  } else if (name == L"EvidenceBuilderEvidence.cpp") {
    if (!TryReadAllText(dir / "EvidenceBuilderEvidence.Context.cpp", &chunk)) {
      return false;
    }
    ss << chunk;
    if (!TryReadAllText(dir / "EvidenceBuilderEvidence.Crash.cpp", &chunk)) {
      return false;
    }
    ss << chunk;
    if (!TryReadAllText(dir / "EvidenceBuilderEvidence.Freeze.cpp", &chunk)) {
      return false;
    }
    ss << chunk;
  } else if (name == L"main.cpp") {
    if (!TryReadAllText(dir / "HelperMain.Startup.cpp", &chunk)) {
      return false;
    }
    ss << chunk;
    if (!TryReadAllText(dir / "HelperMain.Process.cpp", &chunk)) {
      return false;
    }
    ss << chunk;
    if (!TryReadAllText(dir / "HelperMain.Loop.cpp", &chunk)) {
      return false;
    }
    ss << chunk;
  }

  if (out) {
    *out = ss.str();
  }
  return true;
}

inline std::string ReadProjectText(const std::filesystem::path& relativePath)
{
  return ReadSplitAwareText(ProjectRoot() / relativePath);
}

inline std::string ReadProjectConcatenatedText(std::initializer_list<std::filesystem::path> relativePaths)
{
  std::ostringstream ss;
  for (const auto& relativePath : relativePaths) {
    ss << ReadProjectText(relativePath);
  }
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
