#pragma once

#include <Windows.h>

extern "C" {

// C ABI for WinUI/native interop.
// Returns 0 on success. Non-zero follows DumpTool process-style exit codes:
// 2: invalid arguments, 3: analysis failed (including native exceptions), 4: output write failed.
__declspec(dllexport) int __stdcall SkyrimDiagAnalyzeDumpW(
  const wchar_t* dumpPath,
  const wchar_t* outDir,
  const wchar_t* languageToken,
  int debug,
  wchar_t* errorBuf,
  int errorBufChars);

}
