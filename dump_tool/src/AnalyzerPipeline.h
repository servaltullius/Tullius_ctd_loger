#pragma once

#include "Analyzer.h"
#include "MinidumpUtil.h"
#include "Mo2Index.h"

#include <Windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace skydiag::dump_tool {

std::optional<CONTEXT> ParseExceptionInfo(
  void* dumpBase,
  std::uint64_t dumpSize,
  AnalysisResult& out);

void ResolveFaultModule(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& allModules,
  AnalysisResult& out);

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

void ComputeSuspects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ModuleInfo>& allModules,
  const std::optional<CONTEXT>& excCtx,
  bool hangLike,
  const AnalyzeOptions& opt,
  AnalysisResult& out);

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
  const std::string& analysisTimestamp,
  AnalysisResult& out);

void AppendCrashHistoryEntry(
  const std::filesystem::path& historyPath,
  const std::wstring& dumpPath,
  const std::string& analysisTimestamp,
  AnalysisResult& out);

}  // namespace skydiag::dump_tool
