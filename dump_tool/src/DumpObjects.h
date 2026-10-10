#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "Analyzer.h"
#include "MinidumpUtil.h"

namespace skydiag::dump_tool {

// ADR-0010: the objects that the crash registers and the top of the crash
// stack point at, read back from the memory the helper added to the dump
// (shared/SkyrimDiagRtti.h). Dumps written before that, and memory the helper
// could not read, give fewer or no objects. Sorted by relevance, at most
// kMaxDumpObjects.
std::vector<DumpObject> ReadDumpObjects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ThreadRecord>& threads,
  const std::vector<minidump::ModuleInfo>& modules,
  const CONTEXT& context);

inline constexpr std::size_t kMaxDumpObjects = 24;

// "RBX: Character [0xFEAD081B] Skyrim.esm -> MyMod.esp", or for other objects
// "RCX: BSFadeNode (SkyrimSE.exe)". Language-neutral.
std::wstring DescribeDumpObject(const DumpObject& object);

}  // namespace skydiag::dump_tool
