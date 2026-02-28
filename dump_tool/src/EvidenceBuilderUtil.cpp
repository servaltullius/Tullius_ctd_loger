#include "EvidenceBuilderPrivate.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "MinidumpUtil.h"
#include "SkyrimDiagShared.h"
#include "SkyrimDiagStringUtil.h"

namespace skydiag::dump_tool::internal {

using skydiag::WideLower;

std::wstring ConfidenceText(i18n::Language lang, i18n::ConfidenceLevel level)
{
  return std::wstring(i18n::ConfidenceLabel(lang, level));
}

std::wstring JoinList(const std::vector<std::wstring>& items, std::size_t maxN, std::wstring_view sep)
{
  if (items.empty() || maxN == 0) {
    return {};
  }
  const std::size_t n = std::min<std::size_t>(items.size(), maxN);
  std::wstring out;
  for (std::size_t i = 0; i < n; i++) {
    if (i > 0) {
      out += sep;
    }
    out += items[i];
  }
  if (items.size() > n) {
    out += sep;
    out += L"...";
  }
  return out;
}

std::wstring ToWideAscii(std::string_view s)
{
  std::wstring out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    out.push_back(static_cast<wchar_t>(c));
  }
  return out;
}

std::wstring Hex64(std::uint64_t v)
{
  wchar_t buf[32]{};
  swprintf_s(buf, L"0x%llX", static_cast<unsigned long long>(v));
  return buf;
}

std::optional<std::wstring> TryExplainExceptionInfo(const AnalysisResult& r, bool en)
{
  if (r.exc_info.empty()) {
    return std::nullopt;
  }

  auto accessKind = [&](std::uint64_t k) -> std::wstring {
    switch (k) {
      case 0:
        return en ? L"read" : L"읽기";
      case 1:
        return en ? L"write" : L"쓰기";
      case 8:
        return en ? L"execute" : L"실행";
      default:
        return en ? L"unknown" : L"알 수 없음";
    }
  };

  // https://learn.microsoft.com/en-us/windows/win32/debug/structured-exception-handling
  // EXCEPTION_ACCESS_VIOLATION (0xC0000005):
  //   [0] = 0 read, 1 write, 8 execute
  //   [1] = address being accessed
  if (r.exc_code == 0xC0000005u && r.exc_info.size() >= 2) {
    const auto kind = accessKind(r.exc_info[0]);
    const auto addr = Hex64(r.exc_info[1]);
    return en
      ? (L"EXCEPTION_ACCESS_VIOLATION: " + kind + L" at " + addr)
      : (L"접근 위반: " + kind + L" 주소=" + addr);
  }

  // EXCEPTION_IN_PAGE_ERROR (0xC0000006):
  //   [0] = 0 read, 1 write, 8 execute
  //   [1] = address being accessed
  //   [2] = NTSTATUS
  if (r.exc_code == 0xC0000006u && r.exc_info.size() >= 3) {
    const auto kind = accessKind(r.exc_info[0]);
    const auto addr = Hex64(r.exc_info[1]);
    const auto status = Hex64(r.exc_info[2]);
    return en
      ? (L"EXCEPTION_IN_PAGE_ERROR: " + kind + L" at " + addr + L" (NTSTATUS " + status + L")")
      : (L"페이지 오류: " + kind + L" 주소=" + addr + L" (NTSTATUS " + status + L")");
  }

  return std::nullopt;
}


std::optional<WctInfo> TrySummarizeWct(std::string_view utf8)
{
  if (utf8.empty()) {
    return std::nullopt;
  }

  WctInfo info{};
  try {
    const auto j = nlohmann::json::parse(utf8, nullptr, /*allow_exceptions=*/true);
    if (!j.is_object()) {
      return std::nullopt;
    }
    const auto it = j.find("threads");
    if (it == j.end() || !it->is_array()) {
      return std::nullopt;
    }
    info.threads = static_cast<int>(it->size());
    for (const auto& t : *it) {
      if (t.is_object() && t.contains("isCycle") && t["isCycle"].is_boolean() && t["isCycle"].get<bool>()) {
        info.cycles++;
      }
    }

    const auto capIt = j.find("capture");
    if (capIt != j.end() && capIt->is_object()) {
      info.has_capture = true;
      info.capture_kind = capIt->value("kind", std::string{});
      info.secondsSinceHeartbeat = capIt->value("secondsSinceHeartbeat", 0.0);
      info.thresholdSec = capIt->value("thresholdSec", 0u);
      info.isLoading = capIt->value("isLoading", false);
      if (info.thresholdSec > 0u && info.secondsSinceHeartbeat >= static_cast<double>(info.thresholdSec)) {
        info.suggestsHang = true;
      }
    }

    return info;
  } catch (...) {
    return std::nullopt;
  }
}

