; SPDX-License-Identifier: GPL-2.0-or-later
; Deliberately independent of CPack's upstream uninstall/association rules.
; Two explicit scopes share one generated payload/removal contract:
;   default : per-user internal test under Local AppData (existing behavior)
;   machine : owner-requested Program Files internal test with a 64-bit HKLM
;             identity and its own versioned root, Start Menu folder and
;             desktop icon. Select with makensis /DSCOPE_MACHINE only.
; Neither scope is a production release: unsigned, internal-test only, and no
; PATH, file-association or release-gate changes are ever made.
Unicode true
!ifdef SCOPE_MACHINE
RequestExecutionLevel admin
!else
RequestExecutionLevel user
!endif
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!define PRODUCT "VA Studio"
!ifndef DISPLAY_VERSION
!define DISPLAY_VERSION "1.0 beta 2"
!endif
!define CLI_KEY "Software\VACards\VAStudio\CLI\Build30"
!if /FileExists "${PAYLOAD}\bin\vastudio-cli.exe"
!else
!error "Build 30 payload requires bin\vastudio-cli.exe (build/install vastudio_cli first)"
!endif
!define KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\VACards.Inkscape.InternalTest.${TEST_VERSION}"
; VIEW-1: VA Studio draws Explorer thumbnails of .svg files (framed on the
; drawing), replacing the current provider (for example PowerToys) in this scope.
!define SVG_SHELL_STATE "Software\VACards\SvgShell"
!define SVG_THUMB_CLSID "{3A05ACA6-6D9C-4FEA-9240-E0F85BD15050}"
!define SVG_THUMB_SHELLEX "Software\Classes\.svg\ShellEx\{E357FCCD-A995-4576-B01F-234630154E96}"
!define SVG_PREVIEW_CLSID "{7828A5A8-03F2-4DAF-AE5E-E142BE538EA8}"
!define SVG_PREVIEW_SHELLEX "Software\Classes\.svg\ShellEx\{8895B1C6-B41F-4C1C-A562-0D564250836F}"
; The registry view (not the root) is selected per scope. Store the scope's
; uninstall identity beside the matching shortcut context. The root is a
; preprocessor macro so every read/write below uses the selected root.
!ifdef SCOPE_MACHINE
!define REG_ROOT HKLM
!else
!define REG_ROOT HKCU
!endif
; Bounded retry for the whole generated owned-payload removal list. The list
; raises $RemoveFailure when any single owned file is still locked, so a helper
; process that outlives the closed window is tolerated without killing it. A
; failed budget aborts nonzero and keeps identity/uninstaller for a later retry.
; No /REBOOTOK: a pending reboot is not a completed uninstall.
!ifndef REMOVE_RETRY_ATTEMPTS
!define REMOVE_RETRY_ATTEMPTS 30
!endif
!ifndef REMOVE_RETRY_INTERVAL_MS
!define REMOVE_RETRY_INTERVAL_MS 1000
!endif
Name "${PRODUCT} ${DISPLAY_VERSION} (internal test)"
OutFile "${OUTPUT_FILE}"
; Machine scope deliberately uses a separate versioned root so it can never
; become a child of, or overwrite, an existing C:\Program Files\VA Studio.
; The chosen $INSTDIR replaces this default during the directory page.
!ifdef SCOPE_MACHINE
InstallDir "$PROGRAMFILES64\${PRODUCT} ${DISPLAY_VERSION}"
!else
InstallDir "$LOCALAPPDATA\Programs\VACards Test\${TEST_VERSION}"
!endif
SetCompressor /SOLID lzma
SetCompressorDictSize 32
ShowInstDetails show
ShowUninstDetails show
!define MUI_ABORTWARNING
!define MUI_ICON "${PAYLOAD}\share\vacards-test\VACards-AppIcon.ico"
!define MUI_UNICON "${PAYLOAD}\share\vacards-test\VACards-AppIcon.ico"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
Var Existing
Var RemoveFailure
Var RemoveAttempt

Function .onInit
!ifdef SCOPE_MACHINE
  SetRegView 64
  SetShellVarContext all
