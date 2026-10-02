#pragma once

#include <Windows.h>

#include <TlHelp32.h>

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
#include <thread>

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

// A worker thread of this process parked in a wait. Synthetic crash records
// name it as the faulting thread so crash-capture tests dump this process.
// Dumping a freshly launched external process (cmd.exe) intermittently failed
// on GitHub Windows runners with ERROR_PARTIAL_COPY even after it settled,
// while self-process dumps (the hang tests) have been stable there.
class ParkedThread
{
public:
  ParkedThread()
    : release_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      parked_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
  {
    Require(release_ != nullptr && parked_ != nullptr, "CreateEventW failed");
    thread_ = std::thread([this]() {
      tid_ = GetCurrentThreadId();
      SetEvent(parked_);
      WaitForSingleObject(release_, INFINITE);
    });
    Require(WaitForSingleObject(parked_, 5000) == WAIT_OBJECT_0, "Parked thread did not start");
  }

  ParkedThread(const ParkedThread&) = delete;
  ParkedThread& operator=(const ParkedThread&) = delete;

  ~ParkedThread()
  {
    SetEvent(release_);
    if (thread_.joinable()) {
      thread_.join();
    }
    CloseHandle(release_);
    CloseHandle(parked_);
  }

  DWORD tid() const { return tid_; }

  CONTEXT Context() const
  {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid_);
    Require(thread != nullptr, "OpenThread failed");
    Require(SuspendThread(thread) != static_cast<DWORD>(-1), "SuspendThread failed");
    const BOOL ok = GetThreadContext(thread, &ctx);
    ResumeThread(thread);
    CloseHandle(thread);
    Require(ok != FALSE, "GetThreadContext failed");
    return ctx;
  }

private:
  HANDLE release_ = nullptr;
  HANDLE parked_ = nullptr;
  DWORD tid_ = 0;
  std::thread thread_;
};

inline skydiag::helper::AttachedProcess MakeSelfAttachedProcess(skydiag::SharedLayout* shared)
{
  skydiag::helper::AttachedProcess proc{};
  proc.pid = GetCurrentProcessId();
  proc.process = OpenSelfProcessHandle();
  proc.shm = shared;
  proc.shmWritable = shared;
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

inline bool HasChildProcessNamed(DWORD parentPid, const wchar_t* exeName)
{
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return false;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &entry); ok && !found; ok = Process32NextW(snap, &entry)) {
    found = entry.th32ParentProcessID == parentPid && _wcsicmp(entry.szExeFile, exeName) == 0;
  }
  CloseHandle(snap);
  return found;
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
  // A process dumped while the loader is still mapping modules makes
  // MiniDumpWriteDump fail with ERROR_PARTIAL_COPY. cmd.exe starts ping.exe
  // only after it has initialized, so wait for that child before returning.
  // Waiting for any child is not enough: conhost.exe is also a child of a
  // console process and appears early in its startup.
  bool settled = false;
  for (int attempt = 0; attempt < 200 && !settled; ++attempt) {
    settled = HasChildProcessNamed(child.pi.dwProcessId, L"PING.EXE");
    if (!settled) {
      Sleep(25);
    }
  }
  Require(settled, "cmd.exe did not start ping.exe within 5 seconds");
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
  proc.shmWritable = shared;
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

// Closes a MessageBox shown by another thread of this process and waits until
// it is gone. A single-button MessageBox gives its OK button the IDCANCEL id,
// so WM_COMMAND/IDOK is ignored; WM_CLOSE works for every MessageBox type.
inline bool CloseDialogAndWait(const wchar_t* title, DWORD timeoutMs = 10000)
{
  const ULONGLONG deadline = GetTickCount64() + timeoutMs;
  while (GetTickCount64() < deadline) {
    const HWND hwnd = FindWindowW(L"#32770", title);
    if (!hwnd) {
      return true;
    }
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    Sleep(50);
  }
  return FindWindowW(L"#32770", title) == nullptr;
}

inline void AssertContains(std::string_view haystack, std::string_view needle, const char* message)
{
  Require(haystack.find(needle) != std::string_view::npos, message);
}

}  // namespace skydiag::tests::runtime
