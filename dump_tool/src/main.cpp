#include <Windows.h>

#include <commdlg.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Analyzer.h"
#include "GuiApp.h"
#include "OutputWriter.h"

namespace skydiag::dump_tool {
namespace {

std::optional<std::wstring> PickDumpFile()
{
  wchar_t fileBuf[MAX_PATH]{};

  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = nullptr;
  ofn.lpstrFile = fileBuf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"Minidump (*.dmp)\0*.dmp\0All files\0*.*\0\0";
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  ofn.lpstrDefExt = L"dmp";

  if (!GetOpenFileNameW(&ofn)) {
    return std::nullopt;
  }
  return std::wstring(fileBuf);
}

bool WriteErrorFile(const std::filesystem::path& outDir, const std::filesystem::path& dumpPath, const std::wstring& err)
{
  std::error_code ec;
  std::filesystem::create_directories(outDir, ec);

  const auto stem = dumpPath.stem().wstring();
  const auto p = outDir / (stem + L"_SkyrimDiagDumpToolError.txt");
  std::wofstream f(p);
  if (!f) {
    return false;
  }
  f << err;
  return true;
}

}  // namespace
}  // namespace skydiag::dump_tool

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
  using namespace skydiag::dump_tool;

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  std::wstring dumpPath;
  std::wstring outDir;
  bool headless = false;
  bool debug = false;

  for (int i = 1; i < argc; i++) {
    const std::wstring_view a = argv[i];
    if (a == L"--help" || a == L"-h" || a == L"/?") {
      MessageBoxW(
        nullptr,
        L"SkyrimDiagDumpTool (Viewer)\n\n"
        L"사용법:\n"
        L"  - 더블클릭: 덤프 선택창 → 분석/뷰어\n"
        L"  - 드래그&드롭: .dmp를 뷰어에 드롭\n"
        L"  - 헬퍼 자동 분석: --headless (UI 없이 결과 파일 생성)\n\n"
        L"옵션:\n"
        L"  --out-dir <dir>\n"
        L"  --headless\n",
        L"SkyrimDiagDumpTool",
        MB_ICONINFORMATION);
      LocalFree(argv);
      return 0;
    }
    if (a == L"--out-dir" && i + 1 < argc) {
      outDir = argv[++i];
      continue;
    }
    if (a == L"--headless") {
      headless = true;
      continue;
    }
    if (a == L"--debug") {
      debug = true;
      continue;
    }
    if (!a.empty() && a[0] == L'-') {
      continue;
    }
    if (dumpPath.empty()) {
      dumpPath = std::wstring(a);
      continue;
    }
  }

  if (dumpPath.empty() && !headless) {
    auto picked = PickDumpFile();
    if (!picked) {
      LocalFree(argv);
      return 0;
    }
    dumpPath = std::move(*picked);
  }

  if (dumpPath.empty() && headless) {
    LocalFree(argv);
    return 2;
  }

  const std::filesystem::path dumpFs(dumpPath);
  const std::filesystem::path outBase = outDir.empty() ? dumpFs.parent_path() : std::filesystem::path(outDir);

  AnalyzeOptions analyzeOpt{};
  analyzeOpt.debug = debug;

  AnalysisResult r{};
  std::wstring err;
  if (!AnalyzeDump(dumpPath, outBase.wstring(), analyzeOpt, r, &err)) {
    if (headless) {
      WriteErrorFile(outBase, dumpFs, err);
      LocalFree(argv);
      return 3;
    }
    MessageBoxW(nullptr, (L"덤프 분석 실패:\n" + err).c_str(), L"SkyrimDiagDumpTool", MB_ICONERROR);
    LocalFree(argv);
    return 3;
  }

  if (!WriteOutputs(r, &err)) {
    if (headless) {
      WriteErrorFile(outBase, dumpFs, err);
      LocalFree(argv);
      return 4;
    }
    MessageBoxW(nullptr, (L"결과 파일 생성 실패:\n" + err).c_str(), L"SkyrimDiagDumpTool", MB_ICONERROR);
    LocalFree(argv);
    return 4;
  }

  if (headless) {
    LocalFree(argv);
    return 0;
  }

  GuiOptions guiOpt{};
  guiOpt.debug = debug;

  const int rc = RunGuiViewer(hInst, guiOpt, analyzeOpt, std::move(r), &err);
  LocalFree(argv);
  return rc;
}
