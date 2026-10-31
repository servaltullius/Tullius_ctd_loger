#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace skydiag {

// The log of an earlier session next to `log` ("X.log"): "X.previous.log"
// for the last one, "X.previous.<age>.log" before it.
inline std::filesystem::path PreviousSessionLogPath(const std::filesystem::path& log, std::uint32_t age)
{
  auto name = log.stem();
  name += age <= 1u ? std::string(".previous.log") : ".previous." + std::to_string(age) + ".log";
  return log.parent_path() / name;
}

// Moves the last session's log aside before a session starts it again:
// earlier ones shift down (previous.log -> previous.2.log -> ...) and `keep`
// of them stay. A test or a crash is often followed by another launch before
// anyone reads the log. An empty or missing log is left alone. False when
// the log could not be moved (held open elsewhere); the caller starts over
// in it then, as without this.
inline bool KeepSessionLog(const std::filesystem::path& log, std::uint32_t keep)
{
  std::error_code ec;
  if (!std::filesystem::exists(log, ec) || std::filesystem::file_size(log, ec) == 0) {
    return true;
  }
  keep = (std::max)(1u, keep);
  std::filesystem::remove(PreviousSessionLogPath(log, keep), ec);
  for (std::uint32_t age = keep; age > 1u; --age) {
    const auto older = PreviousSessionLogPath(log, age - 1u);
    if (std::filesystem::exists(older, ec)) {
      std::filesystem::rename(older, PreviousSessionLogPath(log, age), ec);
    }
  }
  const auto previous = PreviousSessionLogPath(log, 1u);
  std::filesystem::remove(previous, ec);
  ec.clear();
  std::filesystem::rename(log, previous, ec);
  return !ec;
}

}  // namespace skydiag
