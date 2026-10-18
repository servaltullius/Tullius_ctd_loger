#pragma once

#include "Bucket.h"
#include "CrashHistory.h"
#include "DumpIdentity.h"
#include "GraphicsInjectionDiag.h"
#include "PluginRules.h"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "I18nCore.h"
#include "SignatureDatabase.h"

namespace skydiag::dump_tool {

struct EvidenceItem
{
  i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
  std::wstring confidence;  // "높음/중간/낮음"
  std::wstring title;
  std::wstring details;
};

// ADR-0010: an object that a crash register or the top of the crash stack
// points at, read from the memory the helper added to the dump.
struct DumpObject
{
  std::wstring location;   // "RBX", "RSP+68"
  std::uint64_t address = 0;
  std::wstring type_name;  // "Character", "RE::Foo"
  std::wstring module;     // module holding the RTTI (SkyrimSE.exe or a plugin DLL)
  bool is_form = false;
  std::uint32_t form_id = 0;
  std::uint8_t form_type = 0;
  std::vector<std::wstring> source_files;  // plugin files, in load order
  std::wstring name;  // TESFullName; a reference takes its base object's
  // For a reference: the base object it places (an NPC for a Character).
  bool has_base_object = false;
  std::wstring base_type_name;  // "TESNPC"
  std::uint32_t base_form_id = 0;
  std::vector<std::wstring> base_source_files;
  std::uint32_t relevance = 0;  // Crash Logger's location + type weights
};

struct SuspectItem
{
  i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
  std::wstring confidence;  // "높음/중간/낮음"
  std::wstring module_filename;
  std::wstring module_path;
  std::wstring inferred_mod_name;  // best-effort (MO2 mods\<modname>\...)
  std::uint32_t score = 0;  // weighted callstack/pointer score (heuristic; not a hit count)
  std::wstring reason;
};

struct ActionableCandidate
{
  i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
  std::wstring confidence;
  std::string status_id;  // cross_validated / related / reference_clue / conflicting
  std::wstring display_name;
  std::wstring primary_identifier;
  std::wstring secondary_label;
  std::wstring plugin_name;
  std::wstring mod_name;
  std::wstring module_filename;
  std::wstring explanation;
  std::vector<std::string> supporting_families;
  std::vector<std::string> conflicting_families;
  std::uint32_t score = 0;
  std::uint32_t family_count = 0;
  bool cross_validated = false;
  bool has_conflict = false;
};

struct FreezeRelatedCandidate
{
  i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
  std::wstring confidence;
  std::wstring display_name;
};

struct BlackboxFreezeSummary
{
  bool has_context = false;
  bool loading_window = false;
  std::uint32_t recent_module_loads = 0;
  std::uint32_t recent_module_unloads = 0;
  std::uint32_t recent_thread_creates = 0;
  std::uint32_t recent_thread_exits = 0;
  std::uint32_t module_churn_score = 0;
  std::uint32_t thread_churn_score = 0;
  std::vector<std::wstring> recent_non_system_modules;

