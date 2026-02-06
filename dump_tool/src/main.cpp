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
#include "DumpToolConfig.h"
#include "GuiApp.h"
#include "I18nCore.h"
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
  using skydiag::dump_tool::i18n::Language;

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  std::wstring dumpPath;
  std::wstring outDir;
  bool headless = false;
  bool debug = false;
  bool showHelp = false;

  // Resolve UI/output language: ini default, CLI override.
  std::wstring cfgErr;
  const DumpToolConfig cfg = LoadDumpToolConfig(&cfgErr);
  Language lang = cfg.language;
  bool beginnerMode = cfg.beginnerMode;

  for (int i = 1; i < argc; i++) {
    const std::wstring_view a = argv[i];
    if (a == L"--help" || a == L"-h" || a == L"/?") {
      showHelp = true;
      continue;
    }
    if (a == L"--out-dir" && i + 1 < argc) {
      outDir = argv[++i];
      continue;
    }
    if ((a == L"--lang" || a == L"--language") && i + 1 < argc) {
      const std::wstring v = argv[++i];
      std::string ascii;
      ascii.reserve(v.size());
      for (const wchar_t c : v) {
        if (c >= 0 && c <= 0x7f) {
          ascii.push_back(static_cast<char>(c));
        }
      }
      lang = i18n::ParseLanguageTokenAscii(ascii);
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
    if (a == L"--simple-ui") {
      beginnerMode = true;
      continue;
    }
    if (a == L"--advanced-ui") {
      beginnerMode = false;
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

  if (showHelp) {
    const wchar_t* msg_en =
      L"SkyrimDiagDumpTool (Viewer)\n\n"
      L"Usage:\n"
      L"  - Double click: pick a dump and open the viewer\n"
      L"  - Drag & drop: drop a .dmp onto the viewer\n"
      L"  - Helper auto analysis: --headless (write output files without UI)\n\n"
      L"Options:\n"
      L"  --out-dir <dir>\n"
      L"  --headless\n"
      L"  --lang en|ko\n"
      L"  --simple-ui | --advanced-ui\n";

    const wchar_t* msg_ko =
      L"SkyrimDiagDumpTool (Viewer)\n\n"
      L"사용법:\n"
      L"  - 더블클릭: 덤프 선택창 → 분석/뷰어\n"
      L"  - 드래그&드롭: .dmp를 뷰어에 드롭\n"
      L"  - 헬퍼 자동 분석: --headless (UI 없이 결과 파일 생성)\n\n"
      L"옵션:\n"
      L"  --out-dir <dir>\n"
      L"  --headless\n"
      L"  --lang en|ko\n"
      L"  --simple-ui | --advanced-ui\n";

    MessageBoxW(nullptr, (lang == Language::kKorean) ? msg_ko : msg_en, L"SkyrimDiagDumpTool", MB_ICONINFORMATION);
    LocalFree(argv);
    return 0;
  }

  const std::filesystem::path dumpFs(dumpPath);
  const std::filesystem::path outBase = outDir.empty() ? dumpFs.parent_path() : std::filesystem::path(outDir);

  AnalyzeOptions analyzeOpt{};
  analyzeOpt.debug = debug;
  analyzeOpt.language = lang;
  GuiOptions guiOpt{};
  guiOpt.debug = debug;
  guiOpt.beginnerMode = beginnerMode;

  if (!headless) {
    std::wstring reuseErr;
    if (TryReuseExistingViewerForDump(dumpPath, analyzeOpt, guiOpt, &reuseErr)) {
      LocalFree(argv);
      return 0;
    }
  }

  AnalysisResult r{};
  std::wstring err;
  if (!AnalyzeDump(dumpPath, outBase.wstring(), analyzeOpt, r, &err)) {
    if (headless) {
      WriteErrorFile(outBase, dumpFs, err);
      LocalFree(argv);
      return 3;
    }
    MessageBoxW(nullptr, ((lang == Language::kKorean ? L"덤프 분석 실패:\n" : L"Dump analysis failed:\n") + err).c_str(), L"SkyrimDiagDumpTool", MB_ICONERROR);
    LocalFree(argv);
    return 3;
  }

  if (!WriteOutputs(r, &err)) {
    if (headless) {
      WriteErrorFile(outBase, dumpFs, err);
      LocalFree(argv);
      return 4;
    }
    MessageBoxW(nullptr, ((lang == Language::kKorean ? L"결과 파일 생성 실패:\n" : L"Failed to write output files:\n") + err).c_str(), L"SkyrimDiagDumpTool", MB_ICONERROR);
    LocalFree(argv);
    return 4;
  }

  if (headless) {
    LocalFree(argv);
    return 0;
  }

  const int rc = RunGuiViewer(hInst, guiOpt, analyzeOpt, std::move(r), &err);
  LocalFree(argv);
  return rc;
}
