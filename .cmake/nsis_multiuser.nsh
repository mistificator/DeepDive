; ==============================================================================
; Deep Dive — Multi-user (All Users / Current User) support for CPack NSIS
; Requires prior !define:
;   DEEPDIVE_REG_KEY, DEEPDIVE_UNINST_KEY, DEEPDIVE_INSTALL_SUBDIR
; ==============================================================================

Var DeepDiveIsAdmin
Var DeepDiveInstallMode       ; "all" or "current"
Var DeepDivePrevMode          ; tracks mode switch to avoid overwriting custom $INSTDIR
Var DeepDiveUserLocalAppData  ; real per-user %LOCALAPPDATA% (independent of SetShellVarContext)
Var DeepDiveScopeDialog
Var DeepDiveScopeLabel
Var DeepDiveScopeRadioAll
Var DeepDiveScopeRadioUser
Var DeepDiveScopeHintAll
Var DeepDiveScopeHintUser
Var DeepDiveScopeNote

; Wraps original .onInit from CPack NSIS template
Function .onInit
  Call _CPack_orig_onInit
  Call DeepDiveInitScope
FunctionEnd

; Wraps original un.onInit from CPack NSIS template
Function un.onInit
  Call un._CPack_orig_onInit
  Call un.DeepDiveInitScope
FunctionEnd

Function DeepDiveInitScope
  ; Capture actual user %LOCALAPPDATA% before potentially switching to SetShellVarContext all
  SetShellVarContext current
  StrCpy $DeepDiveUserLocalAppData "$LOCALAPPDATA"

  StrCpy $DeepDiveIsAdmin "0"
  ClearErrors
  UserInfo::GetAccountType
  IfErrors deepdive_not_admin
  Pop $0
  ${If} $0 == "Admin"
  ${OrIf} $0 == "Power"
    StrCpy $DeepDiveIsAdmin "1"
  ${EndIf}
deepdive_not_admin:

  ; Check existing installations in HKLM and HKCU
  ReadRegStr $1 HKLM "${DEEPDIVE_REG_KEY}" ""
  ReadRegStr $2 HKCU "${DEEPDIVE_REG_KEY}" ""

  ${If} $DeepDiveIsAdmin == "1"
    ${If} $2 != ""
    ${AndIf} $1 == ""
      StrCpy $DeepDiveInstallMode "current"
      SetShellVarContext current
    ${Else}
      StrCpy $DeepDiveInstallMode "all"
      SetShellVarContext all
    ${EndIf}
  ${Else}
    StrCpy $DeepDiveInstallMode "current"
    SetShellVarContext current
  ${EndIf}

  StrCpy $DeepDivePrevMode $DeepDiveInstallMode

  ; Set default $INSTDIR unless user passed an explicit /D=... path
  ${If} $DeepDiveInstallMode == "current"
    ${If} $INSTDIR == ""
    ${OrIf} $INSTDIR == "$PROGRAMFILES64\${DEEPDIVE_INSTALL_SUBDIR}"
    ${OrIf} $INSTDIR == "$PROGRAMFILES\${DEEPDIVE_INSTALL_SUBDIR}"
      ${If} $2 != ""
        StrCpy $INSTDIR $2
      ${Else}
        StrCpy $INSTDIR "$DeepDiveUserLocalAppData\Programs\${DEEPDIVE_INSTALL_SUBDIR}"
      ${EndIf}
    ${EndIf}
  ${Else}
    ${If} $1 != ""
    ${AndIf} $INSTDIR == "$PROGRAMFILES64\${DEEPDIVE_INSTALL_SUBDIR}"
      StrCpy $INSTDIR $1
    ${EndIf}
  ${EndIf}
FunctionEnd

