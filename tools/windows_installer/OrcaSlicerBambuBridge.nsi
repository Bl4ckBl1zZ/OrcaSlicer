; Windows x64 setup for OrcaSlicer with the Bambu Linux bridge. Built by build_installer.ps1,
; which passes the defines below; makensis 3.x (CI pins 3.13).
;
;   SOURCE_DIR    the installed tree (build\OrcaSlicer after "build_win.bat -s -i"), the same
;                 one the portable zip is made of
;   OUT_FILE      the setup .exe to write
;   APP_VERSION   the version shown to the user (e.g. 2.5.1-bambu.1)
;   VI_VERSION    the same as four numbers for the file's version resource (e.g. 2.5.1.1)
;   FILES_NSH     generated: one Delete per installed file, then one RMDir per folder, deepest
;                 first. The uninstaller removes exactly what was installed and nothing else.
;
; Layout and data: a per-user install into %LOCALAPPDATA%\Programs\OrcaSlicer-BambuBridge, no
; administrator rights. The empty data_dir folder next to orca-slicer.exe makes the application
; keep its settings, presets, Bambu plug-in and login there instead of %APPDATA%\OrcaSlicer, so
; this build never mixes with a stock OrcaSlicer on the same machine - exactly as the portable zip
; does. Because the folder belongs to the user, the application can write it. Upgrades and the
; uninstaller keep data_dir; the uninstaller deletes it only when the user says so. The WSL
; distro the application imports on first use (OrcaSlicer-BambuBridge, under
; %LOCALAPPDATA%\OrcaSlicer-BambuBridge) is never touched here.

Unicode true
ManifestDPIAware true
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 64

!macro REQUIRE_DEFINE NAME
  !ifndef ${NAME}
    !error "${NAME} is not defined: build this with build_installer.ps1"
  !endif
!macroend
!insertmacro REQUIRE_DEFINE SOURCE_DIR
!insertmacro REQUIRE_DEFINE OUT_FILE
!insertmacro REQUIRE_DEFINE APP_VERSION
!insertmacro REQUIRE_DEFINE VI_VERSION
!insertmacro REQUIRE_DEFINE FILES_NSH

!define APP_ID "OrcaSlicer-BambuBridge"
!define APP_NAME "OrcaSlicer Bambu Bridge"
!define APP_EXE "orca-slicer.exe"
!define APP_KEY "Software\${APP_ID}"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}"
!define UNINST_EXE "Uninstall.exe"
!define SHORTCUT "${APP_NAME}.lnk"
!define REPO_URL "https://github.com/Bl4ckBl1zZ/OrcaSlicer"

!include MUI2.nsh
!include LogicLib.nsh
!include x64.nsh
!include FileFunc.nsh

Name "${APP_NAME}"
OutFile "${OUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\${APP_ID}"
InstallDirRegKey HKCU "${APP_KEY}" "InstallDir"
BrandingText "${APP_NAME} ${APP_VERSION}"
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${VI_VERSION}"
VIFileVersion "${VI_VERSION}"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "FileDescription" "${APP_NAME} ${APP_VERSION} Setup"
VIAddVersionKey "CompanyName" "${APP_ID} (unofficial OrcaSlicer build)"
VIAddVersionKey "LegalCopyright" "OrcaSlicer contributors, GNU AGPL v3"

!define MUI_ICON "${SOURCE_DIR}\resources\images\OrcaSlicer.ico"
!define MUI_UNICON "${SOURCE_DIR}\resources\images\OrcaSlicer.ico"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_NODESC

!define MUI_WELCOMEPAGE_TITLE "${APP_NAME} ${APP_VERSION}"
!define MUI_WELCOMEPAGE_TEXT "This installs OrcaSlicer with the Bambu Linux bridge and the MCP server for your Windows account (no administrator rights needed).$\r$\n$\r$\nSettings, presets and your Bambu login stay in the data_dir folder of the installation, apart from any stock OrcaSlicer, and are kept on upgrade and uninstall.$\r$\n$\r$\nBambu printers need WSL 2 (Windows Subsystem for Linux). If it is not installed yet, run $\"wsl --install$\" in an administrator terminal and restart Windows; OrcaSlicer sets up its own WSL distro the first time it needs it."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SOURCE_DIR}\LICENSE.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP_NAME}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; Asks the user to close the application while it runs from $INSTDIR: a loaded module cannot be
; opened for writing. Opening with "a" would create the file, hence the existence check.
!macro WAIT_UNTIL_CLOSED UN
Function ${UN}WaitUntilClosed
  ${Do}
    ${IfNot} ${FileExists} "$INSTDIR\OrcaSlicer.dll"
      ${Break}
    ${EndIf}
    ClearErrors
    FileOpen $0 "$INSTDIR\OrcaSlicer.dll" a
    ${IfNot} ${Errors}
      FileClose $0
      ${Break}
    ${EndIf}
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "${APP_NAME} is running from$\r$\n$INSTDIR$\r$\n$\r$\nClose it, then choose Retry." /SD IDCANCEL IDRETRY retry
    Abort "${APP_NAME} is still running."
    retry:
  ${Loop}
