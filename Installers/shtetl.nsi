#----- Include Modern UI -----#
!include "MUI2.nsh"
!include "FileFunc.nsh"

# VERSION and SHORTSHA1 are passed in by BuildInstaller.bat (/DVERSION=...).
!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef SHORTSHA1
  !define SHORTSHA1 "local"
!endif
# Directory holding the `cmake --install` output to package.
!ifndef STAGING
  !define STAGING "tmp"
!endif

Name "shtetl"
OutFile "Installshtetl-v${VERSION}-${SHORTSHA1}.exe"
BrandingText "shtetl Installer"

SetCompress force
CRCCheck on
XPStyle on

# Machine-wide install: UAC asks for permission, files go under
# C:\Program Files\shtetl and registry entries under HKLM (64-bit view).
RequestExecutionLevel admin
InstallDir "$PROGRAMFILES64\shtetl"
InstallDirRegKey HKLM "Software\shtetl" ""

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\shtetl"

!define MUI_FINISHPAGE_NOAUTOCLOSE
!define MUI_UNFINISHPAGE_NOAUTOCLOSE

!insertmacro MUI_PAGE_WELCOME
# A license page, if there is a license (Installers\COPYING.dos, CRLF line endings).
!if /FileExists "COPYING.dos"
  !define MUI_LICENSEPAGE_TEXT_TOP "shtetl is free software. Here is its license."
  !insertmacro MUI_PAGE_LICENSE "COPYING.dos"
!endif
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Section "Install"
  SetOutPath "$INSTDIR"
  # bin (shtetl.exe, DLLs, Qt plugins)
  File /r "${STAGING}\*"
  !if /FileExists "..\README.md"
    File "..\README.md"
  !endif
  !if /FileExists "..\COPYING"
    File "..\COPYING"
  !endif

  CreateDirectory "$SMPROGRAMS\shtetl"
  CreateShortCut "$SMPROGRAMS\shtetl\shtetl.lnk" "$INSTDIR\bin\shtetl.exe" "" "$INSTDIR\bin\shtetl.exe" 0
  CreateShortCut "$SMPROGRAMS\shtetl\Uninstall shtetl.lnk" "$INSTDIR\uninst.exe"

  WriteRegStr HKLM "Software\shtetl" "" "$INSTDIR"

  # Add/Remove Programs entry
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "shtetl"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninst.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninst.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"

  WriteUninstaller "$INSTDIR\uninst.exe"
SectionEnd

Section "Uninstall"
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "Software\shtetl"
  System::Call 'Shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'

  # Leave $INSTDIR first so the directory isn't locked by the uninstaller.
  SetOutPath "$TEMP"
  RMDir /r "$SMPROGRAMS\shtetl"
  RMDir /r "$INSTDIR"
SectionEnd
