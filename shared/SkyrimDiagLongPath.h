#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace skydiag {

// Win32 file calls reject a path of MAX_PATH (260) characters or more, and a
// directory of 248 or more, unless the process opts into long paths, which
// these EXEs and DLLs do not. The analyzer's copies under
// <output>\.skydiag-analysis\<sha256>\<size>.<mtime>\ add about 160
// characters with their temporary names, so an output folder of about 100
// characters (a Steam install's Data\SKSE\Plugins\Tullius Ctd Logs) is at the
// edge already. The extended-length form ("\\?\C:\...",
// "\\?\UNC\server\share\...") has no such limit, but it also skips Win32's
// path clean-up, so the path is made absolute first:
// std::filesystem::absolute calls GetFullPathNameW, the same clean-up the
// plain calls apply ("/" to "\", "." and "..", trailing dots and spaces). An
// empty path, a device path or one that cannot be made absolute comes back
// as is, as does every path off Windows.
inline std::filesystem::path ExtendedLengthPath(const std::filesystem::path& path)
{
#ifdef _WIN32
  if (path.empty()) {
    return path;
  }
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  if (ec) {
    return path;
  }
  const std::wstring& text = absolute.native();
  if (text.starts_with(LR"(\\?\)") || text.starts_with(LR"(\\.\)")) {
    return absolute;
  }
  if (text.starts_with(LR"(\\)")) {
    return std::filesystem::path(LR"(\\?\UNC\)" + text.substr(2));
  }
  if (text.size() >= 3 && text[1] == L':' && text[2] == L'\\') {
    return std::filesystem::path(LR"(\\?\)" + text);
  }
  return path;
#else
  return path;
#endif
}

// create_directories on the extended-length form of a volume or share root
// ("\\?\D:\", "\\?\UNC\server\share\") fails with access denied where the
// plain "D:\" succeeds, because MSVC asks CreateDirectoryW for the root
// itself; a directory that already exists is what callers want, so that is
// no error. Pass the plain directory: the parent of
// ExtendedLengthPath(file) loses a root's trailing backslash, and "\\?\D:"
// names the volume rather than its root directory.
inline bool CreateDirectoriesExtended(const std::filesystem::path& dir, std::error_code& ec)
{
  const auto ioDir = ExtendedLengthPath(dir);
  std::filesystem::create_directories(ioDir, ec);
  if (ec) {
    std::error_code existsEc;
    if (std::filesystem::is_directory(ioDir, existsEc)) {
      ec.clear();
    }
  }
  return !ec;
}

}  // namespace skydiag