Function DeepDiveScopePageCreate
  !insertmacro MUI_HEADER_TEXT "Choose Installation Options" "Choose for which users to install $(^NameDA)."

  nsDialogs::Create 1018
  Pop $DeepDiveScopeDialog
  ${If} $DeepDiveScopeDialog == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 296u 24u "Please select whether you wish to make this software available to all users or just yourself."
  Pop $DeepDiveScopeLabel

  ${NSD_CreateRadioButton} 12u 30u 284u 12u "Install for anyone using this computer (all users)"
  Pop $DeepDiveScopeRadioAll
  ${NSD_AddStyle} $DeepDiveScopeRadioAll ${WS_GROUP}

  ${NSD_CreateLabel} 24u 44u 272u 12u "$PROGRAMFILES64\${DEEPDIVE_INSTALL_SUBDIR}"
  Pop $DeepDiveScopeHintAll
  SetCtlColors $DeepDiveScopeHintAll 606060 transparent

  ${NSD_CreateRadioButton} 12u 66u 284u 12u "Install just for me (current user)"
  Pop $DeepDiveScopeRadioUser

  ${NSD_CreateLabel} 24u 80u 272u 12u "$DeepDiveUserLocalAppData\Programs\${DEEPDIVE_INSTALL_SUBDIR}"
  Pop $DeepDiveScopeHintUser
  SetCtlColors $DeepDiveScopeHintUser 606060 transparent

  ${If} $DeepDiveIsAdmin != "1"
    EnableWindow $DeepDiveScopeRadioAll 0
    ${NSD_CreateLabel} 0 106u 296u 24u "Administrator privileges are required to install for all users."
    Pop $DeepDiveScopeNote
    SetCtlColors $DeepDiveScopeNote 606060 transparent
  ${EndIf}

  ${If} $DeepDiveInstallMode == "all"
    ${NSD_Check} $DeepDiveScopeRadioAll
  ${Else}
    ${NSD_Check} $DeepDiveScopeRadioUser
  ${EndIf}

  nsDialogs::Show
FunctionEnd

Function DeepDiveScopePageLeave
  ${NSD_GetState} $DeepDiveScopeRadioAll $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $DeepDiveInstallMode "all"
    SetShellVarContext all
  ${Else}
    StrCpy $DeepDiveInstallMode "current"
    SetShellVarContext current
  ${EndIf}

  ; Update install path only when the user switched modes
  ${If} $DeepDiveInstallMode != $DeepDivePrevMode
    StrCpy $DeepDivePrevMode $DeepDiveInstallMode
    ${If} $DeepDiveInstallMode == "all"
      ReadRegStr $1 HKLM "${DEEPDIVE_REG_KEY}" ""
      ${If} $1 != ""
        StrCpy $INSTDIR $1
      ${Else}
        StrCpy $INSTDIR "$PROGRAMFILES64\${DEEPDIVE_INSTALL_SUBDIR}"
      ${EndIf}
    ${Else}
      ReadRegStr $1 HKCU "${DEEPDIVE_REG_KEY}" ""
      ${If} $1 != ""
        StrCpy $INSTDIR $1
      ${Else}
        StrCpy $INSTDIR "$DeepDiveUserLocalAppData\Programs\${DEEPDIVE_INSTALL_SUBDIR}"
      ${EndIf}
    ${EndIf}
  ${EndIf}
FunctionEnd

; Match uninstall context to the actual $INSTDIR being removed
Function un.DeepDiveInitScope
  SetShellVarContext current

  ReadRegStr $0 HKCU "${DEEPDIVE_REG_KEY}" ""
  ${If} $0 == "$INSTDIR"
    SetShellVarContext current
    Return
  ${EndIf}

  ReadRegStr $0 HKLM "${DEEPDIVE_REG_KEY}" ""
  ${If} $0 == "$INSTDIR"
    SetShellVarContext all
    Return
  ${EndIf}

  ReadRegStr $0 HKCU "${DEEPDIVE_UNINST_KEY}" "UninstallString"
  ${If} $0 != ""
    SetShellVarContext current
    Return
  ${EndIf}

  ReadRegStr $0 HKLM "${DEEPDIVE_UNINST_KEY}" "UninstallString"
  ${If} $0 != ""
    SetShellVarContext all
    Return
  ${EndIf}
FunctionEnd

; Launch DeepDive.exe without admin elevation after setup completes
Function DeepDiveLaunchAsUser
  ${If} $DeepDiveIsAdmin == "1"
    Exec '"$WINDIR\explorer.exe" "$INSTDIR\DeepDive.exe"'
  ${Else}
    Exec '"$INSTDIR\DeepDive.exe"'
  ${EndIf}
FunctionEnd
