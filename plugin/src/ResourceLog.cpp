#include "SkyrimDiag/ResourceLog.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <string_view>

#include "SkyrimDiag/Hash.h"
#include "SkyrimDiag/SharedMemory.h"
#include "SkyrimDiagShared.h"

namespace skydiag::plugin {
namespace {

std::atomic_bool g_adaptiveThrottleEnabled{ true };
std::atomic_uint32_t g_throttleHighWatermarkPerSec{ 1500 };
std::atomic_uint32_t g_throttleMaxSampleDivisor{ 8 };
std::atomic_uint64_t g_windowStartQpc{ 0 };
std::atomic_uint32_t g_windowSeen{ 0 };
std::atomic_uint32_t g_sampleCounter{ 0 };

inline std::uint64_t QpcNow() noexcept
{
  LARGE_INTEGER li{};
  QueryPerformanceCounter(&li);
  return static_cast<std::uint64_t>(li.QuadPart);
}

constexpr bool IsAsciiAlpha(char c) noexcept
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

constexpr char ToLowerAscii(char c) noexcept
{
  if (c >= 'A' && c <= 'Z') {
    return static_cast<char>(c + ('a' - 'A'));
  }
  return c;
}

bool EndsWithCaseInsensitiveAscii(std::string_view s, std::string_view suffix) noexcept
{
  if (suffix.size() > s.size()) {
    return false;
  }
  const auto tail = s.substr(s.size() - suffix.size());
  for (std::size_t i = 0; i < suffix.size(); i++) {
    if (ToLowerAscii(tail[i]) != ToLowerAscii(suffix[i])) {
      return false;
    }
  }
  return true;
}

bool IsInterestingResourcePath(std::string_view path) noexcept
{
  // Keep overhead very low: only track the most CTD-relevant asset types by default.
  return EndsWithCaseInsensitiveAscii(path, ".nif") ||
         EndsWithCaseInsensitiveAscii(path, ".hkx") ||
         EndsWithCaseInsensitiveAscii(path, ".tri");
}

std::uint64_t NormalizePathUtf8(std::string_view in, char* out, std::size_t outCap, std::size_t& outLen) noexcept
{
  outLen = 0;
  if (!out || outCap == 0) {
    return 0;
  }

  std::size_t i = 0;

  // Trim leading whitespace (rare, but safe).
  while (i < in.size() && static_cast<unsigned char>(in[i]) <= 0x20) {
    i++;
  }

  // If an absolute path is observed (e.g. "...\\Skyrim Special Edition\\Data\\meshes\\..."), strip everything up to
  // the last "\data\" component. This keeps paths relative for MO2 provider lookup.
  const auto is_sep = [](char c) noexcept { return c == '\\' || c == '/'; };
  for (std::size_t pos = in.size(); pos-- > i;) {
    if (!is_sep(in[pos])) {
      continue;
    }
    if (pos + 5 >= in.size()) {  // need: sep + "data" + sep
      continue;
    }

    const char d0 = ToLowerAscii(in[pos + 1]);
    const char d1 = ToLowerAscii(in[pos + 2]);
    const char d2 = ToLowerAscii(in[pos + 3]);
    const char d3 = ToLowerAscii(in[pos + 4]);
    const char d4 = in[pos + 5];
    if (d0 == 'd' && d1 == 'a' && d2 == 't' && d3 == 'a' && is_sep(d4)) {
      i = pos + 6;  // after "\data\"
      break;
    }
  }

  // Strip "data\\" prefix if present (case-insensitive).
  if (i + 5 <= in.size()) {
    const char d0 = ToLowerAscii(in[i + 0]);
    const char d1 = ToLowerAscii(in[i + 1]);
    const char d2 = ToLowerAscii(in[i + 2]);
    const char d3 = ToLowerAscii(in[i + 3]);
    const char d4 = in[i + 4];
    if (d0 == 'd' && d1 == 'a' && d2 == 't' && d3 == 'a' && (d4 == '\\' || d4 == '/')) {
      i += 5;
    }
  }

  // Strip leading slashes.
  while (i < in.size() && (in[i] == '\\' || in[i] == '/')) {
    i++;
  }

  // Normalize into out buffer: lower-case ASCII + backslashes.
  std::uint64_t hash = 14695981039346656037ull;  // FNV-1a 64 offset basis
  for (; i < in.size() && (outLen + 1) < outCap; i++) {
    char c = in[i];
    if (c == '/') {
      c = '\\';
    }
    c = ToLowerAscii(c);
    out[outLen++] = c;

    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }

  out[outLen] = '\0';
  return hash;
}

std::uint64_t QpcFreqNow() noexcept
{
  LARGE_INTEGER li{};
  QueryPerformanceFrequency(&li);
  return static_cast<std::uint64_t>(li.QuadPart);
}

bool ShouldRecordResourceEvent(const skydiag::SharedLayout* shm, std::uint64_t nowQpc) noexcept
{
  if (!g_adaptiveThrottleEnabled.load()) {
    return true;
  }

  std::uint64_t qpcFreq = QpcFreqNow();
  if (shm && shm->header.qpc_freq != 0) {
    qpcFreq = shm->header.qpc_freq;
  }
  if (qpcFreq == 0) {
    return true;
  }

  std::uint64_t start = g_windowStartQpc.load();
  if (start == 0 || nowQpc < start || (nowQpc - start) >= qpcFreq) {
    if (g_windowStartQpc.compare_exchange_strong(start, nowQpc)) {
      g_windowSeen.store(0);
      g_sampleCounter.store(0);
    }
  }

  const std::uint32_t seen = g_windowSeen.fetch_add(1) + 1;
  const std::uint32_t highWatermark = g_throttleHighWatermarkPerSec.load();
  if (highWatermark == 0 || seen <= highWatermark) {
    return true;
  }

  std::uint32_t maxDivisor = g_throttleMaxSampleDivisor.load();
  if (maxDivisor < 2) {
    maxDivisor = 2;
  }
  const std::uint32_t overflow = seen - highWatermark;
  std::uint32_t divisor = 2 + (overflow / highWatermark);
  if (divisor > maxDivisor) {
    divisor = maxDivisor;
  }

  const std::uint32_t token = g_sampleCounter.fetch_add(1) + 1;
  return (token % divisor) == 0;
}

}  // namespace

void ConfigureResourceLogThrottle(
  bool enableAdaptive,
  std::uint32_t highWatermarkPerSec,
  std::uint32_t maxSampleDivisor) noexcept
{
  g_adaptiveThrottleEnabled.store(enableAdaptive);
  g_throttleHighWatermarkPerSec.store(highWatermarkPerSec);
  g_throttleMaxSampleDivisor.store(std::max<std::uint32_t>(2u, maxSampleDivisor));
}

void NoteResourceOpen(std::string_view pathUtf8) noexcept
{
  if (pathUtf8.empty()) {
    return;
  }
  if (!IsInterestingResourcePath(pathUtf8)) {
    return;
  }

  auto* shm = GetShared();
  if (!shm) {
    return;
  }
  if ((shm->header.state_flags & skydiag::kState_Frozen) != 0u) {
    return;
  }
  const std::uint64_t nowQpc = QpcNow();
  if (!ShouldRecordResourceEvent(shm, nowQpc)) {
    return;
  }

  char norm[skydiag::kResourcePathMaxBytes]{};
  std::size_t normLen = 0;
  const std::uint64_t h = NormalizePathUtf8(pathUtf8, norm, sizeof(norm), normLen);
  if (normLen == 0 || norm[0] == '\0') {
    return;
  }

  // Dedup adjacent duplicates (best-effort).
  const std::uint32_t prevIdx = shm->resources.write_index;
  if (prevIdx != 0) {
    const auto& prev = shm->resources.entries[(prevIdx - 1) % skydiag::kResourceCapacity];
    if ((prev.seq & 1u) == 0u && prev.path_hash == h) {
      return;
    }
  }

  const auto idx = static_cast<std::uint32_t>(
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shm->resources.write_index)) - 1);

  auto& e = shm->resources.entries[idx % skydiag::kResourceCapacity];
  const std::uint32_t seq = idx * 2u;

  e.seq = seq | 1u;
  e.tid = GetCurrentThreadId();
  e.qpc = nowQpc;
  e.path_hash = h;
  std::memset(e.path_utf8, 0, sizeof(e.path_utf8));
  std::memcpy(e.path_utf8, norm, std::min<std::size_t>(sizeof(e.path_utf8) - 1, normLen));
  e.seq = seq;
}

}  // namespace skydiag::plugin