!else
  SetShellVarContext current
!endif
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "This internal test requires 64-bit Windows."
    SetErrorLevel 2
    Quit
  ${EndIf}
  ReadRegStr $Existing ${REG_ROOT} "${KEY}" "InstallLocation"
  ${If} $Existing != ""
    MessageBox MB_ICONSTOP "This test version is already registered. Uninstall it first or use a new test version." /SD IDOK
    SetErrorLevel 2
    Quit
  ${EndIf}
FunctionEnd

Function CheckDestination
  ; Win32 normalization works even when all destination parents are absent.
  ; Exact prefix and nonexistence checks below are shared by both scopes.
  System::Call 'kernel32::GetFullPathNameW(w "$INSTDIR", i ${NSIS_MAX_STRLEN}, w .r0, p 0) i.r1'
  ${If} $1 == 0
  ${OrIf} $1 >= ${NSIS_MAX_STRLEN}
    MessageBox MB_ICONSTOP "The test destination path is invalid or too long." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}
  StrCpy $INSTDIR $0
!ifdef SCOPE_MACHINE
  ; Machine scope: a new DIRECT CHILD of Program Files (x64) only. The prefix
  ; check rejects anything outside Program Files; the normalized-parent check
  ; then rejects the Program Files root itself and any path nested inside an
  ; existing product directory, so another application's tree is never
  ; polluted or overwritten.
  StrLen $0 "$PROGRAMFILES64\"
  StrCpy $1 "$INSTDIR" $0
  ${If} $1 != "$PROGRAMFILES64\"
    MessageBox MB_ICONSTOP "Choose a new folder inside Program Files (x64)." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}
  ${GetParent} "$INSTDIR" $1
  ${If} $1 != "$PROGRAMFILES64"
    MessageBox MB_ICONSTOP "Choose a new folder directly inside Program Files. Nested folders, including an existing VA Studio directory, are not valid test destinations." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}
!else
  ; Per-user only, never select an existing install or nonempty directory.
  StrLen $0 "$LOCALAPPDATA\"
  StrCpy $1 "$INSTDIR" $0
  ${If} $1 != "$LOCALAPPDATA\"
    MessageBox MB_ICONSTOP "Choose a new folder inside your Local AppData directory." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}
!endif
  System::Call 'kernel32::GetFileAttributesW(w "$INSTDIR") i.r0'
  IntCmp $0 -1 destination_ok
  MessageBox MB_ICONSTOP "The destination already exists. Choose a new test directory; existing folders are never overwritten." /SD IDOK
  SetErrorLevel 2
  Abort
destination_ok:
FunctionEnd

Function CheckMachineShortcuts
!ifdef SCOPE_MACHINE
  ; A friendly display-version shortcut is shared by any two test builds that
  ; report the same DISPLAY_VERSION. Refuse before any payload write so an
  ; existing install's desktop shortcut, Start Menu folder or Start Menu link
  ; is never overwritten and no automatic upgrade happens. This runs after the
  ; registration and destination guards, and leaves files/registration intact.
  System::Call 'kernel32::GetFileAttributesW(w "$DESKTOP\${PRODUCT} ${DISPLAY_VERSION}.lnk") i.r0'
  ${If} $0 != -1
    Goto machine_shortcut_conflict
  ${EndIf}
  System::Call 'kernel32::GetFileAttributesW(w "$SMPROGRAMS\${PRODUCT} ${DISPLAY_VERSION}") i.r0'
  ${If} $0 != -1
    Goto machine_shortcut_conflict
  ${EndIf}
  System::Call 'kernel32::GetFileAttributesW(w "$SMPROGRAMS\${PRODUCT} ${DISPLAY_VERSION}\${PRODUCT} ${DISPLAY_VERSION}.lnk") i.r0'
  ${If} $0 != -1
    Goto machine_shortcut_conflict
  ${EndIf}
  Return
machine_shortcut_conflict:
  MessageBox MB_ICONSTOP "VA Studio ${DISPLAY_VERSION} internal-test shortcuts already exist. Uninstall that test install or choose a new display version; existing shortcuts are never overwritten." /SD IDOK
  SetErrorLevel 2
  Abort
