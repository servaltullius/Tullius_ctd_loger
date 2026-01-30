#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "SkyrimDiagShared.h"

namespace skydiag::helper {

struct AttachedProcess {
  std::uint32_t pid = 0;
  HANDLE process = nullptr;

  HANDLE shmMapping = nullptr;
  const skydiag::SharedLayout* shm = nullptr;
  std::size_t shmSize = 0;  // mapped bytes (best-effort)

  HANDLE crashEvent = nullptr;
};

bool AttachByPid(std::uint32_t pid, AttachedProcess& out, std::wstring* err);
bool FindAndAttach(AttachedProcess& out, std::wstring* err);
void Detach(AttachedProcess& p);

}  // namespace skydiag::helper
