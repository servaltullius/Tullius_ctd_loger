#include "GpuDriverLog.h"

#include <Windows.h>

#include <DbgHelp.h>

#include <algorithm>
#include <vector>

#include <nlohmann/json.hpp>

#include "JsonValue.h"
#include "MinidumpUtil.h"
#include "SkyrimDiagProtocol.h"
#include "Utf.h"

namespace skydiag::dump_tool {

namespace {

constexpr std::size_t kGroupsShown = 3;
constexpr std::size_t kMessageCharsShown = 120;

bool IsProblem(const GpuDriverEventRow& event)
{
  return event.level >= 1u && event.level <= 3u;
}

std::wstring Ago(std::uint64_t seconds, bool en)
{
  if (seconds < 120u) {
    return en ? (std::to_wstring(seconds) + L" s") : (std::to_wstring(seconds) + L"초");
  }
  if (seconds < std::uint64_t{ 2 } * 3600u) {
    return en ? (std::to_wstring(seconds / 60u) + L" min") : (std::to_wstring(seconds / 60u) + L"분");
  }
  return en ? (std::to_wstring(seconds / 3600u) + L" h") : (std::to_wstring(seconds / 3600u) + L"시간");
}

}  // namespace

bool ApplyGpuDriverLogJson(std::string_view json, GpuDriverLog& out)
{
  const auto j = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
  if (j.is_discarded() || !j.is_object() || JsonValueOr(j, "version", 0u) != 1u || !j.contains("events") ||
      !j["events"].is_array()) {
    return false;
  }
  GpuDriverLog log{};
  log.recorded = true;
  log.since_unix = JsonValueOr(j, "since_unix", std::uint64_t{ 0 });
  log.until_unix = JsonValueOr(j, "until_unix", std::uint64_t{ 0 });
  log.total = JsonValueOr(j, "total", std::size_t{ 0 });
  for (const auto& e : j["events"]) {
    GpuDriverEventRow row{};
    row.provider = Utf8ToWide(JsonValueOr(e, "provider", std::string{}));
    row.id = JsonValueOr(e, "id", 0u);
    row.level = JsonValueOr(e, "level", 0u);
    row.unix = JsonValueOr(e, "unix", std::uint64_t{ 0 });
    row.message = Utf8ToWide(JsonValueOr(e, "message", std::string{}));
    if (!row.provider.empty()) {
      log.events.push_back(std::move(row));
    }
  }
  log.total = std::max(log.total, log.events.size());
  out = std::move(log);
  return true;
}

void ReadGpuDriverLog(void* dumpBase, std::uint64_t dumpSize, GpuDriverLog& out)
{
  void* ptr = nullptr;
  ULONG size = 0;
  if (minidump::ReadStreamSized(dumpBase, dumpSize, skydiag::protocol::kMinidumpUserStream_GpuDriverEvents, &ptr, &size) &&
      ptr && size > 0) {
    ApplyGpuDriverLogJson(std::string_view(static_cast<const char*>(ptr), size), out);
  }
}

std::size_t GpuDriverProblemCount(const GpuDriverLog& log)
{
  return static_cast<std::size_t>(std::count_if(log.events.begin(), log.events.end(), IsProblem));
}

std::wstring DescribeGpuDriverLog(const GpuDriverLog& log, std::uint64_t captureUnix, bool en)
{
  if (!log.recorded) {
    return {};
  }
  if (log.total == 0u) {
    return en ? L"none since the game started" : L"게임 시작 이후 없음";
  }
  // One group per driver and event ID, the most recent first.
  struct Group
  {
    const GpuDriverEventRow* newest = nullptr;
    std::size_t count = 0;
  };
  std::vector<Group> groups;
  for (auto it = log.events.rbegin(); it != log.events.rend(); ++it) {
    auto found = std::find_if(groups.begin(), groups.end(), [&](const Group& g) {
      return g.newest->provider == it->provider && g.newest->id == it->id;
    });
    if (found == groups.end()) {
      groups.push_back({ &*it, 1u });
    } else {
      ++found->count;
    }
  }
  std::wstring text = en ? (std::to_wstring(log.total) + L" since the game started")
                         : (L"게임 시작 이후 " + std::to_wstring(log.total) + L"건");
  for (std::size_t i = 0; i < groups.size() && i < kGroupsShown; ++i) {
    const auto& g = groups[i];
    std::wstring message = g.newest->message.substr(0, kMessageCharsShown);
    text += (i == 0u ? L": " : L", ") + g.newest->provider + L" " + std::to_wstring(g.newest->id) + L" " +
      (en ? (L"x" + std::to_wstring(g.count)) : (std::to_wstring(g.count) + L"번")) +
      (message.empty() ? L"" : ((en ? L" (" : L"(") + message + L")"));
  }
  if (groups.size() > kGroupsShown) {
    text += en ? L", ..." : L" 외";
  }
  if (log.total > log.events.size()) {
    text += en ? (L" (the newest " + std::to_wstring(log.events.size()) + L" kept)")
               : (L" (최근 " + std::to_wstring(log.events.size()) + L"건만 남김)");
  }
  if (!log.events.empty() && captureUnix != 0u && log.events.back().unix != 0u && log.events.back().unix <= captureUnix) {
    const auto ago = Ago(captureUnix - log.events.back().unix, en);
    text += en ? (L", the last " + ago + L" before the capture") : (L", 마지막은 캡처 " + ago + L" 전");
  }
  return text;
}

}  // namespace skydiag::dump_tool