!endif
FunctionEnd

; Returns a validated JSON object in $1, or a valid explicit unavailable object.
; Probe/validation failure is discovery metadata failure, not installation failure.
Function CaptureCliIdentity
  StrCpy $1 '{$\"status$\":$\"unavailable$\"}'
  InitPluginsDir
  ClearErrors
  ; nsExec merges streams: redirect the CLI's stdout to a private file and its
  ; stderr to NUL. /D prevents cmd AutoRun hooks from injecting output.
  nsExec::ExecToStack /TIMEOUT=10000 '$\"$SYSDIR\cmd.exe$\" /D /S /C $\"$\"$INSTDIR\bin\vastudio-cli.exe$\" --version --json >$\"$PLUGINSDIR\cli-identity.stdout$\" 2>NUL$\"'
  Pop $0
  Pop $2
  StrCmp $0 "0" 0 identity_done
  ; Windows PowerShell 5.1 parses the entire UTF-8 stdout, requires one object,
  ; and reserializes it. Escape non-ASCII so nsExec's code page cannot corrupt
  ; the identity, and reject output that would truncate its NSIS stack string.
  FileOpen $0 "$PLUGINSDIR\cli-identity.ps1" w
  IfErrors identity_done
  FileWrite $0 'param([string]$$Path)$\r$\n'
  FileWrite $0 '$$ErrorActionPreference = "Stop"$\r$\n'
  FileWrite $0 'try {$\r$\n'
  FileWrite $0 '  $$utf8 = New-Object System.Text.UTF8Encoding -ArgumentList $$false, $$true$\r$\n'
  FileWrite $0 '  $$raw = [IO.File]::ReadAllText($$Path, $$utf8)$\r$\n'
  FileWrite $0 '  $$obj = ConvertFrom-Json -InputObject $$raw -ErrorAction Stop$\r$\n'
  FileWrite $0 '  if ($$obj -isnot [System.Management.Automation.PSCustomObject]) { exit 1 }$\r$\n'
  FileWrite $0 '  $$json = ConvertTo-Json -InputObject $$obj -Depth 32 -Compress$\r$\n'
  FileWrite $0 '  $$json = [regex]::Replace($$json, "[^\x20-\x7e]", { param($$m) "\u{0:x4}" -f [int][char]$$m.Value })$\r$\n'
  FileWrite $0 '  if ($$json.Length -ge ${NSIS_MAX_STRLEN}) { exit 1 }$\r$\n'
  FileWrite $0 '  [Console]::Out.Write($$json)$\r$\n'
  FileWrite $0 '  exit 0$\r$\n'
  FileWrite $0 '} catch { exit 1 }$\r$\n'
  FileClose $0
  IfErrors identity_done
  nsExec::ExecToStack /TIMEOUT=10000 '$\"$SYSDIR\cmd.exe$\" /D /S /C $\"$\"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe$\" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $\"$PLUGINSDIR\cli-identity.ps1$\" $\"$PLUGINSDIR\cli-identity.stdout$\" 2>NUL$\"'
  Pop $0
  Pop $2
  StrCmp $0 "0" 0 identity_done
  StrCmp $2 "" identity_done
  StrCpy $1 $2
identity_done:
  Delete "$PLUGINSDIR\cli-identity.stdout"
  Delete "$PLUGINSDIR\cli-identity.ps1"
  ClearErrors
FunctionEnd

Section "Unsigned internal test" Main
  Call CheckDestination
  Call CheckMachineShortcuts
  ClearErrors
  SetOutPath "$INSTDIR"
  File /r "${PAYLOAD}\*"
  IfErrors install_failed
  Call CaptureCliIdentity
  SetRegView 64
  WriteRegStr ${REG_ROOT} "${CLI_KEY}" "Executable" "$INSTDIR\bin\vastudio-cli.exe"
  WriteRegStr ${REG_ROOT} "${CLI_KEY}" "BuildIdentity" "$1"
  WriteRegDWORD ${REG_ROOT} "${CLI_KEY}" "Build" 30