HitchSummary ComputeHitchSummary(const std::vector<EventRow>& events)
{
  HitchSummary out{};
  std::vector<std::uint64_t> ms;
  for (const auto& e : events) {
    if (e.type != static_cast<std::uint16_t>(skydiag::EventType::kPerfHitch)) {
      continue;
    }
    if (e.a == 0) {
      continue;
    }
    ms.push_back(e.a);
    out.count++;
    out.maxMs = std::max<std::uint64_t>(out.maxMs, e.a);
  }

  if (ms.empty()) {
    return out;
  }

  std::sort(ms.begin(), ms.end());
  const std::size_t idx = (ms.size() - 1) * 95 / 100;
  out.p95Ms = ms[std::min<std::size_t>(idx, ms.size() - 1)];
  return out;
}

HitchSummary ComputeHitchSummaryInRange(const std::vector<EventRow>& events, double fromMs, double toMs)
{
  HitchSummary out{};
  std::vector<std::uint64_t> ms;
  for (const auto& e : events) {
    if (e.type != static_cast<std::uint16_t>(skydiag::EventType::kPerfHitch)) {
      continue;
    }
    if (e.a == 0) {
      continue;
    }
    if (e.t_ms < fromMs || e.t_ms > toMs) {
      continue;
    }
    ms.push_back(e.a);
    out.count++;
    out.maxMs = std::max<std::uint64_t>(out.maxMs, e.a);
  }

  if (ms.empty()) {
    return out;
  }

  std::sort(ms.begin(), ms.end());
  const std::size_t idx = (ms.size() - 1) * 95 / 100;
  out.p95Ms = ms[std::min<std::size_t>(idx, ms.size() - 1)];
  return out;
}

bool IsKeyResourceKind(std::wstring_view kind)
{
  // Focus on the most common "crash-prone" asset types in Skyrim modpacks.
  return kind == L"nif" || kind == L"hkx" || kind == L"tri";
}

std::optional<double> FindLastEventTimeMsByType(const std::vector<EventRow>& events, std::uint16_t type)
{
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    if (it->type == type) {
      return it->t_ms;
    }
  }
  return std::nullopt;
}

std::wstring BuildPreFreezeContextLine(const std::vector<EventRow>& events, bool en)
{
  if (events.empty()) return {};

  // Find the last PerfHitch with a >= 2000ms (big hitch = potential freeze)
  const EventRow* lastBigHitch = nullptr;
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    if (it->type == static_cast<std::uint16_t>(skydiag::EventType::kPerfHitch) && it->a >= 2000) {
      lastBigHitch = &(*it);
      break;
    }
  }

  if (!lastBigHitch) return {};

  // Collect interesting events in the 10 seconds before the big hitch
  const double hitchTime = lastBigHitch->t_ms;
  const double windowMs = 10000.0;

  std::vector<std::wstring> context;
  for (const auto& e : events) {
    if (e.t_ms > hitchTime) break;
    if (e.t_ms < hitchTime - windowMs) continue;
    if (&e == lastBigHitch) break;

    switch (static_cast<skydiag::EventType>(e.type)) {
      case skydiag::EventType::kMenuOpen:
      case skydiag::EventType::kMenuClose:
      case skydiag::EventType::kLoadStart:
      case skydiag::EventType::kLoadEnd:
      case skydiag::EventType::kCellChange:
      case skydiag::EventType::kPerfHitch: {
        std::wstring line = e.type_name;
        if (!e.detail.empty()) {
          line += L"(" + e.detail + L")";
        }
        context.push_back(std::move(line));
        break;
      }
      default:
        break;
    }
    if (context.size() >= 5) break;
  }

  if (context.empty()) return {};

  std::wstring result;
  for (std::size_t i = 0; i < context.size(); i++) {
    if (i > 0) result += L" \u2192 ";
    result += context[i];
  }
  result += L" \u2192 PerfHitch(" + lastBigHitch->detail + L")";

  return result;
}

std::optional<double> InferCaptureAnchorMs(const AnalysisResult& r)
{
  // Prefer explicit crash/hang marks when available, otherwise fall back to the last recorded timestamp.
  if (auto t = FindLastEventTimeMsByType(r.events, static_cast<std::uint16_t>(skydiag::EventType::kCrash))) {
    return t;
  }
  if (auto t = FindLastEventTimeMsByType(r.events, static_cast<std::uint16_t>(skydiag::EventType::kHangMark))) {
    return t;
  }
  if (!r.events.empty()) {
    return r.events.back().t_ms;
  }
  if (!r.resources.empty()) {
    return r.resources.back().t_ms;
  }
  return std::nullopt;
}

