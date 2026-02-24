# PLUGIN KNOWLEDGE BASE

## OVERVIEW
`plugin/` builds `SkyrimDiag.dll`, the in-game SKSE component for event capture and shared-memory blackbox writes.

## STRUCTURE
```text
plugin/
|- include/SkyrimDiag/  # public plugin headers used by plugin sources
|- src/                 # plugin runtime logic
`- CMakeLists.txt       # target wiring, post-build copy hooks
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Plugin load flow | `plugin/src/PluginMain.cpp` | `SKSEPluginLoad` and bootstrap |
| Crash marking | `plugin/src/CrashHandler.cpp` | crash hook mode behavior |
| Runtime events | `plugin/src/EventSinks.cpp` | event sink registration |
| Shared memory writer | `plugin/src/SharedMemory.cpp` | protocol write path |
| Resource hooks | `plugin/src/ResourceHooks.cpp` | optional resource tracking |
| Build wiring | `plugin/CMakeLists.txt` | CommonLibSSE link + deploy copy |

## CONVENTIONS
- Keep plugin changes low overhead; this code runs inside the game process.
- Respect existing `CrashHookMode` semantics; mode 1 is the expected default.
- Preserve post-build copy behavior controlled by `SKYRIM_FOLDER`/`SKYRIM_MODS_FOLDER`.
- Keep Unicode handling consistent with existing `_UNICODE`/`UNICODE` compile defs.

## COMMANDS
```bash
cmake --build build-win --target SkyrimDiag
```

## ANTI-PATTERNS
- Do not convert this module into crash-prevention logic.
- Do not add heavy/blocking work to plugin load/init paths.
- Do not break shared protocol compatibility with helper/dump tool consumers.
- Do not add DllMain-heavy behavior that risks loader deadlocks.

## NOTES
- Runtime behavior here must stay compatible with helper-side capture assumptions.
- If plugin protocol fields change, update shared headers and corresponding tests together.
