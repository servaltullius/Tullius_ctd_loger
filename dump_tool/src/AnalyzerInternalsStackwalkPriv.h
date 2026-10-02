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

// Read-only image-layout views of local module files. Used only for memory the
// dump does not contain (typically unwind data of system DLLs), and only when
// the local file has the dump module's TimeDateStamp and SizeOfImage.
class LocalImageMemory
{
public:
  explicit LocalImageMemory(const std::vector<minidump::ModuleInfo>& modules);
  ~LocalImageMemory();
  LocalImageMemory(const LocalImageMemory&) = delete;
  LocalImageMemory& operator=(const LocalImageMemory&) = delete;

  bool Read(std::uint64_t addr, void* dst, std::size_t n, std::size_t& outRead) const;

  // x64 unwind entry (.pdata) covering addr, or nullptr when the module has no
  // verified local image or the address is in a leaf function. The pointer
  // stays valid for the lifetime of this object.
  const RUNTIME_FUNCTION* FindFunctionEntry(std::uint64_t addr) const;

private:
  struct Image
  {
    const minidump::ModuleInfo* module = nullptr;
    bool attempted = false;
    HMODULE handle = nullptr;
    const std::uint8_t* view = nullptr;
  };
  const Image* Resolve(std::uint64_t addr) const;

  mutable std::vector<Image> images_;
};

struct MinidumpMemoryView
{
  std::vector<MinidumpMemoryRange> ranges;
  const LocalImageMemory* image_fallback = nullptr;

  bool Init(void* dumpBase, std::uint64_t dumpSize, const std::vector<minidump::ThreadRecord>* threads);

  bool Read(std::uint64_t addr, void* dst, std::size_t n, std::size_t& outRead) const;
};

struct SymSession
{
  HANDLE process = nullptr;
  HMODULE ownedMsdiaModule = nullptr;
  bool ok = false;
  std::wstring searchPath;
  std::wstring cachePath;
  std::wstring dbghelpPath;
  std::wstring dbghelpVersion;
  std::wstring msdiaPath;
  bool msdiaAvailable = false;
  bool symbolCacheReady = false;
  bool runtimeDegraded = false;
  bool usedOnlineSymbolSource = false;
  std::vector<std::wstring> runtimeDiagnostics;
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
