#pragma once

#include "Analyzer.h"
#include "CrashLoggerParseCore.h"
#include "MinidumpUtil.h"
#include "Mo2Index.h"
#include "Utf.h"

#include <Windows.h>

#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace skydiag::dump_tool {

class AddressResolver;

std::optional<CONTEXT> ParseExceptionInfo(
  void* dumpBase,
  std::uint64_t dumpSize,
  AnalysisResult& out);

void ResolveFaultModule(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& allModules,
  AnalysisResult& out);

// The helper's plugin scan (dump stream, or the _PluginScan.json sidecar).
void LoadPluginScanJson(
  const std::wstring& dumpPath,
  void* dumpBase,
  std::uint64_t dumpSize,
  AnalysisResult& out);

// From the MO2 state captured with the dump when present, else from the
// current disk (marked as analysis-time in `out`).
std::optional<Mo2Index> BuildMo2IndexForDump(const std::vector<std::wstring>& modulePaths, AnalysisResult& out);

void ParseBlackboxStream(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::optional<Mo2Index>& mo2Index,
  const std::vector<std::wstring>& modulePaths,
  AnalysisResult& out);

void IntegratePluginScan(
  const std::wstring& dumpPath,
  const std::vector<minidump::ModuleInfo>& allModules,
  void* dumpBase,
  std::uint64_t dumpSize,
  const AnalyzeOptions& opt,
  AnalysisResult& out);

bool DetermineHangLike(
  bool nameHang,
  const AnalysisResult& out);

void IntegrateCrashLoggerLog(
  const std::wstring& dumpPath,
  const std::vector<minidump::ModuleInfo>& allModules,
  const std::vector<std::wstring>& modulePaths,
  const std::optional<Mo2Index>& mo2Index,
  AnalysisResult& out);

// Tells a Crash Logger frame in a mod named like a Windows DLL (an ENB or
// ReShade d3d11.dll/dxgi.dll outside Windows, review H4) from one in the
// Windows DLL, by the frame address in the dump's modules. Keeps a reference
// to allModules.
crashlogger_core::CrashLoggerSystemNamedModFn CrashLoggerSystemNamedModTest(
  const std::vector<minidump::ModuleInfo>& allModules);

// After a pointer-scan fallback every suspect is Low, except the plugin of a
// blocked game thread, which comes from the walk of that thread.
void CapPointerScanSuspectConfidence(AnalysisResult& out);

// Direct-fault, first-actionable and streak modules of the paired Crash
// Logger log, and whether each may promote a suspect.
void IntegrateCrashLoggerFrameSignals(
  const std::vector<minidump::ModuleInfo>& allModules,
  AnalysisResult* out);

void ComputeSuspects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& allModules,
  const std::optional<CONTEXT>& excCtx,
  bool hangLike,
  const AnalyzeOptions& opt,
  AnalysisResult& out,
  const AddressResolver* gameFunctions = nullptr);

std::filesystem::path ResolveCrashHistoryPath(
  const std::wstring& dumpPath,
  const std::wstring& outDir,
  const AnalyzeOptions& opt);

void LoadIncidentCaptureProfile(
  const std::wstring& dumpPath,
  const std::wstring& outDir,
  AnalysisResult& out);

std::vector<std::string> CollectHistoryCandidateKeys(const AnalysisResult& out);

void LoadCrashHistoryContext(
  const std::filesystem::path& historyPath,
  const std::wstring& dumpPath,
  const std::string& analysisTimestamp,
  AnalysisResult& out);

void AppendCrashHistoryEntry(
  const std::filesystem::path& historyPath,
  const std::wstring& dumpPath,
  const std::string& analysisTimestamp,
  AnalysisResult& out);

// An optional input from outside the dump (the plugin scan and MO2 state,
// the Crash Logger log, the incident manifest, the crash history) that
// cannot be read leaves its part out with a diagnostics line instead of
// failing the whole analysis (exit 3) over one field of another type. A step
// that fails part way leaves the result as it was before it (keeping its
// diagnostics lines): a half-read input, such as a Crash Logger log path
// without its frames, would pass for a whole one.
template <class Step>
void RunOptionalStep(const wchar_t* name, AnalysisResult& out, Step&& step)
{
  AnalysisResult before = out;
  const auto restore = [&](const std::wstring& cause) {
    auto diagnostics = std::move(out.diagnostics);
    out = std::move(before);
    out.diagnostics = std::move(diagnostics);
    out.diagnostics.push_back(std::wstring(L"[Optional input] ") + name + L" skipped" + cause);
  };
  try {
    step();
  } catch (const std::exception& ex) {
    // what() of the standard library is in the ANSI code page on Windows (a
    // path, a localized system message).
    restore(L": " + Utf8OrAnsiToWide(ex.what()));
  } catch (...) {
    restore({});
  }
}

}  // namespace skydiag::dump_tool
