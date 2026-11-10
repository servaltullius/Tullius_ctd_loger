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
  log.complete = JsonValueOr(j, "complete", true);
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

std::optional<GpuDriverLastProblem> LastGpuDriverProblemBefore(const GpuDriverLog& log, std::uint64_t captureUnix)
{
  if (!log.recorded || captureUnix == 0u) {
    return std::nullopt;
  }
  const GpuDriverEventRow* newest = nullptr;
  for (const auto& event : log.events) {
    // The crash's own log is read after the dump, so it can hold later events.
    if (IsProblem(event) && event.unix != 0u && event.unix <= captureUnix && (!newest || event.unix >= newest->unix)) {
      newest = &event;
    }
  }
  if (!newest) {
    return std::nullopt;
  }
  return GpuDriverLastProblem{ captureUnix - newest->unix, newest->provider + L" " + std::to_wstring(newest->id) };
}

std::wstring DescribeGpuDriverLog(const GpuDriverLog& log, std::uint64_t captureUnix, bool en)
{
  if (!log.recorded) {
    return {};
  }
  // Errors and warnings only: the helper asks for nothing else, and an
  // information event in an older stream says nothing about a fault, nor
  // should its time read as the last error's.
  std::vector<const GpuDriverEventRow*> problems;
  for (const auto& event : log.events) {
    if (IsProblem(event)) {
      problems.push_back(&event);
    }
  }
  // When every kept event is one, the helper's count is of them too.
  const std::size_t total = problems.size() == log.events.size() ? log.total : problems.size();
  if (total == 0u) {
    if (!log.complete) {
      return en ? L"none found, but the log could not be read to the end" : L"찾은 것 없음(기록을 끝까지 읽지 못함)";
    }
    return en ? L"none since the game started" : L"게임 시작 이후 없음";
  }
  // One group per driver and event ID, the most recent first.
  struct Group
  {
    const GpuDriverEventRow* newest = nullptr;
    std::size_t count = 0;
  };
  std::vector<Group> groups;
  for (auto it = problems.rbegin(); it != problems.rend(); ++it) {
    const GpuDriverEventRow* event = *it;
    auto found = std::find_if(groups.begin(), groups.end(), [&](const Group& g) {
      return g.newest->provider == event->provider && g.newest->id == event->id;
    });
    if (found == groups.end()) {
      groups.push_back({ event, 1u });
    } else {
      ++found->count;
    }
  }
  std::wstring text = en ? (std::to_wstring(total) + L" since the game started")
                         : (L"게임 시작 이후 " + std::to_wstring(total) + L"건");
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
  if (problems.empty()) {
    // The helper counted them, but none could be read into the record (its
    // count comes before an event is rendered).
    text += en ? L" (their details could not be read)" : L" (내용은 읽지 못함)";
  } else if (total > problems.size()) {
    text += en ? (L" (the newest " + std::to_wstring(problems.size()) + L" kept)")
               : (L" (최근 " + std::to_wstring(problems.size()) + L"건만 남김)");
  }
  if (!log.complete) {
    text += en ? L" (the log could not be read to the end)" : L" (기록을 끝까지 읽지 못함)";
  }
  if (!problems.empty()) {
    const auto* last = problems.back();
    if (captureUnix != 0u && last->unix != 0u && last->unix <= captureUnix) {
      const auto ago = Ago(captureUnix - last->unix, en);
      text += en ? (L", the last " + ago + L" before the capture") : (L", 마지막은 캡처 " + ago + L" 전");
    }
  }
  return text;
}

}  // namespace skydiag::dump_tool
