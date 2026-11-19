# Installs a setup built by build_installer.ps1 silently into a scratch folder, checks the result,
# installs it again over itself (an upgrade), uninstalls it, and checks what is left. Runs in CI
# after the build and by hand before a release.
#
# Usage: test_installer.ps1 -Installer <setup.exe> -InstallDir <scratch folder>
#                           [-SourceDir <tree the setup was built from>]
#                           [-RuntimeManifest <runtime-files.sha256>]
#
# The setup writes the per-user uninstall entry, the Start menu shortcut and HKCU\Software\
# OrcaSlicer-BambuBridge of the account that runs this, so the script refuses to run when that
# account already has the application installed. No desktop shortcut (/NODESKTOP). Nothing is
# started, and WSL is not touched.
param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$InstallDir,
    [string]$SourceDir = '',
    [string]$RuntimeManifest = ''
)

$ErrorActionPreference = 'Stop'
$Installer = [System.IO.Path]::GetFullPath($Installer)
$InstallDir = [System.IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
$verifier = Join-Path $PSScriptRoot '..\slicer_linux_runtime\ci\verify_windows_package.ps1'
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\OrcaSlicer-BambuBridge'
$appKey = 'HKCU:\Software\OrcaSlicer-BambuBridge'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'OrcaSlicer Bambu Bridge.lnk'
$desktopShortcut = Join-Path ([Environment]::GetFolderPath('Desktop')) 'OrcaSlicer Bambu Bridge.lnk'

function Assert([bool]$Condition, [string]$Text) {
    if (-not $Condition) { throw "FAILED: $Text" }
    Write-Host "  ok    $Text"
}

function Invoke-Setup([string[]]$Arguments) {
    $started = Get-Date
    # /D= must come last and unquoted.
    $process = Start-Process -FilePath $Installer -ArgumentList ($Arguments + "/D=$InstallDir") -PassThru -Wait
    Write-Host ("  setup exited with {0} after {1:mm\:ss}" -f $process.ExitCode, ((Get-Date) - $started))
    return $process.ExitCode
}

function Get-RelativeFiles([string]$Root) {
    Get-ChildItem -LiteralPath $Root -Recurse -Force -File |
        ForEach-Object { $_.FullName.Substring($Root.Length + 1) } | Sort-Object
}

if ((Test-Path $uninstallKey) -or (Test-Path $appKey)) {
    throw 'OrcaSlicer Bambu Bridge is installed for this account; this test would replace that installation.'
}
if ((Test-Path -LiteralPath $InstallDir) -and @(Get-ChildItem -LiteralPath $InstallDir -Force).Count -gt 0) {
    throw "$InstallDir is not empty."
}
$verifyArgs = @{ InstallDir = $InstallDir }
if ($RuntimeManifest) { $verifyArgs.RuntimeManifest = $RuntimeManifest }

Write-Host "Install into $InstallDir"
Assert ((Invoke-Setup @('/S', '/NODESKTOP')) -eq 0) 'silent install exit code 0'
& $verifier @verifyArgs -RequireEmptyDataDir
Assert (Test-Path -LiteralPath (Join-Path $InstallDir 'Uninstall.exe')) 'Uninstall.exe written'
if ($SourceDir) {
    $SourceDir = [System.IO.Path]::GetFullPath($SourceDir).TrimEnd('\')
    $expected = @(Get-RelativeFiles $SourceDir)
    $actual = @(Get-RelativeFiles $InstallDir | Where-Object { $_ -ne 'Uninstall.exe' })
    $diff = @(Compare-Object $expected $actual)
    Assert ($diff.Count -eq 0) ("installed files equal the source tree ({0} files){1}" -f $actual.Count,
        $(if ($diff.Count) { ': ' + (($diff | Select-Object -First 5 | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) -join ', ') } else { '' }))
}
$entry = Get-ItemProperty $uninstallKey
Assert ($entry.DisplayName -eq 'OrcaSlicer Bambu Bridge') "Add/Remove Programs entry '$($entry.DisplayName)' $($entry.DisplayVersion)"
Assert ($entry.InstallLocation -eq $InstallDir) 'entry points at the install folder'
Assert ($entry.UninstallString -eq "`"$InstallDir\Uninstall.exe`"") 'entry has the uninstaller'
Assert ($entry.EstimatedSize -gt 100000) "estimated size $($entry.EstimatedSize) KB"
Assert ((Get-ItemProperty $appKey).InstallDir -eq $InstallDir) 'HKCU\Software\OrcaSlicer-BambuBridge InstallDir'
Assert (Test-Path -LiteralPath $shortcut) 'Start menu shortcut'
Assert (-not (Test-Path -LiteralPath $desktopShortcut)) 'no desktop shortcut with /NODESKTOP'
$shell = New-Object -ComObject WScript.Shell
Assert ($shell.CreateShortcut($shortcut).TargetPath -eq "$InstallDir\orca-slicer.exe") 'shortcut target'

Write-Host 'Upgrade over it, with something in data_dir'
$marker = Join-Path $InstallDir 'data_dir\OrcaSlicer.conf'
Set-Content -LiteralPath $marker -Value 'installer test' -Encoding ascii
$userFile = Join-Path $InstallDir 'resources\user-added.txt'
Set-Content -LiteralPath $userFile -Value 'installer test' -Encoding ascii
Assert ((Invoke-Setup @('/S', '/NODESKTOP')) -eq 0) 'silent upgrade exit code 0'
& $verifier @verifyArgs
Assert ((Get-Content -LiteralPath $marker) -eq 'installer test') 'data_dir kept across the upgrade'
Assert (Test-Path -LiteralPath $userFile) 'a file the user added is kept'
Remove-Item -LiteralPath $userFile

Write-Host 'Uninstall'
$started = Get-Date
# A normal uninstall copies itself to %TEMP% and returns at once; wait for its entry to go.
$process = Start-Process -FilePath (Join-Path $InstallDir 'Uninstall.exe') -ArgumentList '/S' -PassThru -Wait
Assert ($process.ExitCode -eq 0) 'uninstaller started'
$deadline = (Get-Date).AddMinutes(10)
while ((Test-Path $uninstallKey) -or (Test-Path -LiteralPath (Join-Path $InstallDir 'Uninstall.exe'))) {
    if ((Get-Date) -gt $deadline) { throw 'FAILED: the uninstaller did not finish within 10 minutes' }
    Start-Sleep -Seconds 2
}
Write-Host ("  uninstall took {0:mm\:ss}" -f ((Get-Date) - $started))
Assert (-not (Test-Path $appKey)) 'HKCU\Software\OrcaSlicer-BambuBridge removed'
Assert (-not (Test-Path -LiteralPath $shortcut)) 'Start menu shortcut removed'
$left = @(Get-ChildItem -LiteralPath $InstallDir -Recurse -Force | ForEach-Object { $_.FullName.Substring($InstallDir.Length + 1) })
Assert (($left -join '|') -eq 'data_dir|data_dir\OrcaSlicer.conf') "only the user's data_dir is left ($($left -join ', '))"

Remove-Item -LiteralPath $InstallDir -Recurse -Force
Write-Host 'Installer test: passed'
