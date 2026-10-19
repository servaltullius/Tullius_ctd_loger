#pragma once

#include <Windows.h>

#include <DbgHelp.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "SkyrimDiagHandle.h"
#include "SkyrimDiagStringUtil.h"

namespace skydiag::dump_tool::minidump {

struct MappedFile
{
  skydiag::UniqueHandle file;
  skydiag::UniqueHandle mapping;
  void* view = nullptr;
  std::uint64_t size = 0;

  bool Open(const std::wstring& path, std::wstring* err);
  void Close() noexcept;

  ~MappedFile();
};

bool ReadStreamSized(void* dumpBase, std::uint64_t dumpSize, std::uint32_t streamType, void** outPtr, ULONG* outSize);
bool ReadMinidumpStringUtf8(void* dumpBase, std::uint64_t dumpSize, RVA rva, std::string& out);

struct ModuleHit
{
  std::uint64_t base = 0;
  std::wstring path;
  std::wstring filename;
  std::wstring plusOffset;
};

std::optional<ModuleHit> ModuleForAddress(void* dumpBase, std::uint64_t dumpSize, std::uint64_t addr);

inline std::wstring WideLower(std::wstring_view s) { return skydiag::WideLower(s); }

// By file name only; use IsSystemishModuleAt when the module's path is known.
bool IsSystemishModule(std::wstring_view filename);
// DirectX/OpenGL runtime names that graphics injectors also use for their proxies.
bool IsGraphicsRuntimeName(std::wstring_view filename);
bool IsLikelyWindowsSystemModulePath(std::wstring_view modulePath);
// The rule LoadAllModules uses for is_systemish (minus external software): a
// module under a Windows folder is a system module; a DirectX/OpenGL runtime
// name loaded from anywhere else (ENB, ReShade or DXVK in the game folder) is
// that injector, a mod; otherwise the file name decides. Without a path only
// the name is known, so a d3d11.dll stays a system module.
bool IsSystemishModuleAt(std::wstring_view filename, std::wstring_view modulePath);
// Hooks that sit under most stacks without driving them: the SKSE runtime,
// Crash Logger and MO2's virtual file system.
bool IsPassiveHookModule(std::wstring_view filename);
bool IsGameExeModule(std::wstring_view filename);
bool IsSkseModule(std::wstring_view filename);
// Best-effort override for known hook framework DLL list.
// If JSON load fails or provides no valid entries, built-in defaults stay active.
void LoadHookFrameworksFromJson(const std::filesystem::path& jsonPath);
bool IsKnownHookFramework(std::wstring_view filename);
// Software outside the game and its mods that injects into it (GPU vendor
// tools, overlays): "NVIDIA", "Steam overlay", ... or empty. Matched by file
// name (Crash Logger logs carry only names) or by its install folder; a copy
// under Windows stays a system module.
std::wstring ExternalSoftwareName(std::wstring_view filename, std::wstring_view modulePath = {});
bool IsExternalSoftwareModule(std::wstring_view filename, std::wstring_view modulePath = {});

struct ModuleInfo
{
  std::uint64_t base = 0;
  std::uint64_t end = 0;
  std::uint32_t time_date_stamp = 0;
  std::string version;
  std::wstring path;
  std::wstring filename;
  std::wstring inferred_mod_name;
  // ExternalSoftwareName(); such a module also counts as is_systemish.
  std::wstring external_software;
  bool is_systemish = false;
  bool is_game_exe = false;
  bool is_known_hook_framework = false;
};

std::vector<ModuleInfo> LoadAllModules(void* dumpBase, std::uint64_t dumpSize);
std::optional<std::size_t> FindModuleIndexForAddress(const std::vector<ModuleInfo>& mods, std::uint64_t addr);

// DLLs the process had unloaded before the capture (UnloadedModuleListStream;
// empty without it). Code that still calls into one crashes at an address no
// loaded module covers.
struct UnloadedModule
{
  std::uint64_t base = 0;
  std::uint64_t end = 0;
  std::wstring filename;
};
std::vector<UnloadedModule> LoadUnloadedModules(void* dumpBase, std::uint64_t dumpSize);
// The most recently unloaded module covering addr (the list is oldest first).
const UnloadedModule* FindUnloadedModule(const std::vector<UnloadedModule>& mods, std::uint64_t addr);

struct ThreadRecord
{
  std::uint32_t tid = 0;
  MINIDUMP_MEMORY_DESCRIPTOR stack{};
  MINIDUMP_LOCATION_DESCRIPTOR context{};
};

std::vector<ThreadRecord> LoadThreads(void* dumpBase, std::uint64_t dumpSize);
// Thread id -> start address, from the ThreadInfoList stream (empty without it).
std::vector<std::pair<std::uint32_t, std::uint64_t>> LoadThreadStartAddresses(void* dumpBase, std::uint64_t dumpSize);
bool ReadThreadContextWin64(void* dumpBase, std::uint64_t dumpSize, const ThreadRecord& tr, CONTEXT& out);
bool GetThreadStackBytes(
  void* dumpBase,
  std::uint64_t dumpSize,
  const ThreadRecord& tr,
  const std::uint8_t*& outPtr,
  std::size_t& outSize,
  std::uint64_t& outBaseAddr);

}  // namespace skydiag::dump_tool::minidump
