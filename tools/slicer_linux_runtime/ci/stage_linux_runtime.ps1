# Puts the output of ci/build_linux_runtime.sh (the "linux-runtime" artifact of
# .github/workflows/build.yml) where the Windows build expects its two Linux-built inputs:
#
#   tools\slicer_linux_runtime_host\runtime\linux-x86_64\     packaged runtime host
#   tools\slicer_linux_runtime\rootfs\windows-wsl2-rootfs.tar  WSL2 rootfs
#
# Every file is checked against the checksums the Linux job recorded, so a truncated or mixed-up
# artifact fails here instead of producing a Windows build without a working bridge.
#
# Usage: stage_linux_runtime.ps1 -ArtifactDir <dir> [-RepoDir <repository root>]
param(
    [Parameter(Mandatory = $true)][string]$ArtifactDir,
    [string]$RepoDir = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepoDir)) {
    $RepoDir = Join-Path $PSScriptRoot '..\..\..'
}
$RepoDir = [System.IO.Path]::GetFullPath($RepoDir)
$ArtifactDir = [System.IO.Path]::GetFullPath($ArtifactDir)

function Read-Checksums([string]$Path) {
    $entries = @()
    foreach ($line in (Get-Content -LiteralPath $Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        if ($line -notmatch '^([0-9a-fA-F]{64})\s+\*?(.+)$') {
            throw "Unexpected line in ${Path}: $line"
        }
        $entries += [pscustomobject]@{ Hash = $Matches[1].ToLowerInvariant(); Name = $Matches[2].Trim() }
    }
    if ($entries.Count -eq 0) { throw "No checksums in $Path" }
    return $entries
}

function Assert-Checksums([string]$ChecksumFile, [string]$Dir) {
    foreach ($entry in (Read-Checksums $ChecksumFile)) {
        $file = Join-Path $Dir $entry.Name
        if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "Missing file listed in $(Split-Path -Leaf $ChecksumFile): $($entry.Name)"
        }
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant()
        if ($actual -ne $entry.Hash) {
            throw "Checksum mismatch for $($entry.Name): expected $($entry.Hash), got $actual"
        }
        Write-Host ("  ok  {0}" -f $entry.Name)
    }
}

foreach ($name in 'SHA256SUMS', 'runtime-files.sha256', 'linux-host-runtime.tar.gz', 'windows-wsl2-rootfs.tar') {
    if (!(Test-Path -LiteralPath (Join-Path $ArtifactDir $name) -PathType Leaf)) {
        throw "The Linux runtime artifact lacks ${name}: $ArtifactDir"
    }
}

Write-Host "Linux runtime artifact: $ArtifactDir"
Assert-Checksums (Join-Path $ArtifactDir 'SHA256SUMS') $ArtifactDir

# Windows' own tar (bsdtar). The GNU tar of Git for Windows reads "C:\..." as a remote host.
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (!(Test-Path -LiteralPath $tar)) { throw "tar.exe not found: $tar" }

$runtimeParent = Join-Path $RepoDir 'tools\slicer_linux_runtime_host\runtime'
$runtimeDir = Join-Path $runtimeParent 'linux-x86_64'
if (Test-Path -LiteralPath $runtimeDir) { Remove-Item -LiteralPath $runtimeDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $runtimeParent | Out-Null
& $tar -xzf (Join-Path $ArtifactDir 'linux-host-runtime.tar.gz') -C $runtimeParent
if ($LASTEXITCODE -ne 0) { throw "tar could not unpack linux-host-runtime.tar.gz (exit code $LASTEXITCODE)" }

Write-Host "Runtime host: $runtimeDir"
Assert-Checksums (Join-Path $ArtifactDir 'runtime-files.sha256') $runtimeDir

$rootfs = Join-Path $RepoDir 'tools\slicer_linux_runtime\rootfs\windows-wsl2-rootfs.tar'
Copy-Item -LiteralPath (Join-Path $ArtifactDir 'windows-wsl2-rootfs.tar') -Destination $rootfs -Force
Write-Host ("WSL rootfs:   {0} ({1:N0} bytes)" -f $rootfs, (Get-Item -LiteralPath $rootfs).Length)
