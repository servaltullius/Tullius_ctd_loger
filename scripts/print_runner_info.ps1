# Prints the Windows runner details the intermittent self-dump failure
# (MiniDumpWriteDump 0x800706F8) may depend on, so failing and passing CI runs
# can be compared. Diagnostic only: it never fails the job.
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
