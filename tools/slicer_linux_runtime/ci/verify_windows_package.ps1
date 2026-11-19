# Checks that an installed Windows build (build\OrcaSlicer after "build_win.bat -s -i") carries
# the complete Bambu "Linux runtime" package. The install step only warns when the Linux-built
# inputs are missing, and such a build starts fine but can never load the network plug-in - so
# packaging must stop here instead. It also checks for the MCP stdio bridge (orca-mcp-bridge.exe).
#
# Usage: verify_windows_package.ps1 -InstallDir <dir> [-RuntimeManifest <runtime-files.sha256>]
#                                   [-RequireEmptyDataDir]
#
#   -RuntimeManifest      checksums written by ci/build_linux_runtime.sh; every file of the Linux
#                         host package must be installed unchanged
#   -RequireEmptyDataDir  the portable data_dir folder must exist and be empty (a build that is
#                         about to be published must not carry settings or a login)
param(
    [Parameter(Mandatory = $true)][string]$InstallDir,
    [string]$RuntimeManifest = "",
    [switch]$RequireEmptyDataDir
)

$ErrorActionPreference = 'Stop'
$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)
$runtimeDir = Join-Path $InstallDir 'linux_runtime'
$problems = New-Object System.Collections.Generic.List[string]

function Add-Problem([string]$Text) {
    $script:problems.Add($Text)
    Write-Host "  FAIL  $Text"
}

function Read-Head([string]$Path, [int]$Count) {
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $buffer = New-Object byte[] $Count
        $read = $stream.Read($buffer, 0, $Count)
        if ($read -lt $Count) { return ,([byte[]]@()) }
        return ,$buffer
    } finally {
        $stream.Dispose()
    }
}

function Test-FileContainsAscii([string]$Path, [string]$Text) {
    $content = [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($Path))
    return $content.Contains($Text)
}

# Returns the path when the file exists with at least MinBytes, records a problem otherwise.
function Get-RequiredFile([string]$Dir, [string]$Name, [long]$MinBytes = 1) {
    $path = Join-Path $Dir $Name
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        Add-Problem "missing: $Name"
        return $null
    }
    $length = (Get-Item -LiteralPath $path).Length
    if ($length -lt $MinBytes) {
        Add-Problem "$Name is too small ($length bytes, expected at least $MinBytes)"
        return $null
    }
    Write-Host ("  ok    {0} ({1:N0} bytes)" -f $Name, $length)
    return $path
}

Write-Host "Install dir: $InstallDir"
if (!(Test-Path -LiteralPath $InstallDir -PathType Container)) {
    throw "Install dir not found: $InstallDir"
}

Write-Host "Application:"
[void](Get-RequiredFile $InstallDir 'orca-slicer.exe' 10000)
[void](Get-RequiredFile $InstallDir 'OrcaSlicer.dll' 10000000)
if (!(Test-Path -LiteralPath (Join-Path $InstallDir 'resources\profiles') -PathType Container)) {
    Add-Problem 'missing: resources\profiles'
}

# The MCP stdio bridge (src/mcp_bridge): Claude Desktop and other stdio MCP clients start it to
# reach the app's MCP server. An x64 PE executable next to orca-slicer.exe.
Write-Host "MCP stdio bridge:"
$bridge = Get-RequiredFile $InstallDir 'orca-mcp-bridge.exe' 50000
if ($bridge) {
    $head = Read-Head $bridge 1024
    if ($head.Length -ne 1024 -or $head[0] -ne 0x4D -or $head[1] -ne 0x5A) {
        Add-Problem 'orca-mcp-bridge.exe is not a PE file'
    } else {
        $pe = [System.BitConverter]::ToInt32($head, 0x3C)
        if ($pe -lt 0 -or $pe + 6 -gt $head.Length -or [System.BitConverter]::ToUInt32($head, $pe) -ne 0x00004550 -or
            [System.BitConverter]::ToUInt16($head, $pe + 4) -ne 0x8664) {
            Add-Problem 'orca-mcp-bridge.exe is not an x64 PE executable'
        }
    }
}