  // Game state at the capture (ADR-0009). Independent of has_context, which
  // only covers module/thread churn.
  std::vector<std::wstring> open_menus;     // game menus open at the last event, oldest first
  double seconds_since_load_end = -1.0;     // -1 when no load finished in the recorded window
  double pause_gap_seconds = 0.0;           // longest main-thread gap of 5+ minutes (PC asleep / minimized)
  double pause_gap_ended_seconds_before = 0.0;
};

struct FirstChanceSummary
{
  bool has_context = false;
  std::uint32_t recent_count = 0;
  std::uint32_t unique_signature_count = 0;
  std::uint32_t loading_window_count = 0;
  std::uint32_t repeated_signature_count = 0;
  std::vector<std::wstring> recent_non_system_modules;
};

struct HangThreadModuleConsensus
{
  bool has_consensus = false;
  std::uint32_t main_thread_id = 0;
  std::wstring module_filename;
  std::uint32_t matching_thread_count = 0;
  std::uint32_t stable_thread_count = 0;
  bool os_lock_cycle_proven = false;
};

// ADR-0016: a crash on an int3 in a hook stub that the dump shows complete,
// so the stub was still being written when another thread ran the hooked
// call (HookStubRace.h).
struct HookStubRaceInfo
{
  bool detected = false;
  std::uint64_t stub_address = 0;  // the exception address
  std::uint64_t target = 0;        // where the stub jumps now
  std::wstring target_module;      // the plugin whose hook it is
  std::wstring target_module_path;
  std::wstring target_mod_name;    // MO2 mod name, when known
  std::uint64_t target_offset = 0;
  // The patched game call that entered the stub ("SkyrimSE.exe+0xcd8fbb"),
  // when the return address on the stack shows it; empty otherwise.
  std::wstring call_site;
};

// The game main thread parked in a Win32 modal loop (MessageBox / DialogBox /
// TaskDialog). Its stack then only shows bystander modules, so plugin
// attribution must come from the dialog itself, never from stack density.
struct ModalDialogWaitInfo
{
  bool detected = false;
  bool window_evidence = false;  // helper saw a visible dialog owned by the main thread
  bool stack_evidence = false;   // main-thread stack is a pure modal-wait chain
  std::uint32_t main_thread_id = 0;
  std::wstring wait_api;         // e.g. "user32.dll!MessageBoxW"
  std::wstring dialog_title;
  std::wstring dialog_text;
  std::wstring caller_module_filename;
  std::wstring caller_module_path;
  std::wstring caller_inferred_mod_name;
  // plugin / skse_runtime / hook_framework / game_exe / none
  std::string caller_kind = "none";
  // plugin_incompatible / address_library_missing / empty (from the dialog text)
  std::string address_library_issue;
  std::uint32_t other_thread_dialog_count = 0;
};

// What the game main thread was doing when a freeze was captured, read from the
// top of its stack (ADR-0009).
struct MainThreadWaitInfo
{
  // engine_wait: a sleep/wait called by the game executable
  // plugin_wait: a sleep/wait called by a plugin DLL
  // graphics_driver_wait: a wait inside the graphics driver or D3D/DXGI
  // running: code was executing (no wait API on top)
  // unknown: the top frames could not be told apart; empty: not analyzed
  std::string kind;
  std::string wait_class;           // sleep / sync / empty
  // engine_wait only: gpu_query_poll when the engine was polling a Direct3D
  // query (ID3D11DeviceContext::GetData) in its Sleep loop, i.e. waiting for
  // the GPU; empty when the code is not known.
  std::string engine_wait_detail;
  std::wstring wait_api;            // e.g. "KERNELBASE.dll!SleepEx"
  std::wstring waiting_module;      // module that called the wait, or that was running
  std::wstring waiting_mod_name;
  std::vector<std::wstring> path_modules;  // plugin DLLs further down the stack, outermost last

