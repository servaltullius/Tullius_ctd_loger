# PSS Snapshot Evaluation

**Date:** 2026-03-13

## Scope

This spike prototypes a `PssCaptureSnapshot` export path for `hang` and `manual` captures only.

- Feature flag: `EnablePssSnapshotForFreeze=0`
- Default: off
- Fallback: if snapshot capture is unavailable or fails, helper falls back to the existing live-process dump path

Crash capture is intentionally excluded from this spike.

## What Changed

- Added an opt-in helper config flag for freeze captures.
- Added a shared PSS snapshot helper that:
  - resolves `PssCaptureSnapshot` / `PssFreeSnapshot` at runtime
  - captures a freeze snapshot when enabled
  - reports duration and fallback status
- Updated `DumpWriter` so `MiniDumpWriteDump` can export from a process snapshot via `IsProcessSnapshotCallback`.
- Updated `HangCapture` and `ManualCapture` to:
  - attempt PSS snapshot capture first when enabled
  - record snapshot usage/fallback status in WCT capture metadata and incident context
  - fall back to the current live-process dump path on failure

## Evaluation Criteria

The spike is intended to answer three questions in real Windows gameplay testing:

1. Dump success rate
2. WCT consistency versus the existing live-process path
3. User-perceived overhead during hang/manual capture

The helper now records enough metadata to compare:

- `pss_snapshot_requested`
- `pss_snapshot_used`
- `pss_snapshot_capture_ms`
- `pss_snapshot_status`
- `dump_transport`

## Current Decision

**Keep PSS snapshot opt-in only for now. Do not promote it to the default freeze path yet.**

Reason:

- The implementation path is now available and guarded by a feature flag.
- We have not yet collected enough real-world Skyrim freeze/manual captures to justify a silent default swap.
- Freeze capture is timing-sensitive, so default promotion should wait for field validation.

## Promotion Criteria

PSS can be promoted beyond opt-in only if Windows field testing shows:

- no meaningful drop in dump success rate
- materially better freeze-state stability or thread/WCT consistency
- no obvious user-facing overhead regression

If those criteria are not met, keep it opt-in or remove it.
