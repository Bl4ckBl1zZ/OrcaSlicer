# Builds the Windows x64 setup (OrcaSlicerBambuBridge.nsi) from an installed tree: the same
# build\OrcaSlicer folder the portable zip is made of, after verify_windows_package.ps1.
#
# Usage: build_installer.ps1 -SourceDir <tree> -OutFile <setup.exe> -Version <text>
#                            [-Makensis <makensis.exe>]
#
#   -Version   what the user sees, e.g. 2.5.1-bambu.1 (a tag without its "v") or 2.5.1-1a2b3c4.
#              The file's version resource needs four numbers: the first three come from the
#              version, the fourth is the number after "-bambu." (0 without one).
#
# Writes <OutFile> and <OutFile>.sha256 ("<hash>  <name>", LF).
param(
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$OutFile,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Makensis = 'makensis'
)

$ErrorActionPreference = 'Stop'
$SourceDir = [System.IO.Path]::GetFullPath($SourceDir).TrimEnd('\')
$OutFile = [System.IO.Path]::GetFullPath($OutFile)
$script = Join-Path $PSScriptRoot 'OrcaSlicerBambuBridge.nsi'

if (!(Test-Path -LiteralPath (Join-Path $SourceDir 'orca-slicer.exe') -PathType Leaf)) {
    throw "$SourceDir is not an installed OrcaSlicer tree (no orca-slicer.exe)."
}
if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)(.*)$') {
    throw "Version '$Version' does not start with three numbers."
}
$numbers = @($Matches[1], $Matches[2], $Matches[3])
$fourth = if ($Matches[4] -match '-bambu\.(\d+)') { $Matches[1] } else { '0' }
$viVersion = '{0}.{1}.{2}.{3}' -f $numbers[0], $numbers[1], $numbers[2], $fourth
if ($Version -notmatch '^[A-Za-z0-9._+-]+$') {
    throw "Version '$Version' has characters a file version cannot carry."
}

# The data_dir folder is the user's: the installer creates it empty and never ships or deletes
# what is in it. The tree's own copy must be empty (verify_windows_package.ps1 -RequireEmptyDataDir).
$dataDir = Join-Path $SourceDir 'data_dir'
if ((Test-Path -LiteralPath $dataDir) -and @(Get-ChildItem -LiteralPath $dataDir -Force).Count -gt 0) {
    throw "$dataDir is not empty; it must not be packaged."
}
$nested = @(Get-ChildItem -LiteralPath $SourceDir -Recurse -Force -Directory -Filter 'data_dir' |
    Where-Object { $_.FullName -ne $dataDir })
if ($nested.Count -gt 0) {
    # The script excludes the name data_dir at every depth (File /r /x data_dir).
    throw "Unexpected nested data_dir folder: $($nested[0].FullName)"
}

# What the uninstaller removes: every file the installer writes, then every folder, deepest first
# and only when empty, so nothing the user added is lost.
$files = New-Object System.Collections.Generic.List[string]
$dirs = New-Object System.Collections.Generic.List[string]
foreach ($item in Get-ChildItem -LiteralPath $SourceDir -Recurse -Force) {
    $relative = $item.FullName.Substring($SourceDir.Length + 1)
    if ($relative -eq 'data_dir' -or $relative.StartsWith('data_dir\')) { continue }
    if ($item.PSIsContainer) { $dirs.Add($relative) } else { $files.Add($relative) }
}
$nsh = New-Object System.Text.StringBuilder
foreach ($f in $files) { [void]$nsh.AppendLine("Delete `"`$INSTDIR\$($f.Replace('$', '$$'))`"") }
foreach ($d in ($dirs | Sort-Object { ($_ -split '\\').Count } -Descending)) {
    [void]$nsh.AppendLine("RMDir `"`$INSTDIR\$($d.Replace('$', '$$'))`"")
}
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("orca-installer-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $filesNsh = Join-Path $work 'uninstall_files.nsh'
    # UTF-8 with a BOM: makensis reads that as UTF-8 whatever the code page.
    [System.IO.File]::WriteAllText($filesNsh, $nsh.ToString(), (New-Object System.Text.UTF8Encoding $true))
    Write-Host ("{0} files, {1} folders from {2}" -f $files.Count, $dirs.Count, $SourceDir)
    Write-Host "Version $Version (file version $viVersion)"

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutFile) | Out-Null
    $arguments = @(
        '/V3', '/INPUTCHARSET', 'UTF8',
        "/DSOURCE_DIR=$SourceDir",
        "/DOUT_FILE=$OutFile",
        "/DAPP_VERSION=$Version",
        "/DVI_VERSION=$viVersion",
        "/DFILES_NSH=$filesNsh",
        $script
    )
    $started = Get-Date
    & $Makensis @arguments
    if ($LASTEXITCODE -ne 0) { throw "makensis failed with exit code $LASTEXITCODE." }
    Write-Host ("makensis took {0:mm\:ss}" -f ((Get-Date) - $started))
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $OutFile).Hash.ToLowerInvariant()
$name = Split-Path -Leaf $OutFile
[System.IO.File]::WriteAllText("$OutFile.sha256", "$hash  $name`n")
Write-Host ("{0}: {1:N0} bytes, SHA-256 {2}" -f $name, (Get-Item -LiteralPath $OutFile).Length, $hash)