  // engine_wait on a sync object only: a thread the game itself started (its
  // start address is in the game executable, e.g. a BSJobs job thread that
  // runs SKSE tasks) that was inside plugin code while the main thread waited.
  // The main thread waits for such threads every frame, so a plugin stuck on
  // one of them stops the game (seen with the crash tester, 2026-10-12).
  std::uint32_t blocked_thread_tid = 0;
  std::wstring blocked_module;
  std::wstring blocked_module_path;
  std::wstring blocked_mod_name;
  // waiting_in_plugin: the plugin called a sleep/wait
  // engine_call_from_plugin: engine code the plugin called was on top
  // running_in_plugin: plugin code was on top
  std::string blocked_state;
  std::wstring blocked_top;  // top frame, e.g. "KERNELBASE.dll!SleepEx"
  std::uint32_t blocked_thread_count = 0;  // game threads found in plugin code
};

struct FreezeAnalysisResult
{
  // state ids: modal_dialog_wait / deadlock_likely / synchronization_stall_likely /
  // loader_stall_likely / freeze_candidate / freeze_ambiguous
  bool has_analysis = false;
  i18n::ConfidenceLevel confidence_level = i18n::ConfidenceLevel::kUnknown;
  std::wstring confidence;
  std::string state_id;
  std::string support_quality;
  std::vector<std::wstring> primary_reasons;
  std::vector<FreezeRelatedCandidate> related_candidates;
  BlackboxFreezeSummary blackbox_context;
  FirstChanceSummary first_chance_context;
  HangThreadModuleConsensus thread_module_consensus;
};

struct EventRow
{
  std::uint32_t i = 0;
  double t_ms = 0.0;
  std::uint32_t tid = 0;
  std::uint16_t type = 0;
  std::wstring type_name;
  std::uint64_t a = 0;
  std::uint64_t b = 0;
  std::uint64_t c = 0;
  std::uint64_t d = 0;
  std::wstring detail;  // human-readable summary (e.g. "hitch=105.8s flags=Loading interval=100ms")
};

struct ResourceRow
{
  double t_ms = 0.0;
  std::uint32_t tid = 0;
  std::wstring path;  // e.g. "meshes\\foo\\bar.nif" (normalized best-effort)
  std::wstring kind;  // e.g. "nif/hkx/tri"
  std::vector<std::wstring> providers;  // MO2 providers (mods/overwrite) best-effort
  bool is_conflict = false;  // providers.size() >= 2
};

struct BucketCorrelation
{
  std::size_t count = 0;
  std::string first_seen;
  std::string last_seen;
};

struct BucketCandidateRepeat
{
  std::string candidate_key;
  std::size_t prior_count = 0;
};

// A loaded DLL that makes crash evidence unreliable (ProblematicModules.h).
struct ProblematicModule
{
  std::wstring filename;
  std::wstring name;
  std::wstring mod_name;  // MO2 mod folder, when the path shows one
};

// Memory at capture time. RAM and commit come from the dump's own
// SystemMemoryInfo and ProcessVmCounters streams (Windows writes them); VRAM
// comes from the helper's GPU memory stream.
struct MemorySnapshot
{
  bool has_system = false;
  std::uint64_t ram_total_bytes = 0;
  std::uint64_t ram_available_bytes = 0;
  std::uint64_t commit_used_bytes = 0;
  std::uint64_t commit_limit_bytes = 0;
  // Taken from a PSS snapshot, whose clone counts toward system commit while
  // the dump is written; no commit verdict then.
  bool commit_includes_snapshot_clone = false;

  bool has_process = false;
  std::uint64_t process_private_bytes = 0;  // the game's commit charge
  std::uint64_t process_working_set_bytes = 0;
  std::uint64_t process_peak_working_set_bytes = 0;

  // The adapter the game used most. Usage and budget are DXGI's
  // QueryVideoMemoryInfo numbers for the adapter's own memory, the same ones
  // Crash Logger prints as "GPU MEMORY".
  bool has_gpu = false;
  std::wstring gpu_name;
  std::uint64_t vram_usage_bytes = 0;
  std::uint64_t vram_budget_bytes = 0;
  std::uint64_t vram_dedicated_bytes = 0;
  std::uint64_t gpu_shared_usage_bytes = 0;
};

struct AnalysisResult
{
  i18n::Language language = i18n::DefaultLanguage();
  std::wstring dump_path;
  std::wstring out_dir;
  DumpIdentity dump_identity;

  std::uint32_t pid = 0;
  std::uint32_t state_flags = 0;
  std::uint32_t blackbox_crash_seq = 0;
  std::uint32_t blackbox_exception_code = 0;
  std::uint32_t blackbox_faulting_tid = 0;
  std::uint64_t blackbox_exception_addr = 0;
  bool is_filtered_clean_exit = false;
  std::string clean_exit_dump_state;
  // Helper filter context: "process_exit" (exit code 0) or "late_recovery"
  // (the game kept running after the crash was captured).
  std::string clean_exit_filter_context;
  std::wstring clean_exit_evidence_filename;

