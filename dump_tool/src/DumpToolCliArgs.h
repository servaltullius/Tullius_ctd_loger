#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydiag::dump_tool::cli {

struct DumpToolCliArgs
{
  std::wstring dump_path;  // required (positional)
  std::wstring out_dir;    // optional (defaults to dump-derived path)

  // When unset, caller should use its default (e.g. env var or false).
  std::optional<bool> allow_online_symbols;

  bool debug = false;
  std::wstring lang_token;  // e.g. "en", "ko"
};

inline std::wstring DumpToolCliUsage()
{
  return
    L"Usage:\n"
    L"  SkyrimDiagDumpToolCli.exe <dumpPath> [options]\n"
    L"\n"
    L"Options:\n"
    L"  --out-dir <dir>            Output directory (optional)\n"
    L"  --allow-online-symbols     Allow symbol server usage (opt-in)\n"
    L"  --no-online-symbols        Disallow symbol server usage\n"
    L"  --lang <token>             Language token (e.g. en, ko)\n"
    L"  --debug                    Disable path redaction\n"
    L"  --headless                 Accepted for compatibility (ignored)\n"
    L"  --help                     Show this help\n";
}

inline bool ParseDumpToolCliArgs(const std::vector<std::wstring_view>& argv, DumpToolCliArgs* out, std::wstring* err)
{
  if (out) {
    *out = DumpToolCliArgs{};
  }
  if (err) {
    err->clear();
  }
  if (!out) {
    return false;
  }

  const std::size_t n = argv.size();
  std::size_t i = 0;
  if (n > 0) {
    i = 1;  // skip program name for normal argv
  }

  for (; i < n; i++) {
    const std::wstring_view a = argv[i];
    if (a.empty()) {
      continue;
    }

    if (a == L"--help" || a == L"-h" || a == L"/?") {
      if (err) {
        *err = DumpToolCliUsage();
      }
      return false;
    }

    if (a == L"--headless") {
      // Compatibility: helper historically passes this to the WinUI exe.
      continue;
    }

    if (a == L"--debug") {
      out->debug = true;
      continue;
    }

    if (a == L"--allow-online-symbols") {
      out->allow_online_symbols = true;
      continue;
    }

    if (a == L"--no-online-symbols") {
      out->allow_online_symbols = false;
      continue;
    }

    if (a == L"--out-dir") {
      if (i + 1 >= n) {
        if (err) {
          *err = L"--out-dir requires a value";
        }
        return false;
      }
      out->out_dir = std::wstring(argv[++i]);
      continue;
    }

    if (a == L"--lang") {
      if (i + 1 >= n) {
        if (err) {
          *err = L"--lang requires a value";
        }
        return false;
      }
      out->lang_token = std::wstring(argv[++i]);
      continue;
    }

    if (a.size() >= 1 && (a[0] == L'-' || a[0] == L'/')) {
      if (err) {
        *err = std::wstring(L"Unknown option: ") + std::wstring(a);
      }
      return false;
    }

    // Positional: dump path
    if (!out->dump_path.empty()) {
      if (err) {
        *err = L"Too many positional arguments (expected only <dumpPath>)";
      }
      return false;
    }
    out->dump_path = std::wstring(a);
  }

  if (out->dump_path.empty()) {
    if (err) {
      *err = L"Missing required positional argument: <dumpPath>";
    }
    return false;
  }

  return true;
}

}  // namespace skydiag::dump_tool::cli

