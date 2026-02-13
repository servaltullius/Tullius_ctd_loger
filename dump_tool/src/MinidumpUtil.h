#pragma once

#include <Windows.h>

#include <DbgHelp.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool::minidump {

struct MappedFile
{
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE mapping = nullptr;
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
  std::wstring path;
  std::wstring filename;
  std::wstring plusOffset;
};

std::optional<ModuleHit> ModuleForAddress(void* dumpBase, std::uint64_t dumpSize, std::uint64_t addr);

std::wstring WideLower(std::wstring_view s);

bool IsSystemishModule(std::wstring_view filename);
bool IsGameExeModule(std::wstring_view filename);
bool IsKnownHookFramework(std::wstring_view filename);

struct ModuleInfo
{
  std::uint64_t base = 0;
  std::uint64_t end = 0;
  std::wstring path;
  std::wstring filename;
  std::wstring inferred_mod_name;
  bool is_systemish = false;
  bool is_game_exe = false;
  bool is_known_hook_framework = false;
};

std::vector<ModuleInfo> LoadAllModules(void* dumpBase, std::uint64_t dumpSize);
std::optional<std::size_t> FindModuleIndexForAddress(const std::vector<ModuleInfo>& mods, std::uint64_t addr);

struct ThreadRecord
{
  std::uint32_t tid = 0;
  MINIDUMP_MEMORY_DESCRIPTOR stack{};
  MINIDUMP_LOCATION_DESCRIPTOR context{};
};

std::vector<ThreadRecord> LoadThreads(void* dumpBase, std::uint64_t dumpSize);
bool ReadThreadContextWin64(void* dumpBase, std::uint64_t dumpSize, const ThreadRecord& tr, CONTEXT& out);
bool GetThreadStackBytes(
  void* dumpBase,
  std::uint64_t dumpSize,
  const ThreadRecord& tr,
  const std::uint8_t*& outPtr,
  std::size_t& outSize,
  std::uint64_t& outBaseAddr);

}  // namespace skydiag::dump_tool::minidump

