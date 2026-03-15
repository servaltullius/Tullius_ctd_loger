#pragma once

#include <Windows.h>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "SkyrimDiagHelper/Config.h"
#include "SkyrimDiagHelper/ProcessAttach.h"
#include "SkyrimDiagShared.h"

namespace skydiag::tests::runtime {

[[noreturn]] inline void Fail(std::string_view message)
{
  std::cerr << "helper-runtime-test failure: " << message << '\n';
  std::cerr.flush();
  ExitProcess(1);
}

inline void Require(bool condition, std::string_view message)
{
  if (!condition) {
    Fail(message);
  }
}

inline std::filesystem::path MakeTempDir(const wchar_t* prefix)
{
  wchar_t tempPath[MAX_PATH]{};
  const DWORD tempPathLen = GetTempPathW(MAX_PATH, tempPath);
  Require(tempPathLen > 0 && tempPathLen < MAX_PATH, "GetTempPathW failed");

  wchar_t tempFile[MAX_PATH]{};
  const UINT uniqueResult = GetTempFileNameW(tempPath, prefix, 0, tempFile);
  Require(uniqueResult != 0, "GetTempFileNameW failed");

  std::error_code ec;
  std::filesystem::remove(tempFile, ec);
  ec.clear();
  std::filesystem::create_directories(tempFile, ec);
  Require(!ec, "Failed to create temp directory");
  return tempFile;
}

inline std::string ReadAllTextUtf8(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::in | std::ios::binary);
  Require(static_cast<bool>(in), "Failed to open file");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline void WriteAllTextUtf8(const std::filesystem::path& path, std::string_view text)
{
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "Failed to open file for writing");
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

inline std::filesystem::path FindSingleFileWithExt(const std::filesystem::path& dir, std::wstring_view ext)
{
  std::error_code ec;
  std::filesystem::path found;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    Require(!ec, "Failed to enumerate directory");
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    if (entry.path().extension() == ext) {
      if (!found.empty()) {
        Fail("Expected exactly one matching file");
      }
      found = entry.path();
    }
  }
  Require(!found.empty(), "Expected matching file was not found");
  return found;
}

inline std::filesystem::path FindSingleFileByPrefix(
  const std::filesystem::path& dir,
  std::wstring_view prefix,
  std::wstring_view ext)
{
  std::error_code ec;
  std::filesystem::path found;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    Require(!ec, "Failed to enumerate directory");
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const auto name = entry.path().filename().wstring();
    if (name.rfind(prefix, 0) != 0 || entry.path().extension() != ext) {
      continue;
    }
    if (!found.empty()) {
      Fail("Expected exactly one matching prefixed file");
    }
    found = entry.path();
  }
  Require(!found.empty(), "Expected prefixed file was not found");
  return found;
}