  std::uint32_t exc_code = 0;
  std::uint32_t exc_tid = 0;
  std::uint64_t exc_addr = 0;
  std::uint64_t fault_module_offset = 0;  // offset in fault module (if resolved)
  std::vector<std::uint64_t> exc_info;  // MINIDUMP_EXCEPTION.ExceptionInformation (best-effort)
  std::wstring crash_bucket_key;  // stable key for repeated CTD grouping (best-effort)
  std::uint32_t crash_bucket_version = 3;
  std::string game_version;  // best-effort game executable version (e.g. "1.5.97.0")

  std::wstring fault_module_path;      // full path if available
  std::wstring fault_module_filename;  // basename
  std::wstring fault_module_plus_offset;
  std::wstring inferred_mod_name;  // best-effort (MO2 mods\<modname>\...)
  // The fault module is a GPU vendor tool or overlay ("NVIDIA", ...), not a
  // mod (minidump::ExternalSoftwareName).
  std::wstring fault_module_external_software;

  // Optional: Crash Logger SSE/AE integration (best-effort)
  std::wstring crash_logger_log_path;
  std::wstring crash_logger_version;
  std::uint64_t crash_logger_pairing_time_delta_ms = 0;
  std::uint64_t crash_logger_pairing_runner_up_time_delta_ms = 0;
  std::uint32_t crash_logger_pairing_eligible_candidate_count = 0;
  std::uint32_t crash_logger_pairing_nearby_competitor_count = 0;
  std::string crash_logger_pairing_selected_kind;
  std::string crash_logger_pairing_confidence;
  bool crash_logger_pairing_same_directory = false;
  // True for nearby competing logs, and for a log whose exception location
  // differs from the dump's (crash_logger_location_mismatch).
  bool crash_logger_pairing_ambiguous = false;
  bool crash_logger_location_mismatch = false;
  std::vector<std::wstring> crash_logger_top_modules;  // e.g. "hdtSMP64.dll", "MuJointFix.dll"
  std::wstring crash_logger_direct_fault_module;
  std::wstring crash_logger_first_actionable_probable_module;
  std::wstring crash_logger_probable_streak_module;
  std::uint32_t crash_logger_probable_streak_length = 0;
  bool crash_logger_direct_fault_eligible = false;
  bool crash_logger_first_actionable_probable_eligible = false;
  bool crash_logger_probable_streak_eligible = false;
  std::uint32_t crash_logger_frame_signal_strength = 0;
  // Plugins whose frames only wrap the entry of the game thread that crashed
  // (ThreadEntryHooks.h), from Crash Logger's call stack and the stack walk.
  // They are left out of the suspects and of Crash Logger's frame signals.
  std::vector<std::wstring> thread_entry_hook_modules;
  // The shown thread's walk reached its start and had plugin frames, all of
  // them thread-entry hooks (no stack scan then; see Analyzer.CaptureInputs).
  bool stackwalk_only_thread_entry_hook_plugins = false;
  std::wstring crash_logger_cpp_exception_type;
  std::wstring crash_logger_cpp_exception_info;
  std::wstring crash_logger_cpp_exception_throw_location;
  std::wstring crash_logger_cpp_exception_module;
  // Object types Crash Logger names near the crash (crash_signatures.json
  // object_type_any), including ones with no plugin such as InstalledContent.
  std::vector<std::wstring> crash_logger_object_types;

  // CrashLogger: referenced game objects (ESP/ESM) at crash time
  struct CrashLoggerModReference {
    std::wstring esp_name;
    std::wstring best_object_type;
    std::wstring best_location;
    std::wstring object_name;
    std::wstring form_id;
    std::uint32_t ref_count = 0;
    std::uint32_t relevance_score = 0;
  };
  std::vector<CrashLoggerModReference> crash_logger_object_refs;
  // Crash Logger's own exception line: the original exception when the dump
  // holds a secondary one (for example from Crash Logger's memory probes).
  std::wstring crash_logger_exception_type;      // "EXCEPTION_BREAKPOINT"
  std::wstring crash_logger_exception_location;  // "ucrtbase.dll+00C2B24"
  std::wstring crash_logger_exception_instruction;
  // Relevant objects with a name or FormID but no plugin (spawned at runtime);
  // esp_name is empty.
  std::vector<CrashLoggerModReference> crash_logger_unattributed_objects;
  // ADR-0010: objects read from the crash dump itself (crashes only).
  std::vector<DumpObject> dump_objects;
  MemorySnapshot memory;
  std::vector<ProblematicModule> problematic_modules;

