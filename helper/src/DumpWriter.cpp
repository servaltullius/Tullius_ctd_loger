#include "SkyrimDiagHelper/DumpWriter.h"

#include <Windows.h>

#include <DbgHelp.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "SkyrimDiagProtocol.h"

namespace skydiag::helper {
namespace {

MINIDUMP_TYPE MakeDumpType(DumpMode mode)
{
  MINIDUMP_TYPE t = MiniDumpNormal;
  if (mode == DumpMode::kMini) {
    return t;
  }

  t = static_cast<MINIDUMP_TYPE>(t | MiniDumpWithThreadInfo | MiniDumpWithHandleData | MiniDumpWithUnloadedModules);
  if (mode == DumpMode::kFull) {
    t = static_cast<MINIDUMP_TYPE>(t | MiniDumpWithFullMemory);
  }

  return t;
}

}  // namespace

bool WriteDumpWithStreams(
  HANDLE process,
  std::uint32_t pid,
  const std::wstring& dumpPath,
  const skydiag::SharedLayout* shmSnapshot,
  std::size_t shmSnapshotBytes,
  const std::string& wctJsonUtf8,
  bool isCrash,
  DumpMode dumpMode,
  std::wstring* err)
{
  if (!process) {
    if (err) *err = L"Invalid process handle";
    return false;
  }

  HANDLE file = CreateFileW(
    dumpPath.c_str(),
    GENERIC_WRITE,
    0,
    nullptr,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (err) *err = L"CreateFileW failed: " + std::to_wstring(GetLastError());
    return false;
  }

  // ---- build user streams ----
  std::vector<std::uint8_t> blackboxBytes;
  if (shmSnapshot) {
    const std::size_t want = sizeof(skydiag::SharedLayout);
    const std::size_t got = (shmSnapshotBytes > 0) ? std::min<std::size_t>(want, shmSnapshotBytes) : want;
    blackboxBytes.resize(got);
    std::memcpy(blackboxBytes.data(), shmSnapshot, got);
  }

  std::vector<MINIDUMP_USER_STREAM> streams;
  streams.reserve(2);

  MINIDUMP_USER_STREAM s1{};
  s1.Type = skydiag::protocol::kMinidumpUserStream_Blackbox;
  s1.BufferSize = static_cast<ULONG>(blackboxBytes.size());
  s1.Buffer = blackboxBytes.empty() ? nullptr : blackboxBytes.data();
  streams.push_back(s1);

  MINIDUMP_USER_STREAM s2{};
  if (!wctJsonUtf8.empty()) {
    s2.Type = skydiag::protocol::kMinidumpUserStream_WctJson;
    s2.BufferSize = static_cast<ULONG>(wctJsonUtf8.size());
    s2.Buffer = const_cast<char*>(wctJsonUtf8.data());
    streams.push_back(s2);
  }

  MINIDUMP_USER_STREAM_INFORMATION usi{};
  usi.UserStreamCount = static_cast<ULONG>(streams.size());
  usi.UserStreamArray = streams.empty() ? nullptr : streams.data();

  // ---- exception info (crash only) ----
  MINIDUMP_EXCEPTION_INFORMATION mei{};
  EXCEPTION_POINTERS ep{};
  EXCEPTION_RECORD er{};
  CONTEXT ctx{};

  MINIDUMP_EXCEPTION_INFORMATION* meiPtr = nullptr;
  if (isCrash && shmSnapshot && shmSnapshot->header.crash_seq != 0) {
    er = shmSnapshot->header.crash.exception_record;
    ctx = shmSnapshot->header.crash.context;
    ep.ExceptionRecord = &er;
    ep.ContextRecord = &ctx;

    mei.ThreadId = shmSnapshot->header.crash.faulting_tid;
    mei.ExceptionPointers = &ep;
    mei.ClientPointers = FALSE;  // pointers are in this process address space
    meiPtr = &mei;
  }

  const MINIDUMP_TYPE dumpType = MakeDumpType(dumpMode);

  const BOOL ok = MiniDumpWriteDump(
    process,
    pid,
    file,
    dumpType,
    meiPtr,
    &usi,
    nullptr);

  const DWORD lastErr = GetLastError();
  CloseHandle(file);

  if (!ok) {
    if (err) *err = L"MiniDumpWriteDump failed: " + std::to_wstring(lastErr);
    return false;
  }

  if (err) {
    err->clear();
  }
  return true;
}

}  // namespace skydiag::helper
