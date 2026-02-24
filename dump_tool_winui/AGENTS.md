# WINUI KNOWLEDGE BASE

## OVERVIEW
`dump_tool_winui/` is the WinUI 3 shell (`SkyrimDiagDumpToolWinUI.exe`) for interactive dump review.

## STRUCTURE
```text
dump_tool_winui/
|- App.xaml*                    # app bootstrap/resources
|- MainWindow.xaml*             # primary viewer UI
|- Properties/                  # project metadata
|- SkyrimDiagDumpToolWinUI.csproj
|- bin/                         # generated output (do not edit)
`- obj/                         # intermediate output (do not edit)
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| App startup | `dump_tool_winui/App.xaml.cs` | launch flow and args |
| Main window | `dump_tool_winui/MainWindow.xaml` | UI layout |
| Main window code | `dump_tool_winui/MainWindow.xaml.cs` | behavior and bindings |
| Build config | `dump_tool_winui/SkyrimDiagDumpToolWinUI.csproj` | target framework/runtime |
| Packaging requirement | `scripts/build-winui.cmd` | expects `.exe/.pri/.xbf` artifacts |

## CONVENTIONS
- Treat this as source-only: edit `.xaml`, `.cs`, `.csproj`, `Properties/`.
- Build output is copied to `build-winui/` by `scripts/build-winui.cmd`.
- WinUI runtime artifacts (`.pri`, `.xbf`) are required for packaging gate.
- Keep CLI compatibility flags and language switches behavior stable.

## COMMANDS
```bash
scripts\\build-winui.cmd
```

## ANTI-PATTERNS
- Do not edit `dump_tool_winui/bin/` artifacts.
- Do not edit `dump_tool_winui/obj/` artifacts.
- Do not remove required WinUI assets expected by packaging/release gate.
- Do not commit generated outputs in place of source fixes.

## NOTES
- WinUI shell is packaged under `SKSE/Plugins/SkyrimDiagWinUI/`.
- Missing `.pri` or `.xbf` files causes release-gate failure.
