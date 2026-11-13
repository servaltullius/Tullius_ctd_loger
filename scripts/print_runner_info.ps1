# Prints the Windows runner details that intermittent CI failures have depended
# on (the self-dump failure MiniDumpWriteDump 0x800706F8, the stack overflow
# test of the plugin's crash handler), so failing and passing CI runs can be
# compared. Diagnostic only: it never fails the job.
$ErrorActionPreference = 'Continue'

$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
"CPU: $($cpu.Name) (cores=$($cpu.NumberOfCores), logical=$($cpu.NumberOfLogicalProcessors))"

$os = Get-CimInstance Win32_OperatingSystem
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
"OS: $($os.Caption) build $($os.BuildNumber).$($cv.UBR)"
"Memory: total=$([math]::Round($os.TotalVisibleMemorySize / 1MB, 1)) GB free=$([math]::Round($os.FreePhysicalMemory / 1MB, 1)) GB"

try {
  Add-Type -Namespace SkyDiag -Name Native -MemberDefinition '[DllImport("kernel32.dll")] public static extern bool IsProcessorFeaturePresent(uint feature);'
  # PF_AVX2_INSTRUCTIONS_AVAILABLE = 40, PF_AVX512F_INSTRUCTIONS_AVAILABLE = 41
  "AVX2=$([SkyDiag.Native]::IsProcessorFeaturePresent(40)) AVX512F=$([SkyDiag.Native]::IsProcessorFeaturePresent(41))"
} catch {
  "IsProcessorFeaturePresent unavailable: $_"
}

# The processor state Windows is set up to save with every exception. Where it
# is large (Intel AMX on Windows Server 2022: a context of 11,648 to 11,696 bytes on the
# thread's stack, of the 12,288 kept back at a stack overflow) a stack overflow
# ends the process before any handler runs (ADR-0032). The length printed is
# InitializeContext's for the features enabled system-wide; what an exception
# of a thread really takes is in the failure messages of
# skydiag_plugin_crash_handler_runtime_tests.
try {
  Add-Type -Namespace SkyDiag -Name XState -MemberDefinition '[DllImport("kernel32.dll")] public static extern ulong GetEnabledXStateFeatures(); [DllImport("kernel32.dll")] public static extern bool InitializeContext(IntPtr buffer, uint contextFlags, IntPtr context, ref uint contextLength);'
  $xstate = [SkyDiag.XState]::GetEnabledXStateFeatures()
  $contextLength = [uint32]0
  # CONTEXT_ALL | CONTEXT_XSTATE on x64; fails by design and reports the length.
  [void][SkyDiag.XState]::InitializeContext([IntPtr]::Zero, 0x0010005F, [IntPtr]::Zero, [ref]$contextLength)
  # XSTATE_MASK_AMX_TILE_DATA = bit 18
  "XState features enabled system-wide=0x$($xstate.ToString('x')) (AMX tile data=$(($xstate -band 0x40000) -ne 0)) InitializeContext length=$contextLength bytes"
} catch {
  "XState query unavailable: $_"
}

"System32 dbghelp: $((Get-Item "$env:WINDIR\System32\dbghelp.dll").VersionInfo.FileVersion)"

try {
  $mitigation = Get-ProcessMitigation -System
  "System user shadow stack: " + (($mitigation.UserShadowStack | Format-List | Out-String).Trim() -replace '\s*\r?\n\s*', '; ')
} catch {
  "Get-ProcessMitigation unavailable: $_"
}

$deviceGuard = Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction SilentlyContinue
if ($deviceGuard) {
  "VBS status=$($deviceGuard.VirtualizationBasedSecurityStatus) services=$($deviceGuard.SecurityServicesRunning -join ',')"
}
exit 0
