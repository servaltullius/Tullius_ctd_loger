# DUMP TOOL KNOWLEDGE BASE

## OVERVIEW
`dump_tool/` contains the analysis core (`SkyrimDiagDumpToolCore`), native WinUI bridge DLL, and headless CLI executable.

## STRUCTURE
```text
dump_tool/
|- src/            # analyzer core, native API, CLI entry
|- data/           # JSON rule/signature/config assets
`- CMakeLists.txt  # core/native/cli targets
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Analyzer core | `dump_tool/src/Analyzer.cpp` | high-level analysis orchestration |
| Internals scoring/stack | `dump_tool/src/AnalyzerInternals*.cpp` | stackwalk/scoring implementation |
| Evidence output | `dump_tool/src/EvidenceBuilder*.cpp` | summary and recommendation shaping |
| CLI entry | `dump_tool/src/DumpToolCliMain.cpp` | headless analysis process |
| WinUI native bridge | `dump_tool/src/NativeApi.cpp` | managed/native boundary |
| Rule databases | `dump_tool/data/*.json` | signatures, hooks, plugin rules |
| Build wiring | `dump_tool/CMakeLists.txt` | target graph and data copy hooks |

## CONVENTIONS
- Keep UTF conversion through `Utf8ToWide`/`WideToUtf8` in `dump_tool/src/Utf.*`.
- Keep schema/field compatibility stable; tests lock JSON contracts.
- Keep CLI and native outputs aligned on evidence structure.
- Keep data file propagation to both CLI and Native outputs via existing post-build copy loop.

## COMMANDS
```bash
cmake --build build-win --target SkyrimDiagDumpToolCli SkyrimDiagDumpToolNative
```

## ANTI-PATTERNS
- Do not replace UTF helpers with ad-hoc string widening.
- Do not change JSON schema fields without updating guard tests.
- Do not weaken signature/plugin-rule validation gates.
- Do not diverge CLI and Native API behavior for the same dump inputs.

## NOTES
- This directory is the analysis truth source for helper auto-analysis and WinUI rendering.
- When rule JSON changes, run schema and logic tests before packaging.
