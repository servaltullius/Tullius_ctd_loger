#include "AnalyzerInternals.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace skydiag::dump_tool::internal::stackwalk {
namespace {

using skydiag::dump_tool::minidump::FindModuleIndexForAddress;
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

}  // namespace

std::vector<std::wstring> FormatCallstackForDisplay(
  HANDLE process,
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint64_t>& pcs,
  std::size_t maxFrames,
  std::uint32_t* outTotalFrames,
  std::uint32_t* outSymbolizedFrames,
  std::uint32_t* outSourceLineFrames)
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

  std::size_t firstNonSystem = pcs.size();
  for (std::size_t i = 0; i < pcs.size(); i++) {
    auto mi = FindModuleIndexForAddress(modules, pcs[i]);
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
  const std::size_t end = std::min<std::size_t>(pcs.size(), start + maxFrames);
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