std::optional<double> InferHeartbeatAgeFromResultSec(const AnalysisResult& r)
{
  const auto anchor = InferCaptureAnchorMs(r);
  if (!anchor) {
    return std::nullopt;
  }
  const auto hb = FindLastEventTimeMsByType(r.events, static_cast<std::uint16_t>(skydiag::EventType::kHeartbeat));
  if (!hb) {
    return std::nullopt;
  }
  const double ageMs = std::max(0.0, *anchor - *hb);
  return ageMs / 1000.0;
}

std::wstring FormatResourceHitLine(const ResourceRow& rr, double anchorMs)
{
  // Prefix with relative time to make correlation explicit in a single-line ListView cell.
  const double relMs = rr.t_ms - anchorMs;
  wchar_t buf[64]{};
  swprintf_s(buf, L"%+.0fms ", relMs);

  std::wstring line = buf;
  if (!rr.kind.empty() && rr.kind != L"(unknown)") {
    line += L"[";
    line += rr.kind;
    line += L"] ";
  }
  line += rr.path;
  if (rr.is_conflict && !rr.providers.empty()) {
    line += L" (providers: " + JoinList(rr.providers, 4, L", ") + L")";
  }
  return line;
}

std::vector<const ResourceRow*> FindResourcesNearAnchor(
  const std::vector<ResourceRow>& resources,
  double anchorMs,
  double windowBeforeMs,
  double windowAfterMs)
{
  std::vector<const ResourceRow*> hits;
  for (const auto& rr : resources) {
    if (!IsKeyResourceKind(rr.kind)) {
      continue;
    }
    if (rr.t_ms < (anchorMs - windowBeforeMs) || rr.t_ms > (anchorMs + windowAfterMs)) {
      continue;
    }
    hits.push_back(&rr);
  }

  std::sort(hits.begin(), hits.end(), [anchorMs](const ResourceRow* a, const ResourceRow* b) {
    double da = a->t_ms - anchorMs;
    if (da < 0) da = -da;
    double db = b->t_ms - anchorMs;
    if (db < 0) db = -db;
    return da < db;
  });

  constexpr std::size_t kMaxHits = 8;
  if (hits.size() > kMaxHits) {
    hits.resize(kMaxHits);
  }
  return hits;
}

std::vector<std::wstring> InferProviderScoresFromResources(const std::vector<const ResourceRow*>& hits)
{
  if (hits.empty()) {
    return {};
  }

  std::unordered_map<std::wstring, std::uint32_t> score;
  for (const auto* rr : hits) {
    if (!rr) {
      continue;
    }
    for (const auto& p : rr->providers) {
      score[p] += 1;
    }
  }

  struct Row
  {
    std::wstring name;
    std::uint32_t score = 0;
  };
  std::vector<Row> rows;
  rows.reserve(score.size());
  for (auto& [k, v] : score) {
    rows.push_back(Row{ std::move(k), v });
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.score > b.score; });

  std::vector<std::wstring> out;
  out.reserve(std::min<std::size_t>(rows.size(), 5));
  for (const auto& r : rows) {
    out.push_back(r.name + L" (" + std::to_wstring(r.score) + L")");
    if (out.size() >= 5) {
      break;
    }
  }
  return out;
}

std::vector<std::wstring> InferPerfSuspectsFromResourceCorrelation(const std::vector<EventRow>& events, const std::vector<ResourceRow>& resources)
{
  // Correlate hitch timestamps with nearby resource loads and their providers (MO2).
  // Heuristic only; treat as "possible suspects", not proof.
  if (events.empty() || resources.empty()) {
    return {};
  }

  constexpr double kWindowBeforeMs = 1500.0;
  constexpr double kWindowAfterMs = 150.0;

  std::unordered_map<std::wstring, std::uint32_t> score;
  for (const auto& ev : events) {
    if (ev.type != static_cast<std::uint16_t>(skydiag::EventType::kPerfHitch)) {
      continue;
    }
    const double t = ev.t_ms;
    const double from = t - kWindowBeforeMs;
    const double to = t + kWindowAfterMs;

    for (const auto& rr : resources) {
      if (rr.t_ms < from || rr.t_ms > to) {
        continue;
      }
      for (const auto& p : rr.providers) {
        score[p] += 1;
      }
    }
  }

  struct Row
  {
    std::wstring name;
    std::uint32_t score = 0;
  };
  std::vector<Row> rows;
  rows.reserve(score.size());
  for (auto& [k, v] : score) {
    rows.push_back(Row{ std::move(k), v });
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.score > b.score; });

  std::vector<std::wstring> out;
  out.reserve(std::min<std::size_t>(rows.size(), 5));
  for (const auto& r : rows) {
    out.push_back(r.name + L" (" + std::to_wstring(r.score) + L")");
    if (out.size() >= 5) {
      break;
    }
  }
  return out;
}

}  // namespace skydiag::dump_tool::internal