!ifdef SCOPE_MACHINE
  SetRegView 64
!else
  SetRegView default
!endif
  FileOpen $0 "$INSTDIR\cli-location.json" w
  IfErrors install_failed
  ; Separate writes keep the location wrapper from truncating a valid identity.
  FileWrite $0 '{$\"executable$\":$\"bin/vastudio-cli.exe$\",$\"identity$\":'
  FileWrite $0 '$1'
  FileWrite $0 '}$\r$\n'
  FileClose $0
  WriteUninstaller "$INSTDIR\Uninstall-VACards-Test.exe"
  WriteINIStr "$INSTDIR\VACards-Test-Install.ini" "Install" "Identity" "VACards.Inkscape.InternalTest.${TEST_VERSION}"
!ifdef SCOPE_MACHINE
  ; Owner-requested machine-wide beta4 shortcuts. The folder and link names
  ; include the generated display version, so they are distinct from an
  ; existing "VA Studio" install and are removed with the same names below.
  CreateDirectory "$SMPROGRAMS\${PRODUCT} ${DISPLAY_VERSION}"
  CreateShortcut "$SMPROGRAMS\${PRODUCT} ${DISPLAY_VERSION}\${PRODUCT} ${DISPLAY_VERSION}.lnk" "$INSTDIR\VACards-Test.exe" "" "$INSTDIR\VACards-Test.exe" 0
  CreateShortcut "$DESKTOP\${PRODUCT} ${DISPLAY_VERSION}.lnk" "$INSTDIR\VACards-Test.exe" "" "$INSTDIR\VACards-Test.exe" 0
!else
  CreateDirectory "$SMPROGRAMS\VACards Test"
  CreateShortcut "$SMPROGRAMS\VACards Test\${PRODUCT} ${DISPLAY_VERSION} ${TEST_VERSION}.lnk" "$INSTDIR\VACards-Test.exe" "" "$INSTDIR\VACards-Test.exe"
