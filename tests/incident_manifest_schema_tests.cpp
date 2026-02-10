#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static bool ReadAllText(const std::filesystem::path& path, std::string* out)
{
  if (out) {
    out->clear();
  }
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  if (out) {
    *out = ss.str();
  }
  return true;
}

static bool Contains(const std::string& haystack, const char* needle)
{
  if (!needle || !*needle) {
    return true;
  }
  return haystack.find(needle) != std::string::npos;
}

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

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
    if (!ReadAllText(path, &txt)) {
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
  if (!std::filesystem::exists(outputWriterPath)) {
    std::cerr << "ERROR: dump_tool/src/OutputWriter.cpp not found at: " << outputWriterPath << "\n";
    return 1;
  }
  std::string outputWriter;
  if (!ReadAllText(outputWriterPath, &outputWriter)) {
    std::cerr << "ERROR: failed to read: " << outputWriterPath << "\n";
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

  return 0;
}
