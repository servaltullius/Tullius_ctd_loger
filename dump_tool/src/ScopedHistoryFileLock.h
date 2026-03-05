#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace skydiag::dump_tool {

// Uses a Windows Named Mutex instead of a directory-based lock.
// Named Mutex is automatically released by the OS when the owning process terminates,
// preventing stale locks after crashes.
class ScopedHistoryFileLock
{
public:
  explicit ScopedHistoryFileLock(const std::filesystem::path& historyPath)
  {
    // Derive a unique mutex name from the history file path.
    std::wstring name = L"Local\\SkyrimDiag_HistoryLock_";
    // Use a simple hash of the path to keep the name short and unique.
    std::hash<std::wstring> hasher;
    name += std::to_wstring(hasher(historyPath.wstring()));
    m_mutex = CreateMutexW(nullptr, FALSE, name.c_str());
  }

  bool Acquire(DWORD timeoutMs, [[maybe_unused]] DWORD pollMs = 25)
  {
    if (m_acquired) {
      return true;
    }
    if (!m_mutex) {
      return false;
    }
    const DWORD result = WaitForSingleObject(m_mutex, timeoutMs);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
      if (result == WAIT_ABANDONED) {
        OutputDebugStringW(L"SkyrimDiag: history lock acquired via WAIT_ABANDONED (previous owner crashed)\n");
      }
      m_acquired = true;
      return true;
    }
    return false;
  }

  ~ScopedHistoryFileLock()
  {
    if (m_acquired && m_mutex) {
      ReleaseMutex(m_mutex);
    }
    if (m_mutex) {
      CloseHandle(m_mutex);
    }
  }

  ScopedHistoryFileLock(const ScopedHistoryFileLock&) = delete;
  ScopedHistoryFileLock& operator=(const ScopedHistoryFileLock&) = delete;

private:
  HANDLE m_mutex = nullptr;
  bool m_acquired = false;
};

}  // namespace skydiag::dump_tool
