#pragma once

#include "AnalyzerInternals.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace skydiag::dump_tool::internal::stackwalk_internal {

struct MinidumpMemoryRange
{
  std::uint64_t start = 0;
  std::uint64_t end = 0;
  const std::uint8_t* bytes = nullptr;  // points into mapped dump file
};

struct MinidumpMemoryView
{
  std::vector<MinidumpMemoryRange> ranges;

  bool Init(void* dumpBase, std::uint64_t dumpSize, const std::vector<minidump::ThreadRecord>* threads);

  bool Read(std::uint64_t addr, void* dst, std::size_t n, std::size_t& outRead) const;
};

struct SymSession
{
  HANDLE process = nullptr;
  bool ok = false;
  std::wstring searchPath;
  std::wstring cachePath;
  bool usedOnlineSymbolSource = false;
  std::unique_lock<std::mutex> dbghelp_lock;

  explicit SymSession(const std::vector<minidump::ModuleInfo>& modules, bool allowOnlineSymbols);
  ~SymSession();
};

std::vector<std::uint64_t> StackWalkAddrsForContext(
  HANDLE process,
  const MinidumpMemoryView& mem,
  const CONTEXT& inCtx,
  std::size_t maxFrames);

}  // namespace skydiag::dump_tool::internal::stackwalk_internal
