#include <filesystem>
#include <iostream>
#include <string>

#include "SourceGuardTestUtils.h"

static bool Contains(const std::string& haystack, const char* needle)
{
  if (!needle || !*needle) {
    return true;
  }
  return haystack.find(needle) != std::string::npos;
}

int main()
{
  const auto repoRoot = skydiag::tests::source_guard::ProjectRoot();

  const std::filesystem::path helperSrcDir = repoRoot / "helper" / "src";
  if (!std::filesystem::exists(helperSrcDir) || !std::filesystem::is_directory(helperSrcDir)) {
    std::cerr << "ERROR: helper/src not found at: " << helperSrcDir << "\n";
    return 1;
  }
  bool foundIncidentNaming = false;
  std::size_t scannedFiles = 0;
  for (const auto& entry : std::filesystem::directory_iterator(helperSrcDir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto path = entry.path();
    const auto ext = path.extension().string();
    if (ext != ".cpp" && ext != ".h") {
      continue;
    }
    std::string txt;
    if (!skydiag::tests::source_guard::TryReadSplitAwareText(path, &txt)) {
      std::cerr << "ERROR: failed to read: " << path << "\n";
      return 1;
    }
    scannedFiles++;
    if (Contains(txt, "SkyrimDiag_Incident_")) {
      foundIncidentNaming = true;
      break;
    }
  }
  if (!foundIncidentNaming) {
    std::cerr << "ERROR: Helper output naming must include SkyrimDiag_Incident_ (scanned " << scannedFiles << " files)\n";
    return 1;
  }

  const std::filesystem::path outputWriterPath = repoRoot / "dump_tool" / "src" / "OutputWriter.cpp";
  const std::filesystem::path incidentManifestPath = repoRoot / "helper" / "src" / "IncidentManifest.cpp";
  if (!std::filesystem::exists(outputWriterPath)) {
    std::cerr << "ERROR: dump_tool/src/OutputWriter.cpp not found at: " << outputWriterPath << "\n";
    return 1;
  }
  if (!std::filesystem::exists(incidentManifestPath)) {
    std::cerr << "ERROR: helper/src/IncidentManifest.cpp not found at: " << incidentManifestPath << "\n";
    return 1;
  }
  std::string outputWriter;
  if (!skydiag::tests::source_guard::TryReadSplitAwareText(outputWriterPath, &outputWriter)) {
    std::cerr << "ERROR: failed to read: " << outputWriterPath << "\n";
    return 1;
  }
  std::string incidentManifest;
  if (!skydiag::tests::source_guard::TryReadSplitAwareText(incidentManifestPath, &incidentManifest)) {
    std::cerr << "ERROR: failed to read: " << incidentManifestPath << "\n";
    return 1;
  }
  if (!Contains(outputWriter, "incident_id")) {
    std::cerr << "ERROR: Incident manifest schema must include incident_id\n";
    return 1;
  }
  if (!Contains(outputWriter, "capture_kind")) {
    std::cerr << "ERROR: Incident manifest schema must include capture_kind\n";
    return 1;
  }
  if (!Contains(outputWriter, "artifacts")) {
    std::cerr << "ERROR: Incident manifest schema must include artifacts\n";
    return 1;
  }
  if (!Contains(outputWriter, "etw")) {
    std::cerr << "ERROR: Incident manifest schema must include etw\n";
    return 1;
  }
  if (!Contains(outputWriter, "privacy")) {
    std::cerr << "ERROR: Incident manifest schema must include privacy\n";
    return 1;
  }
  if (!Contains(outputWriter, "capture_profile")) {
    std::cerr << "ERROR: Incident manifest schema must include capture_profile\n";
    return 1;
  }
  if (!Contains(outputWriter, "include_full_memory")) {
    std::cerr << "ERROR: Incident manifest schema must include effective profile flags\n";
    return 1;
  }
  if (!Contains(incidentManifest, "recapture_evaluation")) {
    std::cerr << "ERROR: Incident manifest schema must include recapture_evaluation\n";
    return 1;
  }
  if (!Contains(incidentManifest, "target_profile")) {
    std::cerr << "ERROR: Incident manifest schema must include recapture_evaluation.target_profile\n";
    return 1;
  }
  if (!Contains(incidentManifest, "reasons")) {
    std::cerr << "ERROR: Incident manifest schema must include recapture_evaluation.reasons\n";
    return 1;
  }
  if (!Contains(incidentManifest, "triggered")) {
    std::cerr << "ERROR: Incident manifest schema must include recapture_evaluation.triggered\n";
    return 1;
  }
  if (!Contains(incidentManifest, "escalation_level")) {
    std::cerr << "ERROR: Incident manifest schema must include recapture_evaluation.escalation_level\n";
    return 1;
  }

  return 0;
}
