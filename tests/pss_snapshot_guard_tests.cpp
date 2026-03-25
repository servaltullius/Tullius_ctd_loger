#include "SourceGuardTestUtils.h"

#include <filesystem>
#include <cctype>
#include <string_view>

using skydiag::tests::source_guard::AssertContains;
using skydiag::tests::source_guard::ReadAllText;

namespace {

bool IsTokenChar(char c)
{
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool ContainsToken(const std::string& source, std::string_view token)
{
  std::size_t pos = source.find(token);
  while (pos != std::string::npos) {
    const bool startsOnBoundary = pos == 0 || !IsTokenChar(source[pos - 1]);
    const std::size_t end = pos + token.size();
    const bool endsOnBoundary = end >= source.size() || !IsTokenChar(source[end]);
    if (startsOnBoundary && endsOnBoundary) {
      return true;
    }
    pos = source.find(token, pos + 1);
  }

  return false;
}

void AssertContainsToken(const std::string& source, std::string_view token, const char* message)
{
  assert(ContainsToken(source, token) && message);
}

std::string_view ExtractFlagBlock(const std::string& source)
{
  const std::string_view signature = "constexpr PSS_CAPTURE_FLAGS kFreezeSnapshotFlags";
  const std::size_t start = source.find(signature);
  assert(start != std::string::npos && "kFreezeSnapshotFlags definition not found");

  const std::size_t end = source.find(");", start);
  assert(end != std::string::npos && "kFreezeSnapshotFlags definition terminator not found");

  return std::string_view(source).substr(start, end + 2 - start);
}

}  // namespace

int main()
{
  const std::filesystem::path repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto pssSnapshotPath = repoRoot / "helper" / "src" / "PssSnapshot.cpp";

  assert(std::filesystem::exists(pssSnapshotPath) && "helper/src/PssSnapshot.cpp not found");

  const auto pssSnapshot = ReadAllText(pssSnapshotPath);
  const auto freezeFlagsBlock = ExtractFlagBlock(pssSnapshot);

  AssertContainsToken(std::string(freezeFlagsBlock), "PSS_CAPTURE_VA_CLONE", "Freeze PSS capture must include VA clone.");
  AssertContainsToken(std::string(freezeFlagsBlock), "PSS_CAPTURE_VA_SPACE", "Freeze PSS capture must include VA space capture.");
  AssertContainsToken(std::string(freezeFlagsBlock), "PSS_CAPTURE_VA_SPACE_SECTION_INFORMATION", "Freeze PSS capture must include VA space section information.");
  AssertContainsToken(std::string(freezeFlagsBlock), "PSS_CAPTURE_THREADS", "Freeze PSS capture must include thread capture.");
  AssertContainsToken(std::string(freezeFlagsBlock), "PSS_CAPTURE_THREAD_CONTEXT", "Freeze PSS capture must include thread context capture.");

  return 0;
}