FunctionEnd
!macroend
!insertmacro WAIT_UNTIL_CLOSED ""
!insertmacro WAIT_UNTIL_CLOSED "un."

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${APP_NAME} needs 64-bit Windows (x64)." /SD IDOK
    Abort
  ${EndIf}
  SetShellVarContext current
  SetRegView 64
FunctionEnd

Function un.onInit
  SetShellVarContext current
  SetRegView 64
FunctionEnd

Section "${APP_NAME}" SecApp
  SectionIn RO
  Call WaitUntilClosed

  ; Upgrade: the previous version's uninstaller removes the files it installed (data_dir and
  ; anything else the user put there stays), so files a new version no longer has do not linger.
  ; _?= runs it in place and waits for it.
  ${If} ${FileExists} "$INSTDIR\${UNINST_EXE}"
    DetailPrint "Removing the previous version"
    ExecWait '"$INSTDIR\${UNINST_EXE}" /S _?=$INSTDIR' $0
    ${If} $0 != 0
      DetailPrint "The previous uninstaller exited with $0; installing over it."
    ${EndIf}
  ${EndIf}

  SetOutPath "$INSTDIR"
  SetOverwrite on
  File /r /x data_dir "${SOURCE_DIR}\*"
  CreateDirectory "$INSTDIR\data_dir"

  WriteUninstaller "$INSTDIR\${UNINST_EXE}"

  WriteRegStr HKCU "${APP_KEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${APP_KEY}" "Version" "${APP_VERSION}"

  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${APP_ID} (unofficial OrcaSlicer build)"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE},0"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\${UNINST_EXE}"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\${UNINST_EXE}" /S'
  WriteRegStr HKCU "${UNINST_KEY}" "URLInfoAbout" "${REPO_URL}"
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"

  CreateShortCut "$SMPROGRAMS\${SHORTCUT}" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
SectionEnd

; On by default; untick it, or pass /NODESKTOP to a silent install (/S), to skip it.
Section "Desktop shortcut" SecDesktop
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/NODESKTOP" $1
  ${If} ${Errors}
    CreateShortCut "$DESKTOP\${SHORTCUT}" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
  ${EndIf}
SectionEnd

Section "Uninstall"
  Call un.WaitUntilClosed

  !include "${FILES_NSH}"
  Delete "$INSTDIR\${UNINST_EXE}"

  ; The user's settings, presets, Bambu plug-in and login. A silent uninstall (also the one an
  ; upgrade runs) always keeps them.
  ${If} ${FileExists} "$INSTDIR\data_dir\*.*"
    RMDir "$INSTDIR\data_dir"
    ${If} ${FileExists} "$INSTDIR\data_dir\*.*"
      MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Also delete your OrcaSlicer settings, presets and Bambu login?$\r$\n$\r$\n$INSTDIR\data_dir$\r$\n$\r$\nChoose No to keep them for a later installation." /SD IDNO IDNO keep_data
      RMDir /r "$INSTDIR\data_dir"
      keep_data:
    ${EndIf}
  ${EndIf}
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${SHORTCUT}"
  Delete "$DESKTOP\${SHORTCUT}"

  ; The application registers the orcaslicer:// link handler for itself at every start, and the
  ; file types the user asked it to open. Drop them only while they still point at this copy.
  ReadRegStr $0 HKCU "Software\Classes\orcaslicer\shell\open\command" ""
  ${If} $0 == '"$INSTDIR\${APP_EXE}" "%1"'
    DeleteRegKey HKCU "Software\Classes\orcaslicer"
  ${EndIf}
  ReadRegStr $0 HKCU "Software\Classes\ Orca.Slicer.1\Shell\Open\Command" ""
  ${If} $0 == '"$INSTDIR\${APP_EXE}" "%1"'
    DeleteRegKey HKCU "Software\Classes\ Orca.Slicer.1"
  ${EndIf}
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "${APP_KEY}"

  DetailPrint "Kept: the WSL distro ${APP_ID} (%LOCALAPPDATA%\${APP_ID}). Another copy of this build may use it;"
  DetailPrint "remove it yourself with: wsl --unregister ${APP_ID}"
SectionEnd
