#include "SkyrimDiagHelper/DumpWriter.h"

#include <Windows.h>

#include <DbgHelp.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "SkyrimDiagProtocol.h"

namespace skydiag::helper {
namespace {

// Where MiniDumpWriteDump was when it failed. CI runs intermittently fail with
// 0x800706F8 (ERROR_INVALID_USER_BUFFER) on self-dump tests and never
// locally, so the failure message carries the last callback the writer made,
// any memory reads it reported as failed and, when I/O tracing is on, the
// file write that failed.
struct DumpProgress
{
  ULONG lastCallbackType = 0;
  ULONG64 lastCallbackSubject = 0;  // thread id or module base, when the callback has one
  ULONG readFailureCount = 0;
  ULONG64 firstFailedReadOffset = 0;
  ULONG firstFailedReadBytes = 0;
  HRESULT firstFailedReadStatus = S_OK;

  bool ioTraced = false;
  ULONG ioWriteCount = 0;
  ULONG64 ioBytesWritten = 0;
  ULONG64 failedWriteOffset = 0;
  ULONG failedWriteBytes = 0;
  HRESULT failedWriteStatus = S_OK;     // the single WriteFile dbghelp would have made
  HRESULT chunkedRetryStatus = S_OK;    // the same bytes written again in 1 MiB pieces

