#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool {

struct CrashBucketFrame
{
  std::wstring module_filename;
  std::uint64_t module_offset = 0;
};

namespace bucket {

inline std::wstring LowerTrimmed(std::wstring_view s)
{
  std::size_t begin = 0;
  while (begin < s.size()) {
    const wchar_t c = s[begin];
    if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n') {
      break;
    }
    begin++;
  }

  std::size_t end = s.size();
  while (end > begin) {
    const wchar_t c = s[end - 1];
    if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n') {
      break;
    }
    end--;
  }

  std::wstring out;
  out.reserve(end - begin);
  for (std::size_t i = begin; i < end; i++) {
    out.push_back(static_cast<wchar_t>(towlower(s[i])));
  }
  return out;
}

inline std::string NarrowAsciiFallback(std::wstring_view s)
{
  std::string out;
  out.reserve(s.size());
  for (const wchar_t wc : s) {
    if (wc >= 0 && wc <= 0x7F) {
      out.push_back(static_cast<char>(wc));
    } else {
      out.push_back('?');
    }
  }
  return out;
}

inline std::uint64_t Fnv1a64(std::string_view s)
{
  std::uint64_t h = 14695981039346656037ull;
  for (const unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

}  // namespace bucket

inline std::wstring ComputeCrashBucketKey(
  std::uint32_t exceptionCode,
  std::wstring_view faultModule,
  std::uint64_t faultModuleOffset,
  const std::vector<CrashBucketFrame>& frames,
  std::size_t maxFrames = 6)
{
  std::ostringstream canonical;
  canonical << "v=3|exc=0x" << std::hex << std::nouppercase << exceptionCode << "|mod=";
  canonical << bucket::NarrowAsciiFallback(bucket::LowerTrimmed(faultModule));
  canonical << "|off=0x" << std::hex << std::nouppercase << faultModuleOffset;

  const std::size_t n = std::min<std::size_t>(frames.size(), maxFrames);
  for (std::size_t i = 0; i < n; i++) {
    canonical << "|f" << i << "=";
    canonical << bucket::NarrowAsciiFallback(bucket::LowerTrimmed(frames[i].module_filename));
    canonical << "+0x" << std::hex << std::nouppercase << frames[i].module_offset;
  }

  const std::uint64_t hash = bucket::Fnv1a64(canonical.str());
  std::wstringstream wss;
  wss << L"CTD3-" << std::hex << std::nouppercase << std::setw(16) << std::setfill(L'0') << hash;
  return wss.str();
}

// Freezes grouped by the main thread's wait (ADR-0009): "HANG1-<hash>". The
// prefix keeps them apart from stack-keyed history written before.
inline std::wstring ComputeHangBucketKey(std::string_view waitCanonical)
{
  const std::uint64_t hash = bucket::Fnv1a64(waitCanonical);
  std::wstringstream wss;
  wss << L"HANG1-" << std::hex << std::nouppercase << std::setw(16) << std::setfill(L'0') << hash;
  return wss.str();
}

// A thread name without its numbering ("Worker 12" -> "worker #"), so the
// same kind of thread names the same bucket in every run.
inline std::string ThreadNameClass(std::wstring_view threadName)
{
  const std::string lower = bucket::NarrowAsciiFallback(bucket::LowerTrimmed(threadName));
  std::string out;
  out.reserve(lower.size());
  for (const char c : lower) {
    if (c >= '0' && c <= '9') {
      if (out.empty() || out.back() != '#') {
        out.push_back('#');
      }
      continue;
    }
    out.push_back(c);
  }
  return out;
}

// lock_owner_wait: the main thread's own wait is the same for every such
// freeze, so they are grouped by the thread holding the lock instead, its top
// non-Windows module and its kind of thread. Tids and addresses are left out;
// they change with every run.
inline std::wstring ComputeLockOwnerBucketKey(
  std::string_view freezeState,
  std::wstring_view ownerTopModule,
  std::wstring_view ownerThreadName)
{
  std::ostringstream canonical;
  canonical << "v=1|kind=" << freezeState
            << "|owner_module=" << bucket::NarrowAsciiFallback(bucket::LowerTrimmed(ownerTopModule))
            << "|owner_thread=" << ThreadNameClass(ownerThreadName);
  return ComputeHangBucketKey(canonical.str());
}

// A freeze whose state alone is the cause (crash_logger_thread_dump_deadlock:
// whichever thread the dump happened to suspend is incidental).
inline std::wstring ComputeFreezeStateBucketKey(std::string_view freezeState)
{
  return ComputeHangBucketKey("v=1|kind=" + std::string(freezeState));
}

// engine_spin_nan: the engine loop itself (module and offset of the spinning
// instruction), whichever thread ran it and whichever plugins were below it.
inline std::wstring ComputeEngineSpinBucketKey(
  std::string_view freezeState,
  std::wstring_view ripModule,
  std::uint64_t ripOffset)
{
  std::ostringstream canonical;
  canonical << "v=1|kind=" << freezeState
            << "|rip=" << bucket::NarrowAsciiFallback(bucket::LowerTrimmed(ripModule)) << "+0x" << std::hex
            << std::nouppercase << ripOffset;
  return ComputeHangBucketKey(canonical.str());
}

}  // namespace skydiag::dump_tool
