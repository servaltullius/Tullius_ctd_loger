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

  const std::filesystem::path helperMainPath = repoRoot / "helper" / "src" / "main.cpp";
  if (!std::filesystem::exists(helperMainPath)) {
    std::cerr << "ERROR: helper/src/main.cpp not found at: " << helperMainPath << "\n";
    return 1;
  }
  std::string helperMain;
  if (!ReadAllText(helperMainPath, &helperMain)) {
    std::cerr << "ERROR: failed to read: " << helperMainPath << "\n";
    return 1;
  }
  if (!Contains(helperMain, "SkyrimDiag_Incident_")) {
    std::cerr << "ERROR: Helper output naming must include SkyrimDiag_Incident_\n";
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
