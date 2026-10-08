#include "SkyrimDiagHelper/SkseLogParser.h"

#include <cstddef>
#include <optional>
#include <unordered_map>

namespace skydiag::helper {
namespace {

constexpr std::string_view kRuntimeHeader = "SKSE64 runtime: initialize (version = ";
constexpr std::string_view kImageBase = "imagebase = ";
constexpr std::string_view kCheckingPlugin = "checking plugin ";
constexpr std::string_view kPluginPrefix = "plugin ";
constexpr std::string_view kDllOpen = ".dll (";
constexpr std::string_view kHandleSuffix = " (handle ";
constexpr std::string_view kLoadedCorrectly = "loaded correctly";

char LowerAscii(char ch)
{
  return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + ('a' - 'A')) : ch;
}

std::string LowerAscii(std::string_view value)
{
  std::string out(value);
  for (auto& ch : out) {
    ch = LowerAscii(ch);
  }
  return out;
}

bool IsHexDigit(char ch)
{
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

bool IsHex8(std::string_view value)
{
  if (value.size() != 8) {
    return false;
  }
  for (const char ch : value) {
    if (!IsHexDigit(ch)) {
      return false;
    }
  }
  return true;
}

std::optional<std::uint64_t> ParseHex64(std::string_view value)
{
  if (value.empty() || value.size() > 16) {
    return std::nullopt;
  }
  std::uint64_t out = 0;
  for (const char ch : value) {
    if (!IsHexDigit(ch)) {
      return std::nullopt;
    }
    const char lower = LowerAscii(ch);
    const auto digit = (lower >= 'a') ? static_cast<std::uint64_t>(lower - 'a' + 10) : static_cast<std::uint64_t>(lower - '0');
    out = (out << 4u) | digit;
  }
  return out;
}

std::optional<std::int64_t> ParseDecimal(std::string_view value)
{
  bool negative = false;
  if (!value.empty() && value.front() == '-') {
    negative = true;
    value.remove_prefix(1);
  }
  if (value.empty() || value.size() > 18) {
    return std::nullopt;
  }
  std::int64_t out = 0;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
    out = out * 10 + (ch - '0');
  }
  return negative ? -out : out;
}

std::size_t FindCaseInsensitive(std::string_view haystack, std::string_view needle)
{
  if (needle.empty() || haystack.size() < needle.size()) {
    return std::string_view::npos;
  }
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (LowerAscii(haystack[i + j]) != LowerAscii(needle[j])) {
        match = false;
        break;
      }
    }
    if (match) {
      return i;
    }
  }
  return std::string_view::npos;
}

struct PluginStatusLine
{
  std::string_view dll_name;
  std::string_view plugin_name;
  std::string_view status;
  std::int64_t error_code = 0;
  bool loaded = false;
};

std::optional<PluginStatusLine> ParsePluginStatusLine(std::string_view line)
{
  if (line.substr(0, kPluginPrefix.size()) != kPluginPrefix) {
    return std::nullopt;
  }
  const std::string_view rest = line.substr(kPluginPrefix.size());
  const std::size_t dllEnd = FindCaseInsensitive(rest, kDllOpen);
  if (dllEnd == std::string_view::npos || dllEnd == 0u) {
    return std::nullopt;
  }

  PluginStatusLine out{};
  out.dll_name = rest.substr(0, dllEnd + 4u);  // keep ".dll"

  // "<dataVersion> " opens the version block.
  const std::string_view block = rest.substr(dllEnd + kDllOpen.size());
  if (block.size() < 9u || !IsHex8(block.substr(0, 8)) || block[8] != ' ') {
    return std::nullopt;
  }
  // The name may contain spaces; it ends at the first " <pluginVersion>) ".
  const std::string_view body = block.substr(9);
  std::size_t close = std::string_view::npos;
  for (std::size_t pos = body.find(") "); pos != std::string_view::npos; pos = body.find(") ", pos + 1u)) {
    if (pos >= 9u && body[pos - 9u] == ' ' && IsHex8(body.substr(pos - 8u, 8))) {
      close = pos;
      break;
    }
  }
  if (close == std::string_view::npos) {
    return std::nullopt;
  }
  out.plugin_name = body.substr(0, close - 9u);

  std::string_view tail = body.substr(close + 2u);
  const std::size_t handle = tail.rfind(kHandleSuffix);
  if (handle == std::string_view::npos || tail.back() != ')') {
    return std::nullopt;
  }
  tail = tail.substr(0, handle);
  if (tail == kLoadedCorrectly) {
    out.status = tail;
    out.loaded = true;
    return out;
  }

  // Error lines end with " <code>" before the handle.
  const std::size_t lastSpace = tail.rfind(' ');
  if (lastSpace == std::string_view::npos) {
    return std::nullopt;
  }
  const auto code = ParseDecimal(tail.substr(lastSpace + 1u));
  if (!code) {
    return std::nullopt;
  }
  out.status = tail.substr(0, lastSpace);
  out.error_code = *code;
  if (out.status.empty()) {
    return std::nullopt;
  }
  return out;
}

struct PluginState
{
  std::string dll_name;
  std::string plugin_name;
  std::string status;
  std::int64_t error_code = 0;
  bool loaded = false;
};

}  // namespace

SkseLogSummary ParseSkseLog(std::string_view text)
{
  SkseLogSummary summary{};
  std::vector<PluginState> plugins;
  std::unordered_map<std::string, std::size_t> indexByDll;
  bool imageBaseSeen = false;

  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view line = text.substr(pos, end - pos);
    pos = end + 1u;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    if (line.substr(0, kRuntimeHeader.size()) == kRuntimeHeader) {
      summary.recognized = true;
      const std::string_view version = line.substr(kRuntimeHeader.size());
      summary.skse_version = std::string(version.substr(0, version.find(' ')));
      continue;
    }
    if (!imageBaseSeen && line.substr(0, kImageBase.size()) == kImageBase) {
      if (const auto base = ParseHex64(line.substr(kImageBase.size()))) {
        summary.image_base = *base;
        imageBaseSeen = true;
      }
      continue;
    }
    if (line.substr(0, kCheckingPlugin.size()) == kCheckingPlugin) {
      ++summary.checked_count;
      continue;
    }

    const auto parsed = ParsePluginStatusLine(line);
    if (!parsed) {
      continue;
    }
    const auto key = LowerAscii(parsed->dll_name);
    auto [it, inserted] = indexByDll.emplace(key, plugins.size());
    if (inserted) {
      plugins.push_back(PluginState{});
    }
    auto& state = plugins[it->second];
    state.dll_name = std::string(parsed->dll_name);
    if (!parsed->plugin_name.empty()) {
      state.plugin_name = std::string(parsed->plugin_name);
    }
    state.status = std::string(parsed->status);
    state.error_code = parsed->error_code;
    state.loaded = parsed->loaded;
  }

  for (auto& state : plugins) {
    if (state.loaded) {
      ++summary.loaded_count;
      continue;
    }
    SkseLogPluginIssue issue{};
    issue.dll_name = std::move(state.dll_name);
    issue.plugin_name = std::move(state.plugin_name);
    issue.status = std::move(state.status);
    issue.error_code = state.error_code;
    summary.issues.push_back(std::move(issue));
  }
  return summary;
}

}  // namespace skydiag::helper
