# PROFILE_GUIDE (vibe-kit)

vibe-kit does not inject profiling code. It can optionally summarize timing logs you keep under `.vibe/reports/` (gitignored).

## Optional: performance.log summary

- Create `.vibe/reports/performance.log` with tab-separated rows:
  - `name<TAB>count<TAB>avg_ms<TAB>max_ms`
  - Example: `load_config\t120\t0.12\t1.20`
- Run: `python3 scripts/vibe.py doctor --profile`
- Output: `.vibe/reports/performance_stats.json`

## vibe-kit integration
- `python3 scripts/vibe.py doctor --profile` (or `--full --profile`) only summarizes existing logs under `.vibe/reports/`.
- No source-code injection is performed.