!endif
  WriteRegStr ${REG_ROOT} "${KEY}" "DisplayName" "${PRODUCT} ${DISPLAY_VERSION} (internal test)"
  WriteRegStr ${REG_ROOT} "${KEY}" "DisplayVersion" "${TEST_VERSION}"
  WriteRegStr ${REG_ROOT} "${KEY}" "Publisher" "VACards (internal testing)"
  WriteRegStr ${REG_ROOT} "${KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr ${REG_ROOT} "${KEY}" "DisplayIcon" "$INSTDIR\VACards-Test.exe,0"
  WriteRegStr ${REG_ROOT} "${KEY}" "UninstallString" '$\"$INSTDIR\Uninstall-VACards-Test.exe$\"'
  WriteRegDWORD ${REG_ROOT} "${KEY}" "NoModify" 1
  WriteRegDWORD ${REG_ROOT} "${KEY}" "NoRepair" 1
  ; VIEW-1: keep the previous .svg thumbnail provider in this install's key so
  ; the uninstaller can restore it, then register VA Studio's provider. The
  ; 64-bit view is required: 64-bit Explorer does not read the 32-bit CLSIDs a
  ; 32-bit installer writes by default.
  SetRegView 64
  ReadRegStr $0 ${REG_ROOT} "${SVG_THUMB_SHELLEX}" ""
  ClearErrors
  ReadRegStr $1 ${REG_ROOT} "${SVG_SHELL_STATE}" "PreviousSvgThumbnailProvider"
  ${If} ${Errors}
  ${AndIf} $0 != "${SVG_THUMB_CLSID}"
    WriteRegStr ${REG_ROOT} "${SVG_SHELL_STATE}" "PreviousSvgThumbnailProvider" "$0"
  ${EndIf}
  WriteRegStr ${REG_ROOT} "Software\Classes\CLSID\${SVG_THUMB_CLSID}" "" "VA Studio SVG thumbnails"
  WriteRegStr ${REG_ROOT} "Software\Classes\CLSID\${SVG_THUMB_CLSID}\InprocServer32" "" "$INSTDIR\bin\vasvgthumb.dll"
  WriteRegStr ${REG_ROOT} "Software\Classes\CLSID\${SVG_THUMB_CLSID}\InprocServer32" "ThreadingModel" "Apartment"
  WriteRegStr ${REG_ROOT} "${SVG_THUMB_SHELLEX}" "" "${SVG_THUMB_CLSID}"
  ReadRegStr $0 ${REG_ROOT} "${SVG_PREVIEW_SHELLEX}" ""
  ClearErrors
  ReadRegStr $1 ${REG_ROOT} "${SVG_SHELL_STATE}" "PreviousSvgPreviewHandler"
  ${If} ${Errors}
  ${AndIf} $0 != "${SVG_PREVIEW_CLSID}"
    WriteRegStr ${REG_ROOT} "${SVG_SHELL_STATE}" "PreviousSvgPreviewHandler" "$0"
  ${EndIf}
  WriteRegStr ${REG_ROOT} "Software\Classes\CLSID\${SVG_PREVIEW_CLSID}" "" "VA Studio SVG preview"
  WriteRegStr ${REG_ROOT} "Software\Classes\CLSID\${SVG_PREVIEW_CLSID}" "AppID" "{6D2B5079-2F0B-48DD-AB7F-97CEC514D30B}"
  WriteRegStr ${REG_ROOT} "Software\Classes\CLSID\${SVG_PREVIEW_CLSID}" "DisplayName" "VA Studio SVG preview"
  WriteRegDWORD ${REG_ROOT} "Software\Classes\CLSID\${SVG_PREVIEW_CLSID}" "DisableLowILProcessIsolation" 1
  WriteRegStr ${REG_ROOT} "Software\Classes\CLSID\${SVG_PREVIEW_CLSID}\InprocServer32" "" "$INSTDIR\bin\vasvgthumb.dll"
  WriteRegStr ${REG_ROOT} "Software\Classes\CLSID\${SVG_PREVIEW_CLSID}\InprocServer32" "ThreadingModel" "Apartment"
  WriteRegStr ${REG_ROOT} "${SVG_PREVIEW_SHELLEX}" "" "${SVG_PREVIEW_CLSID}"
  WriteRegStr ${REG_ROOT} "Software\Microsoft\Windows\CurrentVersion\PreviewHandlers" "${SVG_PREVIEW_CLSID}" "VA Studio SVG preview"
!ifdef SCOPE_MACHINE
  ; VIEW-2: a per-user handler (PowerToys, for example) hides this machine-wide
  ; one for the user who installs. VA Studio shows the steps when it opens.
  ReadRegStr $0 HKCU "Software\Classes\.svg\ShellEx\{E357FCCD-A995-4576-B01F-234630154E96}" ""
  ReadRegStr $1 HKCU "Software\Classes\.svg\ShellEx\{8895B1C6-B41F-4C1C-A562-0D564250836F}" ""
  ${If} $0 != ""
  ${AndIf} $0 != "${SVG_THUMB_CLSID}"
    StrCpy $2 "1"
  ${ElseIf} $1 != ""
  ${AndIf} $1 != "${SVG_PREVIEW_CLSID}"
    StrCpy $2 "1"
  ${Else}
    StrCpy $2 ""
  ${EndIf}
  ${If} $2 == "1"
    MessageBox MB_ICONINFORMATION "Another program (for example PowerToys) shows SVG thumbnails and previews in File Explorer for your user, so VA Studio's are not used yet.$\r$\n$\r$\nWhen you open VA Studio, it will show you how to switch." /SD IDOK
  ${EndIf}
!endif
!ifndef SCOPE_MACHINE
  SetRegView default
!endif
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
  Goto install_done
install_failed:
  MessageBox MB_ICONSTOP "Installation was incomplete. Preserve this directory for diagnosis; no existing installation was replaced." /SD IDOK
  SetErrorLevel 1
  Abort
install_done:
SectionEnd

Function un.onInit
!ifdef SCOPE_MACHINE
  SetRegView 64
  SetShellVarContext all
!else
  SetShellVarContext current
