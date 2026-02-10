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

struct RequiredConfigKey
{
  const char* key;
  const char* iniMessage;
  const char* configMessage;
};

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();

  constexpr RequiredConfigKey kRequiredKeys[] = {
    {
      "EnableEtwCaptureOnCrash",
      "Missing EnableEtwCaptureOnCrash in SkyrimDiagHelper.ini",
      "helper/src/Config.cpp must read EnableEtwCaptureOnCrash",
    },
    {
      "EtwCrashProfile",
      "Missing EtwCrashProfile in SkyrimDiagHelper.ini",
      "helper/src/Config.cpp must read EtwCrashProfile",
    },
    {
      "EtwCrashCaptureSeconds",
      "Missing EtwCrashCaptureSeconds in SkyrimDiagHelper.ini",
      "helper/src/Config.cpp must read EtwCrashCaptureSeconds",
    },
    {
      "EnableIncidentManifest",
      "Missing EnableIncidentManifest in SkyrimDiagHelper.ini",
      "helper/src/Config.cpp must read EnableIncidentManifest",
    },
    {
      "IncidentManifestIncludeConfigSnapshot",
      "Missing IncidentManifestIncludeConfigSnapshot in SkyrimDiagHelper.ini",
      "helper/src/Config.cpp must read IncidentManifestIncludeConfigSnapshot",
    },
  };

  const std::filesystem::path iniPath = repoRoot / "dist" / "SkyrimDiagHelper.ini";
  if (!std::filesystem::exists(iniPath)) {
    std::cerr << "ERROR: dist/SkyrimDiagHelper.ini not found at: " << iniPath << "\n";
    return 1;
  }
  std::string ini;
  if (!ReadAllText(iniPath, &ini)) {
    std::cerr << "ERROR: failed to read: " << iniPath << "\n";
    return 1;
  }
  for (const auto& required : kRequiredKeys) {
    if (!Contains(ini, required.key)) {
      std::cerr << "ERROR: " << required.iniMessage << "\n";
      return 1;
    }
  }

  const std::filesystem::path configCppPath = repoRoot / "helper" / "src" / "Config.cpp";
  if (!std::filesystem::exists(configCppPath)) {
    std::cerr << "ERROR: helper/src/Config.cpp not found at: " << configCppPath << "\n";
    return 1;
  }
  std::string configCpp;
  if (!ReadAllText(configCppPath, &configCpp)) {
    std::cerr << "ERROR: failed to read: " << configCppPath << "\n";
    return 1;
  }
  for (const auto& required : kRequiredKeys) {
    if (!Contains(configCpp, required.key)) {
      std::cerr << "ERROR: " << required.configMessage << "\n";
      return 1;
    }
  }

  return 0;
}