inline bool FileExists(const std::filesystem::path& path)
{
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

inline std::wstring ToWide(std::string_view text)
{
  if (text.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(
    CP_UTF8,
    0,
    text.data(),
    static_cast<int>(text.size()),
    nullptr,
    0);
  Require(needed > 0, "MultiByteToWideChar size query failed");
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  const int written = MultiByteToWideChar(
    CP_UTF8,
    0,
    text.data(),
    static_cast<int>(text.size()),
    out.data(),
    needed);
  Require(written == needed, "MultiByteToWideChar conversion failed");
  return out;
}

inline HANDLE OpenSelfProcessHandle()
{
  const HANDLE process = OpenProcess(
    PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE | SYNCHRONIZE,
    FALSE,
    GetCurrentProcessId());
  Require(process != nullptr, "OpenProcess(self) failed");
  return process;
}

inline skydiag::helper::AttachedProcess MakeSelfAttachedProcess(skydiag::SharedLayout* shared)
{
  skydiag::helper::AttachedProcess proc{};
  proc.pid = GetCurrentProcessId();
  proc.process = OpenSelfProcessHandle();
  proc.shm = shared;
  proc.shmSize = sizeof(skydiag::SharedLayout);
  return proc;
}

inline void CloseAttachedProcess(skydiag::helper::AttachedProcess* proc)
{
  if (!proc) {
    return;
  }
  if (proc->crashEvent) {
    CloseHandle(proc->crashEvent);
    proc->crashEvent = nullptr;
  }
  if (proc->process) {
    CloseHandle(proc->process);
    proc->process = nullptr;
  }
}

inline std::unique_ptr<skydiag::SharedLayout> MakeSharedLayout()
{
  auto layout = std::make_unique<skydiag::SharedLayout>();
  layout->header.magic = skydiag::kMagic;
  layout->header.version = skydiag::kVersion;
  layout->header.pid = GetCurrentProcessId();
  layout->header.capacity = skydiag::kEventCapacity;
  layout->header.qpc_freq = 10'000'000ull;
  layout->header.start_qpc = 1ull;
  layout->header.last_heartbeat_qpc = 2ull;
  layout->header.state_flags = 0u;
  return layout;
}

struct ChildProcess
{
  PROCESS_INFORMATION pi{};
};

inline std::wstring GetCmdExePath()
{
  wchar_t systemDir[MAX_PATH]{};
  const UINT len = GetSystemDirectoryW(systemDir, MAX_PATH);
  Require(len > 0 && len < MAX_PATH, "GetSystemDirectoryW failed");
  return std::filesystem::path(systemDir).append(L"cmd.exe").wstring();
}

inline ChildProcess LaunchSleepingChildProcess()
{
  ChildProcess child{};
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  std::wstring commandLine = L"cmd.exe /c ping -n 30 127.0.0.1 >nul";
  const auto applicationPath = GetCmdExePath();
  const BOOL ok = CreateProcessW(
    applicationPath.c_str(),
    commandLine.data(),
    nullptr,
    nullptr,
    FALSE,
    CREATE_NO_WINDOW,
    nullptr,
    nullptr,
    &si,
    &child.pi);
  Require(ok != FALSE, "CreateProcessW(cmd.exe) failed");
  if (child.pi.hThread) {
    CloseHandle(child.pi.hThread);
    child.pi.hThread = nullptr;
  }
  return child;
}

inline void TerminateChildProcess(ChildProcess* child)
{
  if (!child || !child->pi.hProcess) {
    return;
  }
  TerminateProcess(child->pi.hProcess, 0);
  WaitForSingleObject(child->pi.hProcess, 5000);
  CloseHandle(child->pi.hProcess);
  child->pi.hProcess = nullptr;
}

inline skydiag::helper::AttachedProcess MakeAttachedProcessForChild(
  const ChildProcess& child,
  skydiag::SharedLayout* shared)
{
  skydiag::helper::AttachedProcess proc{};
  proc.pid = child.pi.dwProcessId;
  proc.process = child.pi.hProcess;
  proc.shm = shared;
  proc.shmSize = sizeof(skydiag::SharedLayout);
  return proc;
}

inline skydiag::helper::HelperConfig MakeTestConfig()
{
  skydiag::helper::HelperConfig cfg{};
  cfg.autoAnalyzeDump = false;
  cfg.autoOpenViewerOnCrash = false;
  cfg.autoOpenViewerOnHang = false;
  cfg.autoOpenHangAfterProcessExit = false;
  cfg.enableIncidentManifest = true;
  cfg.enableEtwCaptureOnCrash = false;
  cfg.enableEtwCaptureOnHang = false;
  cfg.enableAutoRecaptureOnUnknownCrash = false;
  cfg.preserveFilteredCrashDumps = false;
  cfg.enablePssSnapshotForFreeze = false;
  return cfg;
}

inline void AssertContains(std::string_view haystack, std::string_view needle, const char* message)
{
  Require(haystack.find(needle) != std::string_view::npos, message);
}

}  // namespace skydiag::tests::runtime
