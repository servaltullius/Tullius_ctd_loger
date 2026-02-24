# PROJECT KNOWLEDGE BASE

**Generated:** 2026-02-23T22:47:57Z
**Commit:** e0a77d2
**Branch:** main

## OVERVIEW
Tullius/SkyrimDiag is a Skyrim SE/AE diagnostics stack: SKSE plugin + out-of-proc helper + native dump analysis + CLI + WinUI viewer.
Windows is the shipping platform; Linux is used for fast CTest coverage.

## STRUCTURE
```text
./
|- plugin/          # SKSE plugin DLL (runtime event/state capture)
|- helper/          # helper EXE (attach, hang/crash detect, dump capture)
|- dump_tool/       # native analysis core + native DLL + headless CLI
|- dump_tool_winui/ # WinUI viewer shell (.NET)
|- tests/           # assert-based C++ tests + Python script tests
|- scripts/         # Windows build, WinUI build, packaging, release gate
|- docs/            # contributor/user docs, plans, ADRs
|- shared/          # shared protocol headers
`- dist/            # runtime INI + packaged zip outputs
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Plugin runtime behavior | `plugin/src/PluginMain.cpp` | SKSE entry (`SKSEPluginLoad`) |
| Helper dump/hang logic | `helper/src/main.cpp` | process lifecycle entry |
| Dump analysis engine | `dump_tool/src/Analyzer.cpp` | core analyzer internals |
| CLI dump analysis entry | `dump_tool/src/DumpToolCliMain.cpp` | headless analyzer entry |
| WinUI app startup | `dump_tool_winui/App.xaml.cs` | `OnLaunched` entry |
| Test registration | `tests/CMakeLists.txt` | source-of-truth for CTest targets |
| Release packaging | `scripts/package.py` | enforces required artifacts |
| Release hard gate | `scripts/verify_release_gate.sh` | blocks broken upload bundles |

## CODE MAP
LSP C++ index is unavailable in this environment (`clangd` missing), so use file-level map first.

| Unit | Kind | Location | Role |
|------|------|----------|------|
| SkyrimDiag plugin | DLL target | `plugin/CMakeLists.txt` | in-game capture hooks |
| SkyrimDiagHelper | EXE target | `helper/CMakeLists.txt` | out-of-proc monitor/capture |
| DumpToolCore | static library | `dump_tool/CMakeLists.txt` | shared analysis logic |
| DumpToolNative | DLL target | `dump_tool/CMakeLists.txt` | native API for WinUI |
| DumpToolCli | EXE target | `dump_tool/CMakeLists.txt` | headless dump analysis |
| CTest suite | 30+ executables | `tests/CMakeLists.txt` | regression and policy guards |

## CONVENTIONS
- C++ baseline: C++20, MSVC UTF-8 (`/utf-8`), runtime `MultiThreaded...DLL`.
- Keep changes surgical; avoid repo-wide formatting or unrelated refactors.
- Release notes/changelog policy: Korean first (`KO` required, English optional below).
- Package naming policy: `Tullius_ctd_loger_v{version}.zip`.
- Linux validation path exists even for Windows product: CMake + CTest in `build-linux(-test)`.

## ANTI-PATTERNS (THIS PROJECT)
- Do not release before hard gate checks in `scripts/verify_release_gate.sh` pass.
- Do not package without `--no-pdb` for release flow.
- Do not use lexicographic version comparisons for semantic version values.
- Do not replace `Utf8ToWide`/`WideToUtf8` with ad-hoc `std::wstring(begin,end)` conversions.
- Do not move `WriteDumpWithStreams()` behind filtering/post-processing in crash pipeline.
- Do not treat diagnostics output as telemetry upload; this project is local-output-first.
- Do not edit WinUI `bin/` or `obj/` artifacts; edit source files only.

## UNIQUE STYLES
- Test strategy is assert-based standalone executables, not gtest/catch2.
- Many tests are policy/guard tests that lock file formats and safety constraints.
- Docs are split by audience: `README.md`, `docs/DEVELOPMENT.md`, `docs/plans/`, `docs/adr/`.

## COMMANDS
```bash
# Linux fast verification
cmake -S . -B build-linux-test -G Ninja
cmake --build build-linux-test
ctest --test-dir build-linux-test --output-on-failure

# Windows builds
scripts\\build-win.cmd
scripts\\build-winui.cmd

# Package (release default)
python scripts/package.py --build-dir build-win --out dist/Tullius_ctd_loger.zip --no-pdb

# Release hard gate
bash scripts/verify_release_gate.sh
```

## NOTES
- There is no top-level `dump_tool_cli/` directory; CLI target is built from `dump_tool/src/DumpToolCliMain.cpp`.
- WSL working repo and Windows mirror repo are separate paths; sync before release builds.
- If `clangd` is unavailable, rely on CMake target graph + grep/AST grep for symbol discovery.
