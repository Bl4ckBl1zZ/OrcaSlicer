# Prints what the build machine has: processors, memory and free disk space. The Windows build
# times of the Build workflow only mean something next to these numbers.
$ErrorActionPreference = 'Continue'

"NUMBER_OF_PROCESSORS=$env:NUMBER_OF_PROCESSORS"
Get-CimInstance Win32_Processor |
    ForEach-Object { "CPU: $($_.Name.Trim()), $($_.NumberOfCores) cores, $($_.NumberOfLogicalProcessors) logical processors" }
$os = Get-CimInstance Win32_OperatingSystem
if ($os) {
    'RAM: {0:N1} GB total, {1:N1} GB free' -f ($os.TotalVisibleMemorySize / 1MB), ($os.FreePhysicalMemory / 1MB)
    "OS: $($os.Caption) $($os.Version)"
}
Get-PSDrive -PSProvider FileSystem | Where-Object { $null -ne $_.Free } |
    ForEach-Object { 'Drive {0}: {1:N1} GB free' -f $_.Name, ($_.Free / 1GB) }
exit 0