Write-Host "Linux runtime package (linux_runtime):"
if (!(Test-Path -LiteralPath $runtimeDir -PathType Container)) {
    Add-Problem 'missing: the linux_runtime folder'
} else {
    # Forwarder module: a PE file exporting both the plug-in ABI and the runtime's own entry points.
    $forwarder = Get-RequiredFile $runtimeDir 'slicer_linux_runtime.dll' 100000
    if ($forwarder) {
        $head = Read-Head $forwarder 2
        if ($head.Length -ne 2 -or $head[0] -ne 0x4D -or $head[1] -ne 0x5A) {
            Add-Problem 'slicer_linux_runtime.dll is not a PE file'
        }
        foreach ($symbol in 'bambu_network_create_agent', 'bambu_network_get_version', 'slicer_linux_runtime_forwarder_shutdown') {
            if (!(Test-FileContainsAscii $forwarder $symbol)) {
                Add-Problem "slicer_linux_runtime.dll does not export $symbol"
            }
        }
    }

    # Host binaries: ELF64, little endian, x86-64.
    foreach ($name in 'slicer_linux_runtime_host_abi1', 'slicer_linux_runtime_host_abi0') {
        $hostBinary = Get-RequiredFile $runtimeDir $name 500000
        if ($hostBinary) {
            $head = Read-Head $hostBinary 20
            $isElf = $head.Length -eq 20 -and $head[0] -eq 0x7F -and $head[1] -eq 0x45 -and $head[2] -eq 0x4C -and $head[3] -eq 0x46
            if (-not $isElf -or $head[4] -ne 2 -or $head[5] -ne 1 -or $head[18] -ne 0x3E -or $head[19] -ne 0) {
                Add-Problem "$name is not a linux-x86_64 ELF binary"
            }
        }
    }

    # Shell scripts run inside the distro: a "#!" line and no CR.
    foreach ($name in 'slicer_linux_runtime_host', 'slicer_linux_runtime_wsl_run_host.sh') {
        $scriptPath = Get-RequiredFile $runtimeDir $name 100
        if ($scriptPath) {
            $bytes = [System.IO.File]::ReadAllBytes($scriptPath)
            if ($bytes[0] -ne 0x23 -or $bytes[1] -ne 0x21) { Add-Problem "$name does not start with #!" }
            if ($bytes -contains 0x0D) { Add-Problem "$name has CR line endings" }
        }
    }

    # WSL rootfs: a tar archive of a root filesystem, not a stub.
    $rootfs = Get-RequiredFile $runtimeDir 'windows-wsl2-rootfs.tar' 20000000
    if ($rootfs) {
        $head = Read-Head $rootfs 262
        if ($head.Length -ne 262 -or [System.Text.Encoding]::ASCII.GetString($head, 257, 5) -ne 'ustar') {
            Add-Problem 'windows-wsl2-rootfs.tar is not a tar archive'
        }
    }

    foreach ($name in 'install_runtime.ps1', 'install_runtime.cmd', 'verify_runtime.ps1',
                      'slicer_linux_runtime_wsl_distro.txt', 'ca-certificates.crt', 'slicer_base64.cer') {
        [void](Get-RequiredFile $runtimeDir $name)
    }
    $distroFile = Join-Path $runtimeDir 'slicer_linux_runtime_wsl_distro.txt'
    if ((Test-Path -LiteralPath $distroFile) -and [string]::IsNullOrWhiteSpace((Get-Content -LiteralPath $distroFile -Raw))) {
        Add-Problem 'slicer_linux_runtime_wsl_distro.txt names no distro'
    }

    # Bambu's proprietary libraries are downloaded by the application and never redistributed.
    foreach ($name in 'libbambu_networking.so', 'libBambuSource.so', 'liblive555.so', 'libagora_rtc_sdk.so',
                      'libagora-fdkaac.so', 'network_plugins.json', 'linux_component_manifest.json') {
        if (Test-Path -LiteralPath (Join-Path $runtimeDir $name)) {
            Add-Problem "$name must not be part of a build"
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($RuntimeManifest)) {
        Write-Host "Linux host package against $(Split-Path -Leaf $RuntimeManifest):"
        $count = 0
        foreach ($line in (Get-Content -LiteralPath $RuntimeManifest)) {
            if ([string]::IsNullOrWhiteSpace($line)) { continue }
            if ($line -notmatch '^([0-9a-fA-F]{64})\s+\*?(.+)$') {
                Add-Problem "unexpected line in the runtime manifest: $line"
                continue
            }
            $count++
            $expected = $Matches[1].ToLowerInvariant()
            $name = $Matches[2].Trim()
            $file = Join-Path $runtimeDir $name
            if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
                Add-Problem "missing Linux host file: $name"
            } elseif ((Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant() -ne $expected) {
                Add-Problem "$name differs from what the Linux job built"
            } else {
                Write-Host "  ok    $name"
            }
        }
        if ($count -eq 0) { Add-Problem 'the runtime manifest is empty' }
    }
}

Write-Host "Portable data folder (data_dir):"
$dataDir = Join-Path $InstallDir 'data_dir'
if (!(Test-Path -LiteralPath $dataDir -PathType Container)) {
    Add-Problem 'missing: the data_dir folder (without it the build shares %APPDATA%\OrcaSlicer with a stock OrcaSlicer)'
} elseif ($RequireEmptyDataDir) {
    $entries = @(Get-ChildItem -LiteralPath $dataDir -Force)
    if ($entries.Count -gt 0) {
        Add-Problem "data_dir is not empty ($($entries.Count) entries); it would be published with the build"
    } else {
        Write-Host '  ok    present and empty'
    }
} else {
    Write-Host '  ok    present'
}

if ($problems.Count -gt 0) {
    throw ("The build is incomplete (Bambu Linux runtime package or MCP bridge):`n  - " + ($problems -join "`n  - "))
}
Write-Host 'Bambu Linux runtime package and MCP bridge: complete'