  void Reset() noexcept { *this = DumpProgress{}; }
};

struct DumpCallbackContext
{
  DumpProfile profile{};
  DWORD preferredThreadId = 0;
  std::vector<DWORD> preferredThreadIds;
  bool isProcessSnapshot = false;
  // SKYDIAG_DUMP_IO_TRACE=1 (set by CI, never by the game setup): the callback
  // performs the dump's file writes itself so a failing write is recorded.
  bool traceIo = false;
  DumpProgress progress{};
};

std::atomic<std::uint64_t> g_tracedDumpWrites{ 0 };

bool DumpIoTraceRequested()
{
  wchar_t value[8]{};
  const DWORD n = GetEnvironmentVariableW(L"SKYDIAG_DUMP_IO_TRACE", value, static_cast<DWORD>(std::size(value)));
  return n == 1 && value[0] == L'1';
}

HRESULT WriteAllAt(HANDLE file, ULONG64 offset, const BYTE* bytes, ULONG size, ULONG maxChunk)
{
  LARGE_INTEGER pos{};
  pos.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(file, pos, nullptr, FILE_BEGIN)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  while (size > 0) {
    const DWORD chunk = std::min<ULONG>(size, maxChunk);
    DWORD written = 0;
    if (!WriteFile(file, bytes, chunk, &written, nullptr)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    if (written == 0) {
      return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    }
    bytes += written;
    size -= written;
  }
  return S_OK;
}

// IoWriteAllCallback: write the block the way dbghelp would (one WriteFile).
// A failure is recorded together with whether the same bytes could be written
// in 1 MiB pieces, which tells a large-buffer failure apart from one tied to
// the bytes or the file. The write still fails as dbghelp's would, so tracing
// diagnoses the failure without hiding it.
HRESULT TracedWriteAll(const MINIDUMP_IO_CALLBACK& io, DumpProgress& progress)
{
  const auto* bytes = static_cast<const BYTE*>(io.Buffer);
  const HRESULT hr = WriteAllAt(io.Handle, io.Offset, bytes, io.BufferBytes, io.BufferBytes == 0 ? 1u : io.BufferBytes);
  ++progress.ioWriteCount;
  g_tracedDumpWrites.fetch_add(1, std::memory_order_relaxed);
  if (SUCCEEDED(hr)) {
    progress.ioBytesWritten += io.BufferBytes;
  } else if (progress.failedWriteStatus == S_OK) {
    progress.failedWriteOffset = io.Offset;
    progress.failedWriteBytes = io.BufferBytes;
    progress.failedWriteStatus = hr;
    progress.chunkedRetryStatus = WriteAllAt(io.Handle, io.Offset, bytes, io.BufferBytes, 1u << 20);
  }
  return hr;
}

// File version of the dbghelp.dll this process loaded, e.g. "10.0.20348.1 C:\...\dbghelp.dll".
std::wstring DescribeLoadedDbgHelp()
{
  HMODULE module = GetModuleHandleW(L"dbghelp.dll");
  if (!module) {
    return L"not loaded";
  }
  wchar_t path[MAX_PATH]{};
  if (GetModuleFileNameW(module, path, MAX_PATH) == 0) {
    return L"unknown";
  }
  std::wstring version = L"?";
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(path, &handle);
  if (size > 0) {
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiSize = 0;
    if (GetFileVersionInfoW(path, 0, size, data.data()) &&
        VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffiSize) && ffi) {
      version = std::to_wstring(HIWORD(ffi->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(ffi->dwFileVersionMS)) +
        L"." + std::to_wstring(HIWORD(ffi->dwFileVersionLS)) + L"." + std::to_wstring(LOWORD(ffi->dwFileVersionLS));
    }
  }
  return version + L" " + path;
}

ULONG64 CallbackSubject(const MINIDUMP_CALLBACK_INPUT& input) noexcept
{
  switch (input.CallbackType) {
    case ThreadCallback:
    case ThreadExCallback:
      return input.Thread.ThreadId;
    case IncludeThreadCallback:
      return input.IncludeThread.ThreadId;
    case ModuleCallback:
      return input.Module.BaseOfImage;
    case IncludeModuleCallback:
      return input.IncludeModule.BaseOfImage;
    default:
      return 0;
  }
}

std::wstring Hex(ULONG64 value)
{
  wchar_t buf[32]{};
  swprintf_s(buf, L"0x%llX", static_cast<unsigned long long>(value));
  return buf;
}

std::wstring DescribeDumpProgress(const DumpProgress& progress)
{
  std::wstring text = L"last callback type=" + std::to_wstring(progress.lastCallbackType);
  if (progress.lastCallbackSubject != 0) {
    text += L" subject=" + Hex(progress.lastCallbackSubject);
  }
  text += L", memory read failures=" + std::to_wstring(progress.readFailureCount);
  if (progress.readFailureCount != 0) {
    text += L" (first at " + Hex(progress.firstFailedReadOffset) + L" size=" +
      std::to_wstring(progress.firstFailedReadBytes) + L" status=" +
      Hex(static_cast<ULONG>(progress.firstFailedReadStatus)) + L")";
  }
  if (progress.ioTraced) {
    text += L", io writes=" + std::to_wstring(progress.ioWriteCount) + L" bytes=" + std::to_wstring(progress.ioBytesWritten);
    if (progress.failedWriteStatus != S_OK) {
      text += L" (write at " + Hex(progress.failedWriteOffset) + L" size=" + std::to_wstring(progress.failedWriteBytes) +
        L" failed " + Hex(static_cast<ULONG>(progress.failedWriteStatus)) + L", 1 MiB retry " +
        (SUCCEEDED(progress.chunkedRetryStatus) ? std::wstring(L"ok") : Hex(static_cast<ULONG>(progress.chunkedRetryStatus))) +
        L")";
    }
  }
  return text;
}

void AppendPreferredThreadId(std::vector<DWORD>& preferredThreadIds, DWORD tid)
{
  if (tid == 0) {
    return;
  }
  if (std::find(preferredThreadIds.begin(), preferredThreadIds.end(), tid) == preferredThreadIds.end()) {
    preferredThreadIds.push_back(tid);
  }
}

std::vector<DWORD> ExtractPreferredWctThreadIds(std::string_view wctJsonUtf8)
{
  std::vector<DWORD> tids;
  if (wctJsonUtf8.empty()) {
    return tids;
  }

  const auto json = nlohmann::json::parse(wctJsonUtf8, nullptr, /*allow_exceptions=*/false);
  if (!json.is_object() || !json.contains("threads") || !json["threads"].is_array()) {
    return tids;
  }

  for (const auto& thread : json["threads"]) {
    if (!thread.is_object() || !thread.value("isCycle", false)) {
      continue;
    }
    const auto tid = thread.value("tid", 0u);
    if (tid != 0u) {
      AppendPreferredThreadId(tids, tid);
    }
  }

  return tids;
}

std::optional<DWORD> InferMainThreadIdFromSnapshot(
  const skydiag::SharedLayout* snapshot,
  std::size_t snapshotBytes)
{
  if (!snapshot || snapshotBytes < offsetof(skydiag::SharedLayout, events)) {
    return std::nullopt;
  }
  if (snapshot->header.magic != skydiag::kMagic) {
    return std::nullopt;
  }

  const std::size_t availableEventBytes = snapshotBytes - offsetof(skydiag::SharedLayout, events);
  const std::size_t availableEvents = std::min<std::size_t>(
    skydiag::kEventCapacity,
    availableEventBytes / sizeof(skydiag::BlackboxEvent));
  std::uint32_t capacity = snapshot->header.capacity;
  if (capacity == 0u || capacity > availableEvents) {
    capacity = static_cast<std::uint32_t>(availableEvents);
  }
  if (capacity == 0u) {
    return std::nullopt;
  }

  const std::uint32_t writeIndex = snapshot->header.write_index;
  const std::uint32_t begin = (writeIndex > capacity) ? (writeIndex - capacity) : 0u;
  std::optional<DWORD> sessionStartTid;
  std::optional<DWORD> latestHeartbeatTid;
  for (std::uint32_t i = begin; i < writeIndex; ++i) {
    const auto& source = snapshot->events[i % capacity];
    const std::uint32_t seq1 = source.seq;
    if ((seq1 & 1u) != 0u) {
      continue;
    }
    skydiag::BlackboxEvent event{};
    std::memcpy(&event, &source, sizeof(event));
    const std::uint32_t seq2 = source.seq;
    if (seq1 != seq2 || (seq2 & 1u) != 0u || event.tid == 0u) {
      continue;
    }
    if (event.type == static_cast<std::uint16_t>(skydiag::EventType::kHeartbeat)) {
      latestHeartbeatTid = event.tid;
    } else if (!sessionStartTid &&
               event.type == static_cast<std::uint16_t>(skydiag::EventType::kSessionStart)) {
      sessionStartTid = event.tid;
    }
  }
  return latestHeartbeatTid ? latestHeartbeatTid : sessionStartTid;
}

bool ShouldShapePreferredThread(const DumpCallbackContext& ctx)
{
  return !ctx.preferredThreadIds.empty() && (ctx.profile.preferCrashContext || ctx.profile.preferMainThread ||
                                             ctx.profile.preferWctThreads);
}

bool IsPreferredThread(const DumpCallbackContext& ctx, DWORD threadId)
{
  return std::find(ctx.preferredThreadIds.begin(), ctx.preferredThreadIds.end(), threadId) !=
         ctx.preferredThreadIds.end();
}

MINIDUMP_TYPE ApplyProfileToDumpType(const DumpProfile& dumpProfile)
{
  MINIDUMP_TYPE t = MiniDumpNormal;
  if (dumpProfile.includeThreadInfo) {
    t = static_cast<MINIDUMP_TYPE>(t | MiniDumpWithThreadInfo);
  }
  if (dumpProfile.includeHandleData) {
    t = static_cast<MINIDUMP_TYPE>(t | MiniDumpWithHandleData);
  }
  if (dumpProfile.includeUnloadedModules) {
    t = static_cast<MINIDUMP_TYPE>(t | MiniDumpWithUnloadedModules);
  }
  if (dumpProfile.includeCodeSegments) {
    t = static_cast<MINIDUMP_TYPE>(t | MiniDumpWithCodeSegs);
  }
  if (dumpProfile.includeProcessThreadData) {
    t = static_cast<MINIDUMP_TYPE>(t | MiniDumpWithProcessThreadData);
  }
  if (dumpProfile.includeFullMemoryInfo) {
    t = static_cast<MINIDUMP_TYPE>(t | MiniDumpWithFullMemoryInfo);
  }
  if (dumpProfile.includeModuleHeaders) {
    t = static_cast<MINIDUMP_TYPE>(t | MiniDumpWithModuleHeaders);
  }
  if (dumpProfile.includeIndirectMemory) {
    t = static_cast<MINIDUMP_TYPE>(t | MiniDumpWithIndirectlyReferencedMemory);
  }
  if (dumpProfile.ignoreInaccessibleMemory) {
    t = static_cast<MINIDUMP_TYPE>(t | MiniDumpIgnoreInaccessibleMemory);
  }
  if (dumpProfile.includeFullMemory) {
    t = static_cast<MINIDUMP_TYPE>(t | MiniDumpWithFullMemory);
  }
  return t;
}

BOOL CALLBACK MiniDumpCallback(
  PVOID callbackParam,
  const PMINIDUMP_CALLBACK_INPUT callbackInput,
  PMINIDUMP_CALLBACK_OUTPUT callbackOutput)
{
  auto* ctx = static_cast<DumpCallbackContext*>(callbackParam);
  (void)callbackOutput;
  if (!ctx || !callbackInput) {
    return TRUE;
  }

  const auto callbackType = callbackInput->CallbackType;
  if (ctx->traceIo && callbackOutput) {
    // Kept out of lastCallbackType so that field still names the dump phase.
    if (callbackType == IoStartCallback) {
      ctx->progress.ioTraced = true;
      callbackOutput->Status = S_FALSE;  // the callback performs every write
      return TRUE;
    }
    if (callbackType == IoWriteAllCallback) {
      callbackOutput->Status = TracedWriteAll(callbackInput->Io, ctx->progress);
      return TRUE;
    }
    if (callbackType == IoFinishCallback) {
      callbackOutput->Status = S_OK;
      return TRUE;
    }
  }
  if (callbackType == ReadMemoryFailureCallback) {
    // Record the read and let the writer continue without that memory. The
    // record shows up in the failure message if the dump still fails.
    auto& progress = ctx->progress;
    if (progress.readFailureCount == 0) {
      progress.firstFailedReadOffset = callbackInput->ReadMemoryFailure.Offset;
      progress.firstFailedReadBytes = callbackInput->ReadMemoryFailure.Bytes;
      progress.firstFailedReadStatus = callbackInput->ReadMemoryFailure.FailureStatus;
    }
    ++progress.readFailureCount;
    if (callbackOutput) {
      callbackOutput->Status = S_OK;
    }
    return TRUE;
  }
  ctx->progress.lastCallbackType = static_cast<ULONG>(callbackType);
  ctx->progress.lastCallbackSubject = CallbackSubject(*callbackInput);
  if (callbackType == IsProcessSnapshotCallback && callbackOutput) {
    callbackOutput->Status = ctx->isProcessSnapshot ? S_FALSE : S_OK;
    return TRUE;
  }
  if (callbackType == IncludeThreadCallback && callbackOutput && ShouldShapePreferredThread(*ctx)) {
    const DWORD threadId = callbackInput->IncludeThread.ThreadId;
    if (IsPreferredThread(*ctx, threadId)) {
      callbackOutput->ThreadWriteFlags |= ThreadWriteInstructionWindow;
    } else if (ctx->profile.preferCrashContext || ctx->profile.preferWctThreads) {
      callbackOutput->ThreadWriteFlags &= ~ThreadWriteInstructionWindow;
    }
    return TRUE;
  }
  (void)callbackType;
  return TRUE;
}

}  // namespace

bool WriteDumpWithStreams(
  HANDLE process,
  std::uint32_t pid,
  const std::wstring& dumpPath,
  const skydiag::SharedLayout* shmSnapshot,
  std::size_t shmSnapshotBytes,
  const std::string& wctJsonUtf8,
  const std::string& pluginScanJson,
  bool isCrash,
  const DumpProfile& dumpProfile,
  bool isProcessSnapshot,
  std::wstring* err)
{
  if (!process) {
    if (err) *err = L"Invalid process handle";
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
  // vector<uint8_t> is not formally aligned for SharedLayout. Copy only the
  // header into an aligned local object instead of type-punning the byte
  // buffer; both streams still derive from the same immutable byte copy.
  skydiag::SharedHeader committedHeader{};
  const bool hasCommittedHeader = blackboxBytes.size() >= sizeof(committedHeader);
  if (hasCommittedHeader) {
    std::memcpy(&committedHeader, blackboxBytes.data(), sizeof(committedHeader));
  }

  std::vector<MINIDUMP_USER_STREAM> streams;
  streams.reserve(3);

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

  MINIDUMP_USER_STREAM s3{};
  if (!pluginScanJson.empty()) {
    s3.Type = skydiag::protocol::kMinidumpUserStream_PluginInfo;
    s3.BufferSize = static_cast<ULONG>(pluginScanJson.size());
    s3.Buffer = const_cast<char*>(pluginScanJson.data());
    streams.push_back(s3);
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
  if (isCrash && hasCommittedHeader &&
      committedHeader.crash_seq != 0u &&
      (committedHeader.crash_seq & 1u) == 0u) {
    // The exception stream and the blackbox user stream must originate from
    // this same immutable byte copy. Reading the live mapping again here can
    // combine a later exception with an earlier event/resource history.
    er = committedHeader.crash.exception_record;
    ctx = committedHeader.crash.context;
    ep.ExceptionRecord = &er;
    ep.ContextRecord = &ctx;

    mei.ThreadId = committedHeader.crash.faulting_tid;
    mei.ExceptionPointers = &ep;
    mei.ClientPointers = FALSE;  // pointers are in this process address space
    meiPtr = &mei;
  }

  const DumpProfile effectiveProfile = ResolveDumpProfile(dumpProfile.baseMode, dumpProfile.captureKind);
  MINIDUMP_TYPE dumpType = ApplyProfileToDumpType(effectiveProfile);
  DumpCallbackContext callbackContext{};
  callbackContext.profile = effectiveProfile;
  callbackContext.preferredThreadId = mei.ThreadId;
  AppendPreferredThreadId(callbackContext.preferredThreadIds, callbackContext.preferredThreadId);
  if (effectiveProfile.preferMainThread) {
    if (const auto mainTid = InferMainThreadIdFromSnapshot(shmSnapshot, shmSnapshotBytes)) {
      AppendPreferredThreadId(callbackContext.preferredThreadIds, *mainTid);
    }
  }
  if (effectiveProfile.preferWctThreads) {
    for (const DWORD tid : ExtractPreferredWctThreadIds(wctJsonUtf8)) {
      AppendPreferredThreadId(callbackContext.preferredThreadIds, tid);
    }
  }
  callbackContext.isProcessSnapshot = isProcessSnapshot;
  callbackContext.traceIo = DumpIoTraceRequested();
  MINIDUMP_CALLBACK_INFORMATION callbackInfo{};
  callbackInfo.CallbackRoutine = MiniDumpCallback;
  callbackInfo.CallbackParam = &callbackContext;

  static_assert(kMiniDumpIgnoreInaccessibleMemoryFlag == static_cast<std::uint32_t>(MiniDumpIgnoreInaccessibleMemory));

  // See ShouldRetryDumpIgnoringInaccessibleMemory: an ERROR_PARTIAL_COPY
  // failure is retried once at once with unreadable regions skipped.
  DWORD lastErr = ERROR_SUCCESS;
  for (int attempt = 0; attempt < 2; ++attempt) {
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

    callbackContext.progress.Reset();
    const BOOL ok = MiniDumpWriteDump(
      process,
      pid,
      file,
      dumpType,
      meiPtr,
      &usi,
      &callbackInfo);

    lastErr = GetLastError();
    CloseHandle(file);

    if (ok) {
      if (err) {
        err->clear();
      }
      return true;
    }
    if (!ShouldRetryDumpIgnoringInaccessibleMemory(static_cast<std::uint32_t>(dumpType), lastErr)) {
      break;
    }
    dumpType = static_cast<MINIDUMP_TYPE>(dumpType | MiniDumpIgnoreInaccessibleMemory);
  }

  if (err) {
    *err = L"MiniDumpWriteDump failed: " + std::to_wstring(lastErr) + L" (" + Hex(lastErr) + L", " +
      DescribeDumpProgress(callbackContext.progress) + L"; dbghelp " + DescribeLoadedDbgHelp() + L")";
  }
  return false;
}

std::uint64_t TracedDumpWriteCount() noexcept
{
  return g_tracedDumpWrites.load(std::memory_order_relaxed);
}

}  // namespace skydiag::helper
