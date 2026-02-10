#include "Analyzer.h"
#include "Bucket.h"
#include "EvidenceBuilder.h"
#include "CrashLogger.h"
#include "CrashLoggerParseCore.h"
#include "AnalyzerInternals.h"
#include "MinidumpUtil.h"
#include "Mo2Index.h"
#include "Utf.h"

#include <Windows.h>

#include <DbgHelp.h>

#include <algorithm>
#include <cwctype>
#include <cstddef>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "SkyrimDiagProtocol.h"
#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool {

using skydiag::dump_tool::minidump::FindModuleIndexForAddress;
using skydiag::dump_tool::minidump::GetThreadStackBytes;
using skydiag::dump_tool::minidump::LoadAllModules;
using skydiag::dump_tool::minidump::LoadThreads;
using skydiag::dump_tool::minidump::MappedFile;
using skydiag::dump_tool::minidump::ModuleForAddress;
using skydiag::dump_tool::minidump::ModuleInfo;
using skydiag::dump_tool::minidump::ReadStreamSized;
using skydiag::dump_tool::minidump::ReadThreadContextWin64;
using skydiag::dump_tool::minidump::ThreadRecord;
using skydiag::dump_tool::minidump::WideLower;

bool AnalyzeDump(const std::wstring& dumpPath, const std::wstring& outDir, const AnalyzeOptions& opt, AnalysisResult& out, std::wstring* err)
{
  out = AnalysisResult{};
  out.language = opt.language;
  out.dump_path = dumpPath;
  out.out_dir = outDir;
  out.online_symbol_source_allowed = opt.allow_online_symbols;
  out.path_redaction_applied = opt.redact_paths;

  const std::wstring dumpNameLower = WideLower(std::filesystem::path(dumpPath).filename().wstring());
  const bool nameCrash = (dumpNameLower.find(L"_crash_") != std::wstring::npos);
  const bool nameHang = (dumpNameLower.find(L"_hang_") != std::wstring::npos);

  // Memory-map the dump to avoid loading large FullMemory dumps into RAM.
  MappedFile mf{};
  if (!mf.Open(dumpPath, err)) {
    return false;
  }
  const std::uint64_t dumpSize = mf.size;
  void* dumpBase = mf.view;

  const auto allModules = LoadAllModules(dumpBase, dumpSize);
  std::vector<std::wstring> modulePaths;
  modulePaths.reserve(allModules.size());
  for (const auto& m : allModules) {
    if (!m.path.empty()) {
      modulePaths.push_back(m.path);
    }
  }
  const auto mo2Index = TryBuildMo2IndexFromModulePaths(modulePaths);
  std::unordered_map<std::wstring, std::vector<std::wstring>> mo2ProvidersCache;

  // Exception info
  std::optional<CONTEXT> excCtx;
  void* excPtr = nullptr;
  ULONG excSize = 0;
  if (ReadStreamSized(dumpBase, dumpSize, ExceptionStream, &excPtr, &excSize) && excPtr && excSize >= sizeof(MINIDUMP_EXCEPTION_STREAM)) {
    const auto* es = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(excPtr);
    out.exc_code = es->ExceptionRecord.ExceptionCode;
    out.exc_tid = es->ThreadId;
    out.exc_addr = es->ExceptionRecord.ExceptionAddress;
    out.exc_info.clear();
    {
      const ULONG n = es->ExceptionRecord.NumberParameters;
      constexpr ULONG kMaxN =
        static_cast<ULONG>(sizeof(es->ExceptionRecord.ExceptionInformation) / sizeof(es->ExceptionRecord.ExceptionInformation[0]));
      const ULONG take = (n < kMaxN) ? n : kMaxN;
      out.exc_info.reserve(take);
      for (ULONG i = 0; i < take; i++) {
        out.exc_info.push_back(static_cast<std::uint64_t>(es->ExceptionRecord.ExceptionInformation[i]));
      }
    }
    CONTEXT ctx{};
    if (internal::TryReadContextFromLocation(dumpBase, dumpSize, es->ThreadContext, ctx)) {
      excCtx = ctx;
    }
  }

  // Fault module
  if (out.exc_addr != 0) {
    if (auto idx = FindModuleIndexForAddress(allModules, out.exc_addr)) {
      const auto& m = allModules[*idx];
      out.fault_module_path = m.path;
      out.fault_module_filename = m.filename;
      const std::uint64_t off = out.exc_addr - m.base;
      wchar_t buf[1024]{};
      swprintf_s(buf, L"%s+0x%llx", m.filename.c_str(), static_cast<unsigned long long>(off));
      out.fault_module_plus_offset = buf;
      out.inferred_mod_name = m.inferred_mod_name;
    } else if (auto m = ModuleForAddress(dumpBase, dumpSize, out.exc_addr)) {  // fallback
      out.fault_module_path = m->path;
      out.fault_module_filename = m->filename;
      out.fault_module_plus_offset = m->plusOffset;
      out.inferred_mod_name = InferMo2ModNameFromPath(out.fault_module_path);
    }
  }

  // SkyrimDiag blackbox (optional)
  void* bbPtr = nullptr;
  ULONG bbSize = 0;
  if (ReadStreamSized(dumpBase, dumpSize, skydiag::protocol::kMinidumpUserStream_Blackbox, &bbPtr, &bbSize) && bbPtr &&
      bbSize >= offsetof(skydiag::SharedLayout, resources)) {
    const auto* snap = static_cast<const skydiag::SharedLayout*>(bbPtr);
    const auto ver = snap->header.version;
    if (snap->header.magic == skydiag::kMagic && (ver == 1u || ver == skydiag::kVersion)) {
      out.has_blackbox = true;
      out.pid = snap->header.pid;
      out.state_flags = snap->header.state_flags;

      std::uint32_t cap = snap->header.capacity;
      if (cap == 0 || cap > skydiag::kEventCapacity) {
        cap = skydiag::kEventCapacity;
      }
      const std::uint64_t freq = snap->header.qpc_freq ? snap->header.qpc_freq : 1;
      const std::uint64_t start = snap->header.start_qpc;
      const std::uint32_t writeIndex = snap->header.write_index;
      const std::uint32_t begin = (writeIndex > cap) ? (writeIndex - cap) : 0;

      out.events.clear();
      out.events.reserve(static_cast<std::size_t>(std::min<std::uint32_t>(writeIndex, cap)));
      for (std::uint32_t i = begin; i < writeIndex; i++) {
        const auto& ev = snap->events[i % cap];
        const std::uint32_t seq1 = ev.seq;
        if ((seq1 & 1u) != 0u) {
          continue;
        }

        skydiag::BlackboxEvent tmp{};
        std::memcpy(&tmp, &ev, sizeof(tmp));
        const std::uint32_t seq2 = ev.seq;
        if (seq1 != seq2 || (seq2 & 1u) != 0u) {
          continue;
        }

        if (tmp.type == static_cast<std::uint16_t>(skydiag::EventType::kInvalid)) {
          continue;
        }

        EventRow row{};
        row.i = i;
        row.tid = tmp.tid;
        row.type = tmp.type;
        row.type_name = internal::EventTypeName(tmp.type);
        row.a = tmp.payload.a;
        row.b = tmp.payload.b;
        row.c = tmp.payload.c;
        row.d = tmp.payload.d;
        row.t_ms = (tmp.qpc >= start)
          ? (1000.0 * (static_cast<double>(tmp.qpc - start) / static_cast<double>(freq)))
          : 0.0;
        out.events.push_back(std::move(row));
      }

      // Resource log (optional; v2+ includes it after the blackbox events).
      out.resources.clear();
      if (bbSize >= sizeof(skydiag::SharedLayout)) {
        const auto& rl = snap->resources;
        const std::uint32_t rCap = skydiag::kResourceCapacity;
        const std::uint32_t rWrite = rl.write_index;
        const std::uint32_t rBegin = (rWrite > rCap) ? (rWrite - rCap) : 0;

        out.resources.reserve(static_cast<std::size_t>(std::min<std::uint32_t>(rWrite, rCap)));

        const auto normMo2Key = [](std::wstring_view relPath) {
          std::wstring key(relPath);
          while (!key.empty() && (key.front() == L'\\' || key.front() == L'/')) {
            key.erase(key.begin());
          }
          for (auto& ch : key) {
            if (ch == L'/') {
              ch = L'\\';
            }
            ch = static_cast<wchar_t>(towlower(ch));
          }
          return key;
        };

        for (std::uint32_t i = rBegin; i < rWrite; i++) {
          const auto& ent = rl.entries[i % rCap];
          const std::uint32_t seq1 = ent.seq;
          if ((seq1 & 1u) != 0u) {
            continue;
          }

          skydiag::ResourceEntry tmp{};
          std::memcpy(&tmp, &ent, sizeof(tmp));
          const std::uint32_t seq2 = ent.seq;
          if (seq1 != seq2 || (seq2 & 1u) != 0u) {
            continue;
          }

          const std::size_t maxN = sizeof(tmp.path_utf8);
          std::size_t len = 0;
          while (len < maxN && tmp.path_utf8[len] != '\0') {
            len++;
          }
          if (len == 0) {
            continue;
          }

          ResourceRow rr{};
          rr.tid = tmp.tid;
          rr.t_ms = (tmp.qpc >= start)
            ? (1000.0 * (static_cast<double>(tmp.qpc - start) / static_cast<double>(freq)))
            : 0.0;
          rr.path = Utf8ToWide(std::string_view(tmp.path_utf8, len));
          rr.kind = internal::ResourceKindFromPath(rr.path);
          if (mo2Index) {
            // Provider mapping can be expensive in large modpacks. Cache by normalized rel-path.
            const std::wstring key = normMo2Key(rr.path);
            auto it = mo2ProvidersCache.find(key);
            if (it == mo2ProvidersCache.end()) {
              rr.providers = FindMo2ProvidersForDataPath(*mo2Index, rr.path, /*maxProviders=*/8);
              it = mo2ProvidersCache.emplace(key, rr.providers).first;
            } else {
              rr.providers = it->second;
            }
            rr.is_conflict = rr.providers.size() >= 2;
          }
          out.resources.push_back(std::move(rr));
        }

        // Cap display size (best-effort) to keep UI/report manageable.
        constexpr std::size_t kMaxKeep = 80;
        if (out.resources.size() > kMaxKeep) {
          out.resources.erase(out.resources.begin(), out.resources.end() - kMaxKeep);
        }
      }
    }
  }

  // WCT stream (optional)
  void* wctPtr = nullptr;
  ULONG wctSize = 0;
  if (ReadStreamSized(dumpBase, dumpSize, skydiag::protocol::kMinidumpUserStream_WctJson, &wctPtr, &wctSize) && wctPtr && wctSize > 0) {
    out.has_wct = true;
    out.wct_json_utf8.assign(static_cast<const char*>(wctPtr), static_cast<std::size_t>(wctSize));
  }

  bool hasHangEvent = false;
  for (const auto& ev : out.events) {
    if (ev.type == static_cast<std::uint16_t>(skydiag::EventType::kHangMark)) {
      hasHangEvent = true;
      break;
    }
  }

  // Determine whether this looks like a hang/freeze capture.
  bool hangLike = false;
  bool capSaysHang = false;
  if (out.has_wct) {
    if (auto cap = internal::TryParseWctCaptureDecision(out.wct_json_utf8)) {
      capSaysHang =
        (cap->kind == "hang") ||
        (cap->thresholdSec > 0u && cap->secondsSinceHeartbeat >= static_cast<double>(cap->thresholdSec));
      hangLike = capSaysHang;
    } else {
      hangLike = nameHang;
    }

    if (hasHangEvent) {
      hangLike = true;
    }

    // Best-effort override: some manual hotkey dumps are named "_Hang_" even when the game is fine.
    // If the last heartbeat is very recent, treat it as a snapshot (not hang-like).
    if (!capSaysHang && !hasHangEvent) {
      constexpr double kNotHangHeartbeatAgeSec = 5.0;
      if (out.has_blackbox) {
        if (auto hbAge = internal::InferHeartbeatAgeFromEventsSec(out.events)) {
          if (*hbAge < kNotHangHeartbeatAgeSec) {
            hangLike = false;
          }
        }
      }
    }
  } else {
    hangLike = nameHang || hasHangEvent;
  }

  // Optional: Crash Logger SSE/AE log integration (best-effort)
  {
    // Manual snapshot dumps often include WCT even when the game is fine, so only search Crash Logger logs when the
    // capture looks crash/hang-like.
    const bool shouldSearchCrashLogger = (out.exc_code != 0) || nameCrash || hangLike;
    if (shouldSearchCrashLogger) {
      std::wstring clErr;
      const auto dumpFs = std::filesystem::path(dumpPath);
      std::optional<std::filesystem::path> gameRootDir;
      for (const auto& m : allModules) {
        if (m.path.empty() || m.filename.empty()) {
          continue;
        }
        const std::wstring lower = WideLower(m.filename);
        if (lower == L"skyrimse.exe" || lower == L"skyrimae.exe" || lower == L"skyrimvr.exe" || lower == L"skyrim.exe") {
          gameRootDir = std::filesystem::path(m.path).parent_path();
          break;
        }
      }

      const auto mo2Base = TryInferMo2BaseDirFromModulePaths(modulePaths);
      if (auto logPath = TryFindCrashLoggerLogForDump(dumpFs, mo2Base, mo2Index ? &*mo2Index : nullptr, gameRootDir, &clErr)) {
        out.crash_logger_log_path = logPath->wstring();

        std::wstring readErr;
        auto logUtf8 = ReadWholeFileUtf8(*logPath, &readErr);
        if (logUtf8) {
          if (auto ver = crashlogger_core::ParseCrashLoggerVersionAscii(*logUtf8)) {
            out.crash_logger_version = Utf8ToWide(*ver);
          }

          std::unordered_map<std::wstring, std::wstring> canonicalByLower;
          canonicalByLower.reserve(allModules.size());
          for (const auto& m : allModules) {
            if (!m.filename.empty()) {
              canonicalByLower.emplace(WideLower(m.filename), m.filename);
            }
          }
          out.crash_logger_top_modules = ParseCrashLoggerTopModules(*logUtf8, canonicalByLower);

          if (auto cpp = crashlogger_core::ParseCrashLoggerCppExceptionDetailsAscii(*logUtf8)) {
            if (!cpp->type.empty()) {
              out.crash_logger_cpp_exception_type = Utf8ToWide(cpp->type);
            }
            if (!cpp->info.empty()) {
              out.crash_logger_cpp_exception_info = Utf8ToWide(cpp->info);
            }
            if (!cpp->throw_location.empty()) {
              out.crash_logger_cpp_exception_throw_location = Utf8ToWide(cpp->throw_location);
            }
            if (!cpp->module.empty()) {
              std::wstring mod = Utf8ToWide(cpp->module);
              const auto it = canonicalByLower.find(WideLower(mod));
              out.crash_logger_cpp_exception_module = (it != canonicalByLower.end()) ? it->second : mod;
            }
          }
        }
      }
    }
  }

  // Suspects (prefer callstack/stackwalk; fallback to stack scan)
  {
    const bool shouldAnalyzeStacks = (out.exc_tid != 0) || hangLike;
    if (shouldAnalyzeStacks) {
      std::vector<std::uint32_t> tids;
      if (out.exc_tid != 0) {
        tids.push_back(out.exc_tid);
      } else if (out.has_wct) {
        tids = internal::ExtractWctCandidateThreadIds(out.wct_json_utf8, /*maxN=*/8);
      }
      if (out.has_blackbox) {
        if (auto mainTid = internal::InferMainThreadIdFromEvents(out.events)) {
          tids.push_back(*mainTid);
        }
      }
      if (!tids.empty()) {
        // Dedup
        std::sort(tids.begin(), tids.end());
        tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
        const auto threads = LoadThreads(dumpBase, dumpSize);
        if (!internal::TryComputeStackwalkSuspects(dumpBase, dumpSize, allModules, tids, out.exc_tid, excCtx, threads, opt.language, out)) {
          out.suspects_from_stackwalk = false;
          out.suspects = internal::ComputeStackScanSuspects(dumpBase, dumpSize, allModules, tids, opt.language);
        }
      }
    }
  }

  internal::ComputeCrashBucket(out);
  BuildEvidenceAndSummary(out, opt.language);
  if (err) err->clear();
  return true;
}

}  // namespace skydiag::dump_tool
