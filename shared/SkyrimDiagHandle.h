#pragma once

#include <Windows.h>

#include <memory>

namespace skydiag {

// RAII wrapper for Win32 HANDLE values.
// Handles both nullptr and INVALID_HANDLE_VALUE sentinels.
struct HandleDeleter
{
  using pointer = HANDLE;
  void operator()(HANDLE h) const noexcept
  {
    if (h != nullptr && h != INVALID_HANDLE_VALUE)
      ::CloseHandle(h);
  }
};

using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

}  // namespace skydiag