!endif
  ReadINIStr $0 "$INSTDIR\VACards-Test-Install.ini" "Install" "Identity"
  ReadRegStr $2 ${REG_ROOT} "${KEY}" "InstallLocation"
  ${If} $0 != "VACards.Inkscape.InternalTest.${TEST_VERSION}"
  ${OrIf} $2 != "$INSTDIR"
    MessageBox MB_ICONSTOP "Test installation identity does not match. Nothing was removed." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}
  ; A prior bounded removal may already have removed the application image.
  ; The matching INI and registry above still establish this install's identity.
  IfFileExists "$INSTDIR\bin\inkscape.exe" 0 un_image_absent
  ; Refuse removal while this test application's image is open/running.
  System::Call 'kernel32::CreateFileW(w "$INSTDIR\bin\inkscape.exe", i 0x80000000, i 0, p 0, i 3, i 0, p 0) p.r0'
  ${If} $0 == -1
    MessageBox MB_ICONSTOP "Close this VA Studio application before uninstalling." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}
  System::Call 'kernel32::CloseHandle(p r0)'
un_image_absent:
FunctionEnd

Section "Uninstall"
  ; Shared CLSIDs point to another installed test build while one exists.
  SetRegView 64
  ReadRegStr $0 ${REG_ROOT} "Software\Classes\CLSID\${SVG_THUMB_CLSID}\InprocServer32" ""
  ${If} $0 == "$INSTDIR\bin\vasvgthumb.dll"
    StrCpy $2 0
    StrCpy $3 ""
svg_shell_find_next:
    ClearErrors
    EnumRegKey $4 ${REG_ROOT} "Software\Microsoft\Windows\CurrentVersion\Uninstall" $2
    IfErrors svg_shell_found_done
    ; The end of the list is an empty name without the error flag.
    StrCmp $4 "" svg_shell_found_done
    IntOp $2 $2 + 1
    StrCpy $5 $4 30
    StrCmp $5 "VACards.Inkscape.InternalTest." 0 svg_shell_find_next
    StrCmp $4 "VACards.Inkscape.InternalTest.${TEST_VERSION}" svg_shell_find_next
    ReadRegStr $6 ${REG_ROOT} "Software\Microsoft\Windows\CurrentVersion\Uninstall\$4" "InstallLocation"
    StrCmp $6 "" svg_shell_find_next
    IfFileExists "$6\bin\vasvgthumb.dll" 0 svg_shell_find_next
    StrCpy $3 "$6\bin\vasvgthumb.dll"
svg_shell_found_done:
    ${If} $3 != ""
      WriteRegStr ${REG_ROOT} "Software\Classes\CLSID\${SVG_THUMB_CLSID}\InprocServer32" "" "$3"
      WriteRegStr ${REG_ROOT} "Software\Classes\CLSID\${SVG_PREVIEW_CLSID}\InprocServer32" "" "$3"
    ${Else}
      ReadRegStr $1 ${REG_ROOT} "${SVG_THUMB_SHELLEX}" ""
      ${If} $1 == "${SVG_THUMB_CLSID}"
        ReadRegStr $2 ${REG_ROOT} "${SVG_SHELL_STATE}" "PreviousSvgThumbnailProvider"
        ${If} $2 == ""
          DeleteRegKey ${REG_ROOT} "${SVG_THUMB_SHELLEX}"
        ${Else}
          WriteRegStr ${REG_ROOT} "${SVG_THUMB_SHELLEX}" "" "$2"
        ${EndIf}
      ${EndIf}
      ReadRegStr $1 ${REG_ROOT} "${SVG_PREVIEW_SHELLEX}" ""
      ${If} $1 == "${SVG_PREVIEW_CLSID}"
        ReadRegStr $2 ${REG_ROOT} "${SVG_SHELL_STATE}" "PreviousSvgPreviewHandler"
        ${If} $2 == ""
          DeleteRegKey ${REG_ROOT} "${SVG_PREVIEW_SHELLEX}"
        ${Else}
          WriteRegStr ${REG_ROOT} "${SVG_PREVIEW_SHELLEX}" "" "$2"
        ${EndIf}
      ${EndIf}
      DeleteRegValue ${REG_ROOT} "Software\Microsoft\Windows\CurrentVersion\PreviewHandlers" "${SVG_PREVIEW_CLSID}"
      DeleteRegKey ${REG_ROOT} "Software\Classes\CLSID\${SVG_THUMB_CLSID}"
      DeleteRegKey ${REG_ROOT} "Software\Classes\CLSID\${SVG_PREVIEW_CLSID}"
      DeleteRegKey ${REG_ROOT} "${SVG_SHELL_STATE}"
