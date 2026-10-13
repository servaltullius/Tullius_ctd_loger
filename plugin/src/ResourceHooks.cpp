#include "SkyrimDiag/ResourceLog.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

// CommonLibSSE-NG must come before any Windows API header (REX/W32/BASE.h).
#include <RE/Skyrim.h>
#include <RE/L/LooseFileStream.h>
#include <REL/Relocation.h>

#include <Windows.h>
#include <spdlog/spdlog.h>

namespace skydiag::plugin {
namespace {

using ErrorCode = RE::BSResource::ErrorCode;
using DoOpen_t = ErrorCode(void*);  // this = the Stream subobject
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

// Where the game keeps a loose-file stream's names, read from the game itself
// (RTTI and LooseFileStream::DoOpen) rather than from CommonLibSSE-NG, whose
// SE+AE build lays the class out one way for every runtime:
// - 1.5.97: Stream is the primary base at offset 0 and the names follow it;
//   DoOpen is slot 1 of the class's vtable (VTABLE[0]).
// - 1.6.x / 1.7.x: the object starts with a one-slot vtable and the names,
//   and Stream is a secondary base at 0x30 with its own vtable (VTABLE[1]).
//   VTABLE[0] slot 1 there is the next vtable's RTTI pointer: hooking it, as
//   every release up to v0.2.64 did, overwrote that pointer and never saw a
//   single DoOpen (no resource was ever recorded on AE).
// v0.2.62 and v0.2.63 crashed on 1.5.97 reading the names at the AE offset.
struct LooseFileStreamLayout
{
  std::size_t vtableIndex = 0;      // into VTABLE_BSResource____LooseFileStream
  std::uint32_t streamOffset = 0;   // Stream subobject in the object; DoOpen's this
  std::size_t namesOffset = 0;      // prefix, dirName, fileName (BSFixedString each)
};

constexpr LooseFileStreamLayout kLayoutSE{ 0, 0x00, 0x10 };
constexpr LooseFileStreamLayout kLayoutAE{ 1, 0x30, 0x08 };
LooseFileStreamLayout g_layout{};

struct StreamNames
{
  const char* prefix = "";
  const char* dirName = "";
  const char* fileName = "";
};

StreamNames NamesOf(const void* stream) noexcept
{
  const auto* object = static_cast<const std::byte*>(stream) - g_layout.streamOffset;
  const auto* names = reinterpret_cast<const RE::BSFixedString*>(object + g_layout.namesOffset);
  return StreamNames{ names[0].c_str(), names[1].c_str(), names[2].c_str() };
}

// MSVC x64 RTTI: the vtable is preceded by its CompleteObjectLocator, which
// gives the subobject offset and the class's TypeDescriptor (name at +0x10).
struct CompleteObjectLocator
{
  std::uint32_t signature;  // 1 on x64 (image-relative RVAs)
  std::uint32_t offset;
  std::uint32_t cdOffset;
  std::uint32_t typeDescriptorRva;
  std::uint32_t classDescriptorRva;
  std::uint32_t selfRva;
};

// The vtable belongs to LooseFileStream's Stream subobject at the expected
// offset; anything else (another runtime, another layout) is left unhooked.
bool IsLooseFileStreamVtable(std::uintptr_t vtable, std::uint32_t streamOffset) noexcept
{
  const auto base = REL::Module::get().base();
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const std::uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
  const auto inImage = [&](std::uintptr_t p, std::size_t n) { return p >= base && p + n <= end; };

  if (!inImage(vtable - sizeof(void*), 2 * sizeof(void*))) {
    return false;
  }
  const auto colAddr = *reinterpret_cast<const std::uintptr_t*>(vtable - sizeof(void*));
  if (!inImage(colAddr, sizeof(CompleteObjectLocator))) {
    return false;
  }
  const auto& col = *reinterpret_cast<const CompleteObjectLocator*>(colAddr);
  if (col.signature != 1 || col.offset != streamOffset || base + col.selfRva != colAddr) {
    return false;
  }
  const std::uintptr_t nameAddr = base + col.typeDescriptorRva + 0x10;
  constexpr std::string_view kClass = ".?AVLooseFileStream@";
  if (!inImage(nameAddr, kClass.size()) ||
      std::string_view(reinterpret_cast<const char*>(nameAddr), kClass.size()) != kClass) {
    return false;
  }
  // DoOpen, the slot we replace, must be code in the game image.
  const auto doOpen = *reinterpret_cast<const std::uintptr_t*>(vtable + sizeof(void*));
  return inImage(doOpen, 1);
}

ErrorCode LooseFileDoOpen_Hook(void* self)
{
  if (!g_origLooseFileDoOpen) {
    return ErrorCode::kInvalidParam;
  }

  const auto rc = g_origLooseFileDoOpen(self);
  if (rc != ErrorCode::kNone || !self) {
    return rc;
  }

  const auto names = NamesOf(self);
  const char* fileName = names.fileName;
  if (!IsInterestingResourceName(fileName)) {
    return rc;
  }

  char buf[512]{};
  std::size_t n = 0;
  n = AppendPathPart(buf, sizeof(buf), n, names.prefix);
  n = AppendPathPart(buf, sizeof(buf), n, names.dirName);
  n = AppendPathPart(buf, sizeof(buf), n, fileName);

  if (n > 0 && buf[0] != '\0') {
    NoteResourceOpen(std::string_view(buf, n));
    // One line in SkyrimDiag.log shows the hook works on this runtime (no
    // resource had ever been recorded on AE before v0.2.65).
    static std::atomic_bool logged{ false };
    if (!logged.exchange(true, std::memory_order_relaxed)) {
      spdlog::info("SkyrimDiag: resource hook saw its first loose file: {}", std::string_view(buf, n));
    }
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
    g_layout = REL::Module::IsAE() ? kLayoutAE : kLayoutSE;
    REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSResource____LooseFileStream[g_layout.vtableIndex] };
    if (!IsLooseFileStreamVtable(vtbl.address(), g_layout.streamOffset)) {
      return false;  // an unknown layout: patch nothing
    }
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
