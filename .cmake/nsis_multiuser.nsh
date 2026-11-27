; ==============================================================================
; Deep Dive — Multi-user (All Users / Current User) support for CPack NSIS
; Требует предварительных !define:
;   DEEPDIVE_REG_KEY, DEEPDIVE_UNINST_KEY, DEEPDIVE_INSTALL_SUBDIR
; ==============================================================================

Var DeepDiveIsAdmin
Var DeepDiveInstallMode     ; "all" или "current"
Var DeepDivePrevMode        ; отслеживаем смену режима, чтобы не затирать путь пользователя
Var DeepDiveScopeDialog
Var DeepDiveScopeLabel
Var DeepDiveScopeRadioAll
Var DeepDiveScopeRadioUser
Var DeepDiveScopeHintAll
Var DeepDiveScopeHintUser
Var DeepDiveScopeNote

; Обёртка поверх оригинального .onInit из шаблона CPack
Function .onInit
  Call _CPack_orig_onInit
  Call DeepDiveInitScope
FunctionEnd

; Обёртка поверх оригинального un.onInit из шаблона CPack
Function un.onInit
  Call un._CPack_orig_onInit
  Call un.DeepDiveInitScope
FunctionEnd

Function DeepDiveInitScope
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

  ; Проверяем ранее установленные копии в HKLM и HKCU
  ReadRegStr $1 HKLM "${DEEPDIVE_REG_KEY}" ""
  ReadRegStr $2 HKCU "${DEEPDIVE_REG_KEY}" ""

  ${If} $DeepDiveIsAdmin == "1"
    ; Если ранее ставили только для текущего пользователя (а системной копии нет) — выберем этот режим по умолчанию
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

  ; Подставляем корректный $INSTDIR по умолчанию (не трогая явный /D=... из командной строки)
  ${If} $DeepDiveInstallMode == "current"
    ${If} $INSTDIR == ""
    ${OrIf} $INSTDIR == "$PROGRAMFILES64\${DEEPDIVE_INSTALL_SUBDIR}"
    ${OrIf} $INSTDIR == "$PROGRAMFILES\${DEEPDIVE_INSTALL_SUBDIR}"
      ${If} $2 != ""
        StrCpy $INSTDIR $2
      ${Else}
        StrCpy $INSTDIR "$LOCALAPPDATA\Programs\${DEEPDIVE_INSTALL_SUBDIR}"
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
  !insertmacro MUI_HEADER_TEXT "$(DEEPDIVE_SCOPE_TITLE)" "$(DEEPDIVE_SCOPE_SUBTITLE)"

  nsDialogs::Create 1018
  Pop $DeepDiveScopeDialog
  ${If} $DeepDiveScopeDialog == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 296u 24u "$(DEEPDIVE_SCOPE_TOP)"
  Pop $DeepDiveScopeLabel

  ${NSD_CreateRadioButton} 12u 30u 284u 12u "$(DEEPDIVE_SCOPE_ALL)"
  Pop $DeepDiveScopeRadioAll
  ${NSD_AddStyle} $DeepDiveScopeRadioAll ${WS_GROUP}

  ${NSD_CreateLabel} 24u 44u 272u 12u "$PROGRAMFILES64\${DEEPDIVE_INSTALL_SUBDIR}"
  Pop $DeepDiveScopeHintAll
  SetCtlColors $DeepDiveScopeHintAll 606060 transparent

  ${NSD_CreateRadioButton} 12u 66u 284u 12u "$(DEEPDIVE_SCOPE_CURRENT)"
  Pop $DeepDiveScopeRadioUser

  ${NSD_CreateLabel} 24u 80u 272u 12u "$LOCALAPPDATA\Programs\${DEEPDIVE_INSTALL_SUBDIR}"
  Pop $DeepDiveScopeHintUser
  SetCtlColors $DeepDiveScopeHintUser 606060 transparent

  ${If} $DeepDiveIsAdmin != "1"
    EnableWindow $DeepDiveScopeRadioAll 0
    ${NSD_CreateLabel} 0 106u 296u 26u "$(DEEPDIVE_SCOPE_NOADMIN)"
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

  ; Обновляем путь установки только если пользователь переключил режим
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
        StrCpy $INSTDIR "$LOCALAPPDATA\Programs\${DEEPDIVE_INSTALL_SUBDIR}"
      ${EndIf}
    ${EndIf}
  ${EndIf}
FunctionEnd

; Точное определение контекста при удалении по фактическому $INSTDIR
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

  ; Резервная проверка по ключу Uninstall (если путь нестандартный)
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

; Запуск DeepDive.exe после установки с обычными правами пользователя
Function DeepDiveLaunchAsUser
  ${If} $DeepDiveIsAdmin == "1"
    Exec '"$WINDIR\explorer.exe" "$INSTDIR\DeepDive.exe"'
  ${Else}
    Exec '"$INSTDIR\DeepDive.exe"'
  ${EndIf}
FunctionEnd