  // Heuristic: suspects inferred from stack/module scanning
  std::vector<SuspectItem> suspects;
  bool suspects_from_stackwalk = false;
  std::optional<SignatureMatch> signature_match;
  std::unordered_map<std::uint64_t, std::string> resolved_functions;
  std::vector<ModuleStats> history_stats;
  BucketCorrelation history_correlation;
  std::vector<BucketCandidateRepeat> bucket_candidate_repeats;
  GraphicsEnvironment graphics_env;
  std::optional<GraphicsDiagResult> graphics_diag;
  bool has_plugin_scan = false;
  std::string plugin_scan_json_utf8;
  std::vector<std::wstring> missing_masters;
  bool needs_bees = false;
  std::vector<PluginRuleDiagnosis> plugin_diagnostics;
  // skse64.log of the captured session (environment context, never a cause candidate)
  SkseLogScanInfo skse_log;

  // Best-effort callstack (primary thread: crash thread, WCT cycle thread, or inferred main thread)
  std::uint32_t stackwalk_primary_tid = 0;
  std::vector<std::wstring> stackwalk_primary_frames;
  std::vector<CrashBucketFrame> stackwalk_primary_bucket_frames;
  std::uint32_t stackwalk_total_frames = 0;
  std::uint32_t stackwalk_symbolized_frames = 0;
  std::uint32_t stackwalk_source_line_frames = 0;
  // The shown thread's walk stopped before the thread start ("" when it did
  // not): "outside_modules", "non_code_address" or "no_unwind_info".
  std::string stackwalk_truncation;
  std::uint32_t stackwalk_truncation_depth = 0;     // frames walked before the cut
  std::uint64_t stackwalk_truncation_address = 0;   // the rejected caller, or the last frame
  std::wstring stackwalk_truncation_module;         // no_unwind_info: the frame's module
  std::wstring stackwalk_truncation_unloaded_module;  // the cut address lies in this unloaded DLL
  std::wstring fault_unloaded_module;  // the exception address lies in this unloaded DLL
  HookStubRaceInfo hook_stub_race;
  std::wstring symbol_search_path;  // effective DbgHelp search path (best-effort)
  std::wstring symbol_cache_path;   // inferred/selected local cache path (best-effort)
  std::wstring dbghelp_path;        // effective dbghelp runtime path (best-effort)
  std::wstring dbghelp_version;     // dbghelp file version (best-effort)
  std::wstring msdia_path;          // effective msdia140 runtime path (best-effort)
  bool msdia_available = false;
  bool symbol_cache_ready = false;
  bool symbol_runtime_degraded = false;
  bool incident_capture_profile_present = false;
  std::string incident_capture_kind;               // crash / hang / manual / crash_recapture
  std::string incident_capture_profile_base_mode;  // mini / default / full
  bool incident_capture_profile_code_segments = false;
  bool incident_capture_profile_process_thread_data = false;
  bool incident_capture_profile_full_memory_info = false;
  bool incident_capture_profile_module_headers = false;
  bool incident_capture_profile_indirect_memory = false;
  bool incident_capture_profile_ignore_inaccessible_memory = false;
  bool incident_capture_profile_full_memory = false;
  bool incident_recapture_evaluation_present = false;
  bool incident_recapture_triggered = false;
  std::string incident_recapture_kind;            // crash / freeze
  std::string incident_recapture_target_profile;  // none / crash_richer / crash_full / freeze_snapshot_richer
  std::uint32_t incident_recapture_escalation_level = 0;
  std::vector<std::string> incident_recapture_reasons;
  // ADR-0017: another fatal exception was still being checked when this crash
  // was published; the helper kept its dump (incident manifest
  // context.earlier_fault). The module is resolved with this dump's modules.
  bool earlier_fault_present = false;
  std::wstring earlier_fault_dump;
  std::uint32_t earlier_fault_code = 0;
  std::uint64_t earlier_fault_addr = 0;
  std::uint32_t earlier_fault_tid = 0;
  double earlier_fault_seconds_before = 0.0;
  std::wstring earlier_fault_module_plus_offset;
  bool online_symbol_source_allowed = false;
  bool online_symbol_source_used = false;
  bool path_redaction_applied = true;

