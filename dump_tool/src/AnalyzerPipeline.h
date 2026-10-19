#pragma once

#include "Analyzer.h"
#include "CrashLoggerParseCore.h"
#include "MinidumpUtil.h"
#include "Mo2Index.h"

#include <Windows.h>

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

}  // namespace skydiag::dump_tool
