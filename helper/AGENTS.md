# HELPER KNOWLEDGE BASE

## OVERVIEW
`helper/` builds `SkyrimDiagHelper.exe`, the out-of-proc monitor that attaches to the game, captures dumps, and triggers analysis.

## STRUCTURE
```text
helper/
|- include/SkyrimDiagHelper/  # helper-facing public headers
|- src/                       # capture, attach, retention, and launcher logic
`- CMakeLists.txt             # helper target + system deps
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Process entry | `helper/src/main.cpp` | helper lifecycle and loop |
| Crash dump write | `helper/src/DumpWriter.cpp` | dump file creation |
| Crash pipeline | `helper/src/CrashCapture.cpp` | crash capture orchestration |
| Hang pipeline | `helper/src/HangCapture.cpp` | hang/ILS dump flow |
| Auto analysis | `helper/src/DumpToolLaunch.cpp` | invokes CLI analyzer |
| Retention policy | `helper/src/RetentionWorker.cpp` | cleanup limits and policy |
| Settings | `helper/src/Config.cpp` | INI and runtime flags |
| Build wiring | `helper/CMakeLists.txt` | `nlohmann_json` + Win32 link set |

## CONVENTIONS
- Keep dump-first order: write dump before filtering/post-processing.
- Keep user-facing outputs local; no telemetry/upload behavior.
- Keep helper robust under missing optional artifacts (degrade gracefully).
- Keep `.ini` compatibility; new keys should preserve old defaults when absent.

## COMMANDS
```bash
cmake --build build-win --target SkyrimDiagHelper
```

## ANTI-PATTERNS
- Do not move `WriteDumpWithStreams()` behind post-processing.
- Do not remove manifest/retention guards without matching tests.
- Do not introduce lexicographic semantic-version comparisons.
- Do not break pending-crash analysis and preflight output contracts.

## NOTES
- Helper is the bridge between in-game plugin data and post-crash analysis.
- Any capture policy change must be reflected in `tests/*guard_tests.cpp`.