  bool has_blackbox = false;
  std::vector<EventRow> events;
  // Last main-thread heartbeat on the event clock (-1 = unknown). In a freeze
  // this is when the main thread stopped.
  double blackbox_last_heartbeat_ms = -1.0;

  // Optional: recent resource loads (best-effort; nif/hkx/tri)
  std::vector<ResourceRow> resources;
  // Where the MO2 providers in `resources` come from: "crash_time" (profile
  // and mod list captured while the game ran), "analysis_time" (read from disk
  // at analysis, may not match the crash), or empty (no MO2 index).
  std::string mo2_providers_source;
  std::wstring mo2_profile_name;

  // Cross-signal actionable candidates (best-effort; mainly for EXE/system victim-like crashes)
  std::vector<ActionableCandidate> actionable_candidates;
  BlackboxFreezeSummary blackbox_freeze_summary;
  FirstChanceSummary first_chance_summary;
  HangThreadModuleConsensus hang_thread_module_consensus;
  ModalDialogWaitInfo modal_dialog_wait;
  MainThreadWaitInfo main_thread_wait;
  // The suspects came from the game main thread's stack (a freeze's stack walk
  // can pick a WCT cycle thread instead). A bystander wait only speaks for
  // the main thread's own stack.
  bool suspects_from_main_thread = false;
  FreezeAnalysisResult freeze_analysis;

  bool has_wct = false;
  std::string wct_json_utf8;

  bool is_crash_like = false;
  bool is_hang_like = false;
  bool is_snapshot_like = false;
  bool is_manual_capture = false;

  std::wstring summary_sentence;
  std::vector<EvidenceItem> evidence;
  std::vector<std::wstring> recommendations;
  std::vector<std::wstring> troubleshooting_steps;
  std::wstring troubleshooting_title;

  // Diagnostic messages from best-effort subsystems (data loading, CrashLogger integration, etc.)
  std::vector<std::wstring> diagnostics;
};

// The dump holds a C++ exception (0xE06D7363) that Crash Logger also logged as
// its unhandled exception: a real crash, not a caught throw. With the default
// CrashHookMode=1 the plugin records a C++ exception only in that case. Not
// when the log names another exception location (another crash's log).
inline bool IsCrashLoggerConfirmedCppException(const AnalysisResult& r)
{
  if (r.exc_code != 0xE06D7363u || r.crash_logger_location_mismatch) {
    return false;
  }
  std::wstring type = r.crash_logger_exception_type;
  for (auto& ch : type) {
    ch = static_cast<wchar_t>(ch >= L'A' && ch <= L'Z' ? ch + (L'a' - L'A') : ch);
  }
  return type.find(L"c++ exception") != std::wstring::npos;
}

struct AnalyzeOptions
{
  bool debug = false;
  bool allow_online_symbols = false;
  bool redact_paths = true;
  std::wstring data_dir;  // Optional analyzer data directory (e.g. "<exe>/data")
  std::string game_version;  // Optional override (e.g. "1.6.640.0")
  std::wstring output_dir;   // Optional output base directory for crash history
  bool use_crash_logger = true;  // false: do not pair a Crash Logger log (CLI --no-crash-logger)
  i18n::Language language = i18n::DefaultLanguage();
};

bool AnalyzeDump(const std::wstring& dumpPath, const std::wstring& outDir, const AnalyzeOptions& opt, AnalysisResult& out, std::wstring* err);

}  // namespace skydiag::dump_tool
