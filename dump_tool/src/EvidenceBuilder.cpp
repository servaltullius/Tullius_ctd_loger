#include "EvidenceBuilder.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool {
namespace {

std::wstring ConfidenceHigh() { return L"높음"; }
std::wstring ConfidenceMid() { return L"중간"; }
std::wstring ConfidenceLow() { return L"낮음"; }

std::wstring WideLower(std::wstring_view s)
{
  std::wstring out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  return out;
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

bool IsSystemishModule(std::wstring_view filename)
{
  std::wstring lower(filename);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

  const wchar_t* k[] = {
    L"kernelbase.dll", L"ntdll.dll",     L"kernel32.dll",  L"ucrtbase.dll",
    L"msvcp140.dll",   L"vcruntime140.dll", L"vcruntime140_1.dll", L"concrt140.dll", L"user32.dll",
    L"gdi32.dll",      L"combase.dll",   L"ole32.dll",     L"ws2_32.dll",
  };
  for (const auto* m : k) {
    if (lower == m) {
      return true;
    }
  }
  return false;
}

bool IsGameExeModule(std::wstring_view filename)
{
  const std::wstring lower = WideLower(filename);
  return (lower == L"skyrimse.exe" || lower == L"skyrimae.exe" || lower == L"skyrimvr.exe" || lower == L"skyrim.exe");
}

struct WctInfo
{
  int threads = 0;
  int cycles = 0;
  bool has_capture = false;
  std::string capture_kind;
  double secondsSinceHeartbeat = 0.0;
  std::uint32_t thresholdSec = 0;
  bool isLoading = false;
  bool suggestsHang = false;
};

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

struct HitchSummary
{
  std::uint32_t count = 0;
  std::uint64_t maxMs = 0;
  std::uint64_t p95Ms = 0;
};

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

std::vector<const ResourceRow*> FindResourcesNearAnchor(const std::vector<ResourceRow>& resources, double anchorMs, double windowBeforeMs, double windowAfterMs)
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

}  // namespace

void BuildEvidenceAndSummary(AnalysisResult& r)
{
  r.evidence.clear();
  r.recommendations.clear();

  // Capture type (best-effort from filename / streams)
  std::wstring lowerName = std::filesystem::path(r.dump_path).filename().wstring();
  std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  const bool nameCrash = (lowerName.find(L"_crash_") != std::wstring::npos);
  const bool nameHang = (lowerName.find(L"_hang_") != std::wstring::npos);
  const bool nameManual = (lowerName.find(L"_manual_") != std::wstring::npos);

  bool hasCrashEvent = false;
  bool hasHangEvent = false;
  for (const auto& ev : r.events) {
    if (ev.type == static_cast<std::uint16_t>(skydiag::EventType::kCrash)) {
      hasCrashEvent = true;
    }
    if (ev.type == static_cast<std::uint16_t>(skydiag::EventType::kHangMark)) {
      hasHangEvent = true;
    }
  }

  const bool hasException = (r.exc_code != 0u);
  const bool isCrashLike = nameCrash || hasException || hasCrashEvent;
  const auto wct = (r.has_wct && !r.wct_json_utf8.empty()) ? TrySummarizeWct(r.wct_json_utf8) : std::nullopt;
  constexpr double kNotHangHeartbeatAgeSec = 5.0;
  const auto hbAge = InferHeartbeatAgeFromResultSec(r);
  const bool heartbeatSuggestsNotHang = hbAge && (*hbAge < kNotHangHeartbeatAgeSec);
  const bool manualFromWct = wct && wct->has_capture && (wct->capture_kind == "manual");
  const bool wctSuggestsHang = wct && ((wct->cycles > 0) || wct->suggestsHang);
  const bool nameHangEffective = nameHang && !manualFromWct && !heartbeatSuggestsNotHang;
  const bool isHangLike = nameHangEffective || hasHangEvent || wctSuggestsHang;
  const bool isSnapshotLike = !isCrashLike && !isHangLike;
  const bool isManualCapture = nameManual || manualFromWct || (nameHang && isSnapshotLike);

  const bool isSystem = IsSystemishModule(r.fault_module_filename);
  const bool hasModule = !r.fault_module_filename.empty();
  const bool isGameExe = IsGameExeModule(r.fault_module_filename);
  const auto hitch = ComputeHitchSummary(r.events);
  const std::wstring suspectBasis = r.suspects_from_stackwalk ? L"콜스택" : L"스택 스캔";

  if (isSnapshotLike) {
    EvidenceItem e{};
    e.confidence = ConfidenceHigh();
    e.title = L"이 덤프는 크래시 덤프가 아니라 '상태 스냅샷'으로 보임";
    e.details = isManualCapture
      ? L"수동 캡처로 추정됩니다. 이 결과만으로 '문제가 있다'고 단정할 수 없습니다. (상태 확인용)"
      : L"크래시/행 신호 없이 캡처된 덤프입니다. 원인 확정용이 아니라 '상태 확인용'입니다.";
    r.evidence.push_back(std::move(e));
  }

  if (!r.crash_logger_log_path.empty()) {
    EvidenceItem e{};
    e.confidence = ConfidenceMid();
    e.title = L"Crash Logger SSE/AE 로그를 자동으로 찾음";
    e.details = std::filesystem::path(r.crash_logger_log_path).filename().wstring();
    r.evidence.push_back(std::move(e));
  }

  if (!r.crash_logger_top_modules.empty()) {
    EvidenceItem e{};
    e.confidence = ConfidenceMid();
    e.title = L"Crash Logger 콜스택 상위 모듈";
    e.details = JoinList(r.crash_logger_top_modules, 4, L", ");
    r.evidence.push_back(std::move(e));
  }

  if (!r.stackwalk_primary_frames.empty()) {
    EvidenceItem e{};
    e.confidence = !r.suspects.empty() ? (r.suspects[0].confidence.empty() ? ConfidenceMid() : r.suspects[0].confidence) : ConfidenceLow();
    e.title = L"콜스택(대표 스레드) 상위 프레임";
    e.details = L"tid=" + std::to_wstring(r.stackwalk_primary_tid) + L": " + JoinList(r.stackwalk_primary_frames, 4, L" | ");
    r.evidence.push_back(std::move(e));
  }

  if (!r.suspects.empty()) {
    EvidenceItem e{};
    e.confidence = r.suspects[0].confidence.empty() ? ConfidenceMid() : r.suspects[0].confidence;
    e.title = r.suspects_from_stackwalk ? L"콜스택 기반 유력 후보" : L"스택 스캔 기반 유력 후보";

    std::vector<std::wstring> display;
    for (const auto& s : r.suspects) {
      std::wstring who;
      if (!s.inferred_mod_name.empty()) {
        who = s.inferred_mod_name + L" (" + s.module_filename + L")";
      } else {
        who = s.module_filename;
      }
      display.push_back(who);
      if (display.size() >= 3) {
        break;
      }
    }
    e.details = JoinList(display, 3, L", ");
    r.evidence.push_back(std::move(e));
  }

  if (!r.resources.empty()) {
    std::vector<std::wstring> recent;
    const std::size_t n = std::min<std::size_t>(r.resources.size(), 4);
    recent.reserve(n);
    for (std::size_t i = r.resources.size() - n; i < r.resources.size(); i++) {
      const auto& rr = r.resources[i];
      std::wstring line = rr.path;
      if (!rr.kind.empty() && rr.kind != L"(unknown)") {
        line = L"[" + rr.kind + L"] " + line;
      }
      if (rr.is_conflict && !rr.providers.empty()) {
        line += L" (providers: " + JoinList(rr.providers, 4, L", ") + L")";
      }
      recent.push_back(std::move(line));
    }

    EvidenceItem e{};
    e.confidence = ConfidenceMid();
    e.title = L"최근 로드된 리소스(메쉬/애니) 기록";
    e.details = JoinList(recent, 4, L", ");
    r.evidence.push_back(std::move(e));

    std::vector<std::wstring> conflicts;
    for (const auto& rr : r.resources) {
      if (!rr.is_conflict || rr.providers.size() < 2) {
        continue;
      }
      std::wstring who = rr.path + L" <= " + JoinList(rr.providers, 6, L", ");
      conflicts.push_back(std::move(who));
      if (conflicts.size() >= 4) {
        break;
      }
    }

    if (!conflicts.empty()) {
      EvidenceItem c{};
      c.confidence = ConfidenceMid();
      c.title = L"동일 파일을 여러 모드가 제공(충돌 가능)";
      c.details = JoinList(conflicts, 4, L" | ");
      r.evidence.push_back(std::move(c));
    }

    // For crashes/hangs, highlight resources that happened closest to capture time.
    if (isCrashLike || isHangLike) {
      if (auto anchorMs = InferCaptureAnchorMs(r)) {
        const double windowBeforeMs = ((r.state_flags & skydiag::kState_Loading) != 0u) ? 15000.0 : 5000.0;
        const double windowAfterMs = 300.0;
        const auto hits = FindResourcesNearAnchor(r.resources, *anchorMs, windowBeforeMs, windowAfterMs);
        if (!hits.empty()) {
          std::vector<std::wstring> lines;
          lines.reserve(std::min<std::size_t>(hits.size(), 4));
          for (const auto* rr : hits) {
            if (!rr) {
              continue;
            }
            lines.push_back(FormatResourceHitLine(*rr, *anchorMs));
            if (lines.size() >= 4) {
              break;
            }
          }

          if (!lines.empty()) {
            EvidenceItem e{};
            e.confidence = ConfidenceLow();
            e.title = isCrashLike
              ? L"크래시 직전/직후 로드된 리소스(메쉬/애니) 추정"
              : L"프리징/무한로딩 시점 근처 로드된 리소스(메쉬/애니) 추정";
            e.details = JoinList(lines, 4, L" | ");
            r.evidence.push_back(std::move(e));
          }

          std::vector<std::wstring> nearConflicts;
          for (const auto* rr : hits) {
            if (!rr || !rr->is_conflict || rr->providers.size() < 2) {
              continue;
            }
            nearConflicts.push_back(FormatResourceHitLine(*rr, *anchorMs));
            if (nearConflicts.size() >= 3) {
              break;
            }
          }
          if (!nearConflicts.empty()) {
            EvidenceItem c{};
            c.confidence = ConfidenceMid();
            c.title = L"시점 근처 리소스가 여러 모드에 존재(충돌 가능)";
            c.details = JoinList(nearConflicts, 3, L" | ");
            r.evidence.push_back(std::move(c));
          }

          const auto suspects = InferProviderScoresFromResources(hits);
          if (!suspects.empty()) {
            EvidenceItem s{};
            s.confidence = ConfidenceLow();
            s.title = L"시점 근처 리소스를 제공한 모드(상관분석)";
            s.details = JoinList(suspects, 5, L", ");
            r.evidence.push_back(std::move(s));
          }
        }
      }
    }
  }

  if (hitch.count > 0) {
    EvidenceItem e{};
    e.confidence = ConfidenceMid();
    e.title = L"끊김/프레임 드랍(히치) 감지";
    e.details = L"count=" + std::to_wstring(hitch.count) +
      L", max=" + std::to_wstring(hitch.maxMs) + L"ms, p95=" + std::to_wstring(hitch.p95Ms) + L"ms";
    r.evidence.push_back(std::move(e));

    if (!r.resources.empty()) {
      const auto suspects = InferPerfSuspectsFromResourceCorrelation(r.events, r.resources);
      if (!suspects.empty()) {
        EvidenceItem s{};
        s.confidence = ConfidenceLow();
        s.title = L"히치 시점 근처 리소스를 제공한 모드(상관분석)";
        s.details = JoinList(suspects, 5, L", ");
        r.evidence.push_back(std::move(s));
      }
    }
  }

  if (hasModule && !isSystem && !isGameExe) {
    EvidenceItem e{};
    e.confidence = ConfidenceHigh();
    e.title = L"크래시가 특정 DLL 내부에서 발생";
    e.details = L"예외 주소가 " + r.fault_module_filename + L" 범위에 포함됩니다. (Module+Offset: " + r.fault_module_plus_offset + L")";
    r.evidence.push_back(std::move(e));
  } else if (hasModule && isSystem) {
    EvidenceItem e{};
    e.confidence = ConfidenceLow();
    e.title = L"크래시가 Windows 시스템 DLL에서 보고됨";
    e.details =
      L"예외 주소가 " + r.fault_module_filename +
      L" 에서 보고됩니다. 이 경우 실제 원인은 다른 DLL/모드일 수 있습니다. (Module+Offset: " + r.fault_module_plus_offset + L")";
    r.evidence.push_back(std::move(e));
  } else if (!hasModule) {
    EvidenceItem e{};
    e.confidence = ConfidenceLow();
    e.title = L"fault module을 특정하지 못함";
    e.details = L"덤프에 모듈 목록/예외 정보가 부족할 수 있습니다.";
    r.evidence.push_back(std::move(e));
  }

  if (!r.inferred_mod_name.empty()) {
    EvidenceItem e{};
    e.confidence = ConfidenceMid();
    e.title = L"MO2 모드 폴더 경로에서 모드명 추정";
    e.details = L"모듈 경로에 \\mods\\<모드명>\\ 패턴이 있어 '" + r.inferred_mod_name + L"' 로 추정했습니다.";
    r.evidence.push_back(std::move(e));
  }

  if ((r.state_flags & skydiag::kState_Loading) != 0u) {
    EvidenceItem e{};
    e.confidence = ConfidenceMid();
    e.title = L"크래시 당시 로딩 상태로 추정";
    e.details = L"state_flags에 Loading 플래그가 설정되어 있습니다. (메쉬/텍스처/스크립트 초기화 단계일 수 있음)";
    r.evidence.push_back(std::move(e));
  }

  if (r.has_wct) {
    EvidenceItem e{};
    if (isSnapshotLike && isManualCapture && !wctSuggestsHang) {
      e.confidence = ConfidenceLow();
      e.title = L"WCT(Wait Chain) 스냅샷(수동 캡처)";
      e.details = L"수동 캡처에는 WCT가 항상 포함됩니다. 이것만으로 프리징/무한로딩을 의미하지 않습니다.";
    } else {
      e.confidence = ConfidenceMid();
      e.title = L"WCT(Wait Chain) 정보 포함";
      e.details = L"프리징/무한로딩처럼 멈춘 경우, 어떤 스레드가 무엇을 기다리는지 단서를 제공합니다.";
    }
    r.evidence.push_back(std::move(e));
  }

  if (wct) {
    EvidenceItem e{};
    e.confidence = ConfidenceMid();
    e.title = L"WCT 요약";
    wchar_t buf[512]{};
    if (wct->has_capture && wct->thresholdSec > 0u) {
      const std::wstring kindW = ToWideAscii(wct->capture_kind);
      swprintf_s(
        buf,
        L"capture=%s, threads=%d, cycleThreads=%d, heartbeatAge=%.1fs (threshold=%us, loading=%d)",
        kindW.c_str(),
        wct->threads,
        wct->cycles,
        wct->secondsSinceHeartbeat,
        wct->thresholdSec,
        wct->isLoading ? 1 : 0);
    } else {
      swprintf_s(buf, L"threads=%d, cycleThreads=%d", wct->threads, wct->cycles);
    }
    e.details = buf;
    r.evidence.push_back(std::move(e));
  }

  // Recommendations (checklist)
  if (isSnapshotLike) {
    r.recommendations.push_back(L"[정상/스냅샷] 예외(크래시) 정보가 없습니다. 이 덤프만으로 '어떤 모드가 크래시 원인'인지 판단하기 어렵습니다.");
    r.recommendations.push_back(L"[정상/스냅샷] 문제 상황에서 캡처해야 진단이 가능합니다: (1) 실제 크래시 덤프, (2) 프리징/무한로딩 중 수동 캡처(Ctrl+Shift+F12) 또는 자동 감지 덤프");
  }

  if (r.exc_code != 0) {
    if (r.exc_code == 0xC0000005u) {
      r.recommendations.push_back(L"[기본] ExceptionCode=0xC0000005(접근 위반)입니다. 보통 DLL 후킹/메모리 접근 문제로 발생합니다.");
    } else {
      wchar_t buf[128]{};
      swprintf_s(buf, L"[기본] ExceptionCode=0x%08X 입니다.", r.exc_code);
      r.recommendations.push_back(buf);
    }

    if (r.exc_code == 0xE06D7363u) {
      r.recommendations.push_back(L"[해석] 0xE06D7363은 흔한 C++ 예외(throw) 코드입니다. 정상 동작 중에도 throw/catch로 발생할 수 있습니다.");
      r.recommendations.push_back(L"[해석] 게임이 실제로 튕기지 않았다면, 이 덤프는 '실제 CTD'가 아니라 'handled exception 오탐'일 수 있습니다.");
      r.recommendations.push_back(L"[설정] SkyrimDiag.ini의 CrashHookMode=1(치명 예외만)로 두면 이런 오탐을 크게 줄일 수 있습니다.");
    }
  }

  if (!r.inferred_mod_name.empty()) {
    r.recommendations.push_back(L"[유력 후보] '" + r.inferred_mod_name + L"' 모드를 업데이트/재설치 후 재현 여부 확인");
    r.recommendations.push_back(L"[유력 후보] 동일 크래시가 반복되면 '" + r.inferred_mod_name + L"' 모드(또는 해당 모드의 SKSE 플러그인 DLL)를 비활성화 후 재현 여부 확인");
  }

  if (r.inferred_mod_name.empty() && !r.suspects.empty()) {
    const auto& s0 = r.suspects[0];
    if (!s0.inferred_mod_name.empty()) {
      r.recommendations.push_back(L"[유력 후보] " + suspectBasis + L" 기반 후보: '" + s0.inferred_mod_name + L"' 모드 업데이트/재설치 후 재현 여부 확인");
      r.recommendations.push_back(L"[유력 후보] 동일 문제가 반복되면 '" + s0.inferred_mod_name + L"' 모드(또는 해당 모드의 SKSE 플러그인 DLL)를 비활성화 후 재현 여부 확인");
    } else if (!s0.module_filename.empty()) {
      r.recommendations.push_back(L"[유력 후보] " + suspectBasis + L" 기반 후보 DLL: " + s0.module_filename + L" — 포함된 모드를 우선 점검");
    }
  }

  if (!r.resources.empty()) {
    bool hasConflict = false;
    for (const auto& rr : r.resources) {
      if (rr.is_conflict) {
        hasConflict = true;
        break;
      }
    }

    r.recommendations.push_back(L"[메쉬/애니] 이 덤프에는 최근 로드된 리소스(.nif/.hkx/.tri) 기록이 포함되어 있습니다. '최근 로드된 리소스' 항목을 확인하세요.");
    if (hasConflict) {
      r.recommendations.push_back(L"[충돌] 같은 파일을 제공하는 모드가 2개 이상이면 충돌 가능성이 큽니다. MO2에서 우선순위(모드 순서) 조정/비활성화로 재현 여부 확인");
    }
  }

  if (hitch.count > 0) {
    r.recommendations.push_back(L"[성능] PerfHitch 이벤트가 기록되었습니다. 이벤트 탭에서 t_ms와 hitch(ms)를 확인해 '언제 끊기는지' 먼저 파악하세요.");
    if (!r.resources.empty()) {
      r.recommendations.push_back(L"[성능] 리소스 탭에서 히치 직전/직후 로드된 .nif/.hkx/.tri 및 제공 모드를 확인하세요. (상관관계 기반, 확정 아님)");
    }
  }

  if (hasModule && !isSystem && !isGameExe) {
    r.recommendations.push_back(L"[유력 후보] 해당 DLL이 포함된 모드의 선행 모드/요구 버전(SKSE/Address Library/엔진 버전) 충족 여부 확인");
    r.recommendations.push_back(L"[유력 후보] 이 리포트 파일(*_SkyrimDiagReport.txt)과 덤프(*.dmp)를 모드 제작자에게 첨부");
  } else if (hasModule && isGameExe) {
    r.recommendations.push_back(L"[점검] 크래시 위치가 게임 본체(EXE)로 나옵니다. Address Library/ SKSE 버전 불일치 또는 후킹 충돌 가능성이 큽니다.");
    r.recommendations.push_back(L"[점검] 최근 추가/업데이트한 SKSE 플러그인(DLL)부터 하나씩 제외하며 재현 여부 확인");
  } else if (hasModule && isSystem) {
    r.recommendations.push_back(L"[점검] Windows 시스템 DLL로 표시될 때는 실제 원인이 다른 모드/DLL인 경우가 많습니다.");
    r.recommendations.push_back(L"[점검] 최근 추가/업데이트한 SKSE 플러그인(DLL)부터 하나씩 제외하며 재현 여부 확인");
    r.recommendations.push_back(L"[점검] SKSE 버전/게임 버전(AE/SE/VR)/Address Library 버전이 서로 맞는지 확인");
  } else {
    if (!isSnapshotLike) {
      r.recommendations.push_back(L"[점검] 덤프에서 fault module을 특정하지 못했습니다. DumpMode를 2(FullMemory)로 올려 다시 캡처하면 단서가 늘 수 있습니다.");
    }
  }

  if ((r.state_flags & skydiag::kState_Loading) != 0u) {
    r.recommendations.push_back(L"[로딩 중] 로딩 화면/세이브 로드 직후 크래시는 애니메이션/메쉬/텍스처/스켈레톤/스크립트 초기화 쪽이 흔합니다.");
    r.recommendations.push_back(L"[로딩 중] 해당 시점에 개입하는 모드(애니메이션/스켈레톤/바디/물리/프리캐시)를 우선 점검");
  }

  if (r.has_wct) {
    if (isHangLike) {
      if (wct) {
        if (wct->cycles > 0) {
          r.recommendations.push_back(L"[프리징] WCT에서 isCycle=true 스레드가 감지되었습니다. 데드락 가능성이 높습니다.");
        } else {
          r.recommendations.push_back(L"[프리징] WCT cycle이 없으면 무한루프/바쁜 대기(busy wait) 가능성도 있습니다.");
        }
      }
      r.recommendations.push_back(L"[프리징] 프리징이 반복되면 문제 상황 직전에 실행된 이벤트(이벤트 탭)를 기준으로 관련 모드를 점검");
    } else if (isManualCapture && isSnapshotLike) {
      if (wct && wct->has_capture && wct->thresholdSec > 0u && wct->secondsSinceHeartbeat < static_cast<double>(wct->thresholdSec)) {
        wchar_t buf[256]{};
        swprintf_s(
          buf,
          L"[수동] 수동 캡처 당시 heartbeatAge=%.1fs < threshold=%us 이므로 '프리징/무한로딩'으로 판단되지 않습니다.",
          wct->secondsSinceHeartbeat,
          wct->thresholdSec);
        r.recommendations.push_back(buf);
      }
      r.recommendations.push_back(L"[수동] 수동 캡처에는 WCT가 포함됩니다. 실제 프리징/무한로딩 중 캡처한 덤프에서 WCT 탭을 참고하세요.");
    }
  }

  // Summary sentence
  const bool hasSuspect = !r.suspects.empty();
  std::wstring suspectWho;
  std::wstring suspectConf;
  if (hasSuspect) {
    const auto& s0 = r.suspects[0];
    suspectConf = s0.confidence.empty() ? ConfidenceMid() : s0.confidence;
    if (!s0.inferred_mod_name.empty()) {
      suspectWho = s0.inferred_mod_name + L" (" + s0.module_filename + L")";
    } else {
      suspectWho = s0.module_filename;
    }
  }

  std::wstring who;
  if (!r.inferred_mod_name.empty()) {
    who = r.inferred_mod_name + L" (" + r.fault_module_filename + L")";
  } else if (!r.fault_module_filename.empty()) {
    who = r.fault_module_filename;
  } else {
    who = L"(알 수 없음)";
  }

  if (isSnapshotLike) {
    r.summary_sentence = isManualCapture
      ? L"수동 캡처 스냅샷으로 보입니다. 이 결과만으로 '문제가 있다'고 단정할 수 없습니다. (신뢰도: 높음)"
      : L"스냅샷 덤프(크래시/행 아님)로 보입니다. 원인 판정용이 아니라 '상태 확인'에 유용합니다. (신뢰도: 높음)";
  } else if (hasModule && !isSystem && !isGameExe) {
    r.summary_sentence = L"유력 후보: " + who + L" — 해당 DLL 내부에서 크래시가 발생한 것으로 보입니다. (신뢰도: 높음)";
  } else if (hasModule && isSystem) {
    if (hasSuspect && !suspectWho.empty()) {
      r.summary_sentence =
        L"크래시가 Windows 시스템 DLL에서 보고되었지만, " + suspectBasis + L"에서는 " + suspectWho +
        L" 가 유력합니다. (신뢰도: " + suspectConf + L")";
    } else if (r.exc_code == 0xE06D7363u) {
      r.summary_sentence =
        L"0xE06D7363(C++ 예외)로 Windows 시스템 DLL에서 보고되었습니다. 정상 동작 중 throw/catch일 수도 있어 실제 CTD 여부 확인이 필요합니다. (신뢰도: 낮음)";
    } else {
      r.summary_sentence =
        L"크래시가 Windows 시스템 DLL에서 보고되었습니다. 실제 원인은 다른 모드/DLL일 수 있습니다. (신뢰도: 낮음)";
    }
  } else if (hasModule && isGameExe) {
    if (hasSuspect && !suspectWho.empty()) {
      r.summary_sentence =
        L"크래시 위치가 게임 본체(EXE)로 보고되었지만, " + suspectBasis + L"에서는 " + suspectWho +
        L" 가 유력합니다. (신뢰도: " + suspectConf + L")";
    } else {
      r.summary_sentence =
        L"크래시 위치가 게임 본체(EXE)로 보고되었습니다. 버전 불일치/후킹 충돌 가능성이 있습니다. (신뢰도: 중간)";
    }
  } else {
    if (isHangLike) {
      std::wstring hangPrefix = L"프리징/무한로딩으로 추정됩니다.";
      if (wct && wct->has_capture && wct->thresholdSec > 0u) {
        const std::wstring kindW = ToWideAscii(wct->capture_kind);
        wchar_t hb[256]{};
        swprintf_s(
          hb,
          L"프리징 감지(capture=%s, heartbeatAge=%.1fs >= %us).",
          kindW.c_str(),
          wct->secondsSinceHeartbeat,
          wct->thresholdSec);
        hangPrefix = hb;
      }

      if (hasSuspect && !suspectWho.empty()) {
        r.summary_sentence =
          hangPrefix + L" 후보: " + suspectWho + L" — " + suspectBasis + L" 기반 추정입니다. (신뢰도: " + suspectConf + L")";
      } else {
        r.summary_sentence = hangPrefix + L" 덤프만으로 후보를 특정하기 어렵습니다. (신뢰도: 낮음)";
      }
    } else {
      if (hasSuspect && !suspectWho.empty()) {
        r.summary_sentence = L"유력 후보: " + suspectWho + L" — " + suspectBasis + L" 기반 추정입니다. (신뢰도: " + suspectConf + L")";
      } else {
        r.summary_sentence = L"덤프만으로 유력 후보를 특정하기 어렵습니다. (신뢰도: 낮음)";
      }
    }
  }
}

}  // namespace skydiag::dump_tool
