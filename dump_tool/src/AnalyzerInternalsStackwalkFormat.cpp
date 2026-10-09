#include "AnalyzerInternals.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace skydiag::dump_tool::internal::stackwalk {
namespace {

using skydiag::dump_tool::minidump::FindModuleIndexForAddress;
using skydiag::dump_tool::minidump::IsSkseModule;
using skydiag::dump_tool::minidump::ModuleInfo;

std::wstring FormatModulePlusOffset(const std::vector<ModuleInfo>& modules, std::uint64_t addr)
{
  if (auto idx = FindModuleIndexForAddress(modules, addr)) {
    const auto& m = modules[*idx];
    const std::uint64_t off = addr - m.base;
    wchar_t buf[1024]{};
    swprintf_s(buf, L"%s+0x%llx", m.filename.c_str(), static_cast<unsigned long long>(off));
    return buf;
  }
  wchar_t buf[64]{};
  swprintf_s(buf, L"0x%llx", static_cast<unsigned long long>(addr));
  return buf;
}

std::wstring FormatSymbolizedFrame(
  HANDLE process,
  const std::vector<ModuleInfo>& modules,
  std::uint64_t addr,
  bool* outHasSymbol,
  bool* outHasSourceLine)
{
  if (outHasSymbol) {
    *outHasSymbol = false;
  }
  if (outHasSourceLine) {
    *outHasSourceLine = false;
  }

  const std::wstring fallback = FormatModulePlusOffset(modules, addr);
  if (!process || addr == 0) {
    return fallback;
  }

  alignas(SYMBOL_INFOW) unsigned char symBuf[sizeof(SYMBOL_INFOW) + (MAX_SYM_NAME * sizeof(wchar_t))]{};
  auto* sym = reinterpret_cast<PSYMBOL_INFOW>(symBuf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
  sym->MaxNameLen = MAX_SYM_NAME;

  DWORD64 displacement = 0;
  if (!SymFromAddrW(process, static_cast<DWORD64>(addr), &displacement, sym) || sym->NameLen == 0) {
    return fallback;
  }
  // Without a PDB, DbgHelp names an address after the nearest exported
  // function below it, however far away. A mod DLL usually exports only
  // SKSEPlugin_Load and friends, so most of its code would be shown as
  // SKSEPlugin_Load+0x7bd57. Keep an export name only when the address is
  // close enough to plausibly be inside that function.
  constexpr DWORD64 kMaxExportSymbolDisplacement = 0x1000;
  IMAGEHLP_MODULEW64 moduleInfo{};
  moduleInfo.SizeOfStruct = sizeof(moduleInfo);
  if (displacement > kMaxExportSymbolDisplacement &&
      SymGetModuleInfoW64(process, static_cast<DWORD64>(addr), &moduleInfo) &&
      moduleInfo.SymType == SymExport) {
    return fallback;
  }
  if (outHasSymbol) {
    *outHasSymbol = true;
  }

  std::wstring frame;
  if (auto idx = FindModuleIndexForAddress(modules, addr)) {
    frame = modules[*idx].filename;
    frame += L"!";
  }
  frame.append(sym->Name, sym->NameLen);

  wchar_t offBuf[64]{};
  swprintf_s(offBuf, L"+0x%llx", static_cast<unsigned long long>(displacement));
  frame += offBuf;

  IMAGEHLP_LINEW64 line{};
  line.SizeOfStruct = sizeof(line);
  DWORD lineDisp = 0;
  if (SymGetLineFromAddrW64(process, static_cast<DWORD64>(addr), &lineDisp, &line) && line.FileName && line.LineNumber > 0) {
    if (outHasSourceLine) {
      *outHasSourceLine = true;
    }
    std::filesystem::path src(line.FileName);
    frame += L" [";
    frame += src.filename().wstring();
    frame += L":";
    frame += std::to_wstring(line.LineNumber);
    frame += L"]";
  }

  return frame;
}

std::pair<std::size_t, std::size_t> SelectCallstackFrameRange(
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  std::size_t maxFrames)
{
  std::size_t firstNonSystem = pcs.size();
  for (std::size_t i = 0; i < pcs.size(); i++) {
    const auto mi = FindModuleIndexForAddress(modules, pcs[i]);
    if (!mi) {
      continue;
    }
    const auto& m = modules[*mi];
    if (!m.is_systemish && !m.is_game_exe) {
      firstNonSystem = i;
      break;
    }
  }

  const std::size_t start = (firstNonSystem != pcs.size() && firstNonSystem > 2) ? (firstNonSystem - 2) : 0;
  return { start, std::min<std::size_t>(pcs.size(), start + maxFrames) };
}

}  // namespace

std::vector<CrashBucketFrame> BuildCanonicalCallstackFrames(
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  std::size_t maxFrames)
{
  std::vector<CrashBucketFrame> out;
  if (pcs.empty() || maxFrames == 0) {
    return out;
  }

  const auto [start, end] = SelectCallstackFrameRange(modules, pcs, maxFrames);
  out.reserve(end - start);
  for (std::size_t i = start; i < end; i++) {
    const auto moduleIndex = FindModuleIndexForAddress(modules, pcs[i]);
    if (!moduleIndex) {
      continue;
    }
    const auto& module = modules[*moduleIndex];
    out.push_back({ module.filename, pcs[i] - module.base });
  }
  return out;
}

std::vector<ModalStackFrame> BuildModalProbeFrames(
  HANDLE process,
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  std::size_t maxFrames)
{
  std::vector<ModalStackFrame> out;
  const std::size_t n = std::min(pcs.size(), maxFrames);
  out.reserve(n);
  for (std::size_t i = 0; i < n; i++) {
    ModalStackFrame frame{};
    if (const auto idx = FindModuleIndexForAddress(modules, pcs[i])) {
      const auto& m = modules[*idx];
      frame.has_module = true;
      frame.module_filename = m.filename;
      frame.module_path = m.path;
      frame.inferred_mod_name = m.inferred_mod_name;
      frame.is_system = m.is_systemish;
      frame.is_game_exe = m.is_game_exe;
      frame.is_skse_runtime = IsSkseModule(m.filename);
      frame.is_hook_framework = m.is_known_hook_framework;
      // Modal entry points live in system DLLs; only those need names.
      if (m.is_systemish && process) {
        alignas(SYMBOL_INFOW) unsigned char symBuf[sizeof(SYMBOL_INFOW) + (MAX_SYM_NAME * sizeof(wchar_t))]{};
        auto* sym = reinterpret_cast<PSYMBOL_INFOW>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
        sym->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (SymFromAddrW(process, static_cast<DWORD64>(pcs[i]), &displacement, sym) && sym->NameLen > 0) {
          frame.symbol.assign(sym->Name, sym->NameLen);
          frame.displacement = static_cast<std::uint64_t>(displacement);
        }
      }
    }
    out.push_back(std::move(frame));
  }
  return out;
}

std::vector<std::wstring> FormatCallstackForDisplay(
  HANDLE process,
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  std::size_t maxFrames,
  std::uint32_t* outTotalFrames,
  std::uint32_t* outSymbolizedFrames,
  std::uint32_t* outSourceLineFrames,
  bool fromTop)
{
  if (outTotalFrames) {
    *outTotalFrames = 0;
  }
  if (outSymbolizedFrames) {
    *outSymbolizedFrames = 0;
  }
  if (outSourceLineFrames) {
    *outSourceLineFrames = 0;
  }

  std::vector<std::wstring> out;
  if (pcs.empty() || maxFrames == 0) {
    return out;
  }

  // A frozen main thread's top frames show what it waits in (ADR-0009), so
  // freeze stacks start at frame 0; crash stacks skip to the first plugin.
  const auto [start, end] = fromTop
    ? std::pair<std::size_t, std::size_t>{ 0u, std::min<std::size_t>(pcs.size(), maxFrames) }
    : SelectCallstackFrameRange(modules, pcs, maxFrames);
  out.reserve(end - start);
  for (std::size_t i = start; i < end; i++) {
    bool hasSymbol = false;
    bool hasSourceLine = false;
    out.push_back(FormatSymbolizedFrame(process, modules, pcs[i], &hasSymbol, &hasSourceLine));
    if (outTotalFrames) {
      *outTotalFrames += 1;
    }
    if (hasSymbol && outSymbolizedFrames) {
      *outSymbolizedFrames += 1;
    }
    if (hasSourceLine && outSourceLineFrames) {
      *outSourceLineFrames += 1;
    }
  }
  return out;
}

}  // namespace skydiag::dump_tool::internal::stackwalk