!ifndef SCOPE_MACHINE
      RMDir /r "$LOCALAPPDATA\VAStudio\SvgThumbnails"
!endif
    ${EndIf}
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
  ${EndIf}
!ifndef SCOPE_MACHINE
  SetRegView default
!endif
  ; Re-run the whole generated manifest removal list until every owned payload
  ; file is gone or the bounded budget is exhausted. Delete never sets the error
  ; flag for a missing file, so re-running a partially successful pass is safe;
  ; IfErrors clears the flag after each check so one locked file cannot hide the
  ; next failure. Unknown files are never listed and are never touched.
  ; No RMDir /r: only the manifest directories and the empty install root remain.
  StrCpy $RemoveAttempt 0
  ${Do}
    ClearErrors
    StrCpy $RemoveFailure 0
    !include "${REMOVE_LIST}"
    ${If} $RemoveFailure == 0
      ${ExitDo}
    ${EndIf}
    IntOp $RemoveAttempt $RemoveAttempt + 1
    ${If} $RemoveAttempt >= ${REMOVE_RETRY_ATTEMPTS}
      ${ExitDo}
    ${EndIf}
    Sleep ${REMOVE_RETRY_INTERVAL_MS}
  ${Loop}
  ${If} $RemoveFailure != 0
    ; Owned files stayed locked past the budget: a real, retryable failure.
    ; Keep the registry identity, install INI and uninstaller untouched.
    MessageBox MB_ICONSTOP "VA Studio test files are still in use. Close VA Studio and run the uninstaller again. The previous SVG thumbnail provider was already restored; nothing else was deregistered." /SD IDOK
    SetErrorLevel 1
    Abort
  ${EndIf}
  ; Shortcuts, identity and registry are removed only after all owned payload
  ; files are confirmed gone, so a failed uninstall never erases retryability.
  SetRegView 64
  ReadRegStr $0 ${REG_ROOT} "${CLI_KEY}" "Executable"
  ${If} $0 == "$INSTDIR\bin\vastudio-cli.exe"
    DeleteRegValue ${REG_ROOT} "${CLI_KEY}" "Executable"
    DeleteRegValue ${REG_ROOT} "${CLI_KEY}" "BuildIdentity"
    DeleteRegValue ${REG_ROOT} "${CLI_KEY}" "Build"
    DeleteRegKey /ifempty ${REG_ROOT} "${CLI_KEY}"
  ${EndIf}
!ifndef SCOPE_MACHINE
  SetRegView default
!endif
  Delete "$INSTDIR\cli-location.json"
  Delete "$INSTDIR\VACards-Test-Install.ini"
  Delete "$INSTDIR\Uninstall-VACards-Test.exe"
!ifdef SCOPE_MACHINE
  Delete "$SMPROGRAMS\${PRODUCT} ${DISPLAY_VERSION}\${PRODUCT} ${DISPLAY_VERSION}.lnk"
  RMDir "$SMPROGRAMS\${PRODUCT} ${DISPLAY_VERSION}"
  Delete "$DESKTOP\${PRODUCT} ${DISPLAY_VERSION}.lnk"
!else
  Delete "$SMPROGRAMS\VACards Test\${PRODUCT} ${DISPLAY_VERSION} ${TEST_VERSION}.lnk"
  RMDir "$SMPROGRAMS\VACards Test"
!endif
  DeleteRegKey ${REG_ROOT} "${KEY}"
  RMDir "$INSTDIR"
SectionEnd
