#include "SkyrimDiag/ResourceLog.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <RE/Skyrim.h>
#include <RE/L/LooseFileStream.h>
#include <REL/Relocation.h>

namespace skydiag::plugin {
namespace {

using ErrorCode = RE::BSResource::ErrorCode;
using DoOpen_t = ErrorCode(RE::BSResource::LooseFileStream*);
DoOpen_t* g_origLooseFileDoOpen = nullptr;

std::size_t Append(char* dst, std::size_t cap, std::size_t pos, const char* s) noexcept
{
  if (!dst || cap == 0) {
    return 0;
  }
  if (!s) {
    if (pos < cap) {
      dst[pos] = '\0';
    }
    return pos;
  }
  while (*s && (pos + 1) < cap) {
    dst[pos++] = *s++;
  }
  dst[pos] = '\0';
  return pos;
}

bool IsSlash(char c) noexcept
{
  return c == '\\' || c == '/';
}

constexpr char ToLowerAscii(char c) noexcept
{
  if (c >= 'A' && c <= 'Z') {
    return static_cast<char>(c + ('a' - 'A'));
  }
  return c;
}

bool EndsWithCaseInsensitiveAscii(const char* s, const char* suffix) noexcept
{
  if (!s || !suffix) {
    return false;
  }
  std::size_t sLen = 0;
  while (s[sLen] != '\0') {
    ++sLen;
  }
  std::size_t sufLen = 0;
  while (suffix[sufLen] != '\0') {
    ++sufLen;
  }
  if (sufLen == 0 || sufLen > sLen) {
    return false;
  }
  const std::size_t start = sLen - sufLen;
  for (std::size_t i = 0; i < sufLen; ++i) {
    if (ToLowerAscii(s[start + i]) != ToLowerAscii(suffix[i])) {
      return false;
    }
  }
  return true;
}

bool IsInterestingResourceName(const char* fileName) noexcept
{
  return EndsWithCaseInsensitiveAscii(fileName, ".nif") ||
         EndsWithCaseInsensitiveAscii(fileName, ".hkx") ||
         EndsWithCaseInsensitiveAscii(fileName, ".tri");
}

std::size_t AppendPathPart(char* dst, std::size_t cap, std::size_t pos, const char* part) noexcept
{
  if (!dst || cap == 0 || !part || part[0] == '\0') {
    return pos;
  }

  if (pos > 0) {
    const char last = dst[pos - 1];
    const char first = part[0];
    if (!IsSlash(last) && !IsSlash(first) && (pos + 1) < cap) {
      dst[pos++] = '\\';
      dst[pos] = '\0';
    }
    while (IsSlash(dst[pos - 1]) && IsSlash(part[0])) {
      part++;
    }
  }

  return Append(dst, cap, pos, part);
}

// The stream's prefix, directory and file name. CommonLibSSE-NG lays out
// LooseFileStream for AE when built for SE and AE (its Stream base is 0x18
// bytes on AE, 0x10 on SE), so self->fileName on 1.5.97 reads the wrong
// field: v0.2.62 and v0.2.63 crashed there on the first loose file the game
// opened. Locate the names for the running game instead.
const RE::BSResource::LooseFileStreamBase& NamesOf(const RE::BSResource::LooseFileStream& stream) noexcept
{
  constexpr std::size_t kNamesOffsetAE = 0x18;  // after Stream on 1.6.x / 1.7.x
  constexpr std::size_t kNamesOffsetSE = 0x10;  // after Stream on 1.5.97
  const std::size_t offset = REL::Module::IsAE() ? kNamesOffsetAE : kNamesOffsetSE;
  return *reinterpret_cast<const RE::BSResource::LooseFileStreamBase*>(
    reinterpret_cast<const std::byte*>(&stream) + offset);
}

ErrorCode LooseFileDoOpen_Hook(RE::BSResource::LooseFileStream* self)
{
  if (!g_origLooseFileDoOpen) {
    return ErrorCode::kInvalidParam;
  }

  const auto rc = g_origLooseFileDoOpen(self);
  if (rc != ErrorCode::kNone || !self) {
    return rc;
  }

  const auto& names = NamesOf(*self);
  const char* fileName = names.fileName.c_str();
  if (!IsInterestingResourceName(fileName)) {
    return rc;
  }

  char buf[512]{};
  std::size_t n = 0;
  n = AppendPathPart(buf, sizeof(buf), n, names.prefix.c_str());
  n = AppendPathPart(buf, sizeof(buf), n, names.dirName.c_str());
  n = AppendPathPart(buf, sizeof(buf), n, fileName);

  if (n > 0 && buf[0] != '\0') {
    NoteResourceOpen(std::string_view(buf, n));
  }

  return rc;
}

}  // namespace

bool InstallResourceHooks() noexcept
{
  // Best-effort: hook loose file opens and record interesting resource paths.
  // This is intentionally lightweight and filtered (nif/hkx/tri only by default).
  static bool installed = false;
  if (installed) {
    return true;
  }

  try {
    REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSResource____LooseFileStream[0] };
    g_origLooseFileDoOpen = reinterpret_cast<DoOpen_t*>(vtbl.write_vfunc(0x01, LooseFileDoOpen_Hook));
    installed = (g_origLooseFileDoOpen != nullptr);
  } catch (...) {
    // Hook installation is optional. Keep this noexcept boundary reliable when
    // relocation lookup or trampoline patching rejects the current runtime.
    g_origLooseFileDoOpen = nullptr;
    installed = false;
  }
  return installed;
}

}  // namespace skydiag::plugin
