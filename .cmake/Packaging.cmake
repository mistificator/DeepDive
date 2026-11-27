# ==============================================================================
# Настройка CPack (NSIS / ZIP / DEB / TXZ / DragNDrop)
# Подключается из CMakeLists.txt после определения таргета DeepDive и install().
# ==============================================================================
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(DEEPDIVE_ARCH "win64")
    else()
        set(DEEPDIVE_ARCH "win32")
    endif()
    set(CPACK_GENERATOR "NSIS;ZIP")
elseif(APPLE)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _osx_arch_count)
    if(_osx_arch_count GREATER 1)
        set(DEEPDIVE_ARCH "universal")
    elseif(CMAKE_OSX_ARCHITECTURES)
        set(DEEPDIVE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(DEEPDIVE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    set(CPACK_GENERATOR "DragNDrop;ZIP")
else()
    set(DEEPDIVE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_GENERATOR "DEB;TXZ")
endif()

set(CPACK_PACKAGE_NAME "DeepDive")
set(CPACK_PACKAGE_VENDOR "DeepDive")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
# Явно задаём 4-компонентную версию MAJOR.MINOR.PATCH.N (по умолчанию CPack обрезает TWEAK)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Deep Dive")

# ==============================================================================
# Windows NSIS (English-only, Multi-User, без страницы лицензии)
# ==============================================================================
set(CPACK_NSIS_DISPLAY_NAME "Deep Dive")
set(CPACK_NSIS_PACKAGE_NAME "Deep Dive ${PROJECT_VERSION}")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
set(CPACK_NSIS_INSTALLED_ICON_NAME "DeepDive.exe")
set(CPACK_PACKAGE_EXECUTABLES "DeepDive" "Deep Dive")
set(CPACK_CREATE_DESKTOP_LINKS "DeepDive")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
    "System::Call 'shell32.dll::SHChangeNotify(i 0x08000000, i 0, i 0, i 0)'")
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
    "System::Call 'shell32.dll::SHChangeNotify(i 0x08000000, i 0, i 0, i 0)'")

set(DEEPDIVE_CPACK_TEMPLATE_DIR "${DEEPDIVE_GENERATED_DIR}/cpack_templates")
if(WIN32)
    set(_cpack_nsis_tpl_src "${CMAKE_ROOT}/Modules/Internal/CPack/NSIS.template.in")
    if(NOT EXISTS "${_cpack_nsis_tpl_src}")
        set(_cpack_nsis_tpl_src "${CMAKE_ROOT}/Modules/NSIS.template.in")
    endif()

    if(EXISTS "${_cpack_nsis_tpl_src}")
        file(MAKE_DIRECTORY "${DEEPDIVE_CPACK_TEMPLATE_DIR}")
        file(TO_NATIVE_PATH "${CMAKE_CURRENT_LIST_DIR}/nsis_multiuser.nsh" _nsis_multiuser_nsh)

        string(ASCII 239 187 191 _utf8_bom)
        file(READ "${_cpack_nsis_tpl_src}" _nsis_tpl)

        # Снимаем исходный UTF-8 BOM шаблона CMake до вставки Unicode True,
        # чтобы BOM остался строго в байте 0 и не попал на 2-ю строку перед ';'
        string(FIND "${_nsis_tpl}" "${_utf8_bom}" _bom_pos)
        if(_bom_pos EQUAL 0)
            string(SUBSTRING "${_nsis_tpl}" 3 -1 _nsis_tpl)
        endif()

        # Unicode True нужен для корректной работы с нелатинскими именами профилей Windows (%LOCALAPPDATA%)
        if(NOT _nsis_tpl MATCHES "(^|\n)[ \t]*Unicode[ \t]+")
            set(_nsis_tpl "Unicode True\n${_nsis_tpl}")
        endif()

        # Разрешаем запуск без UAC под обычной учётной записью
        string(REGEX REPLACE
            "(^|\n)[ \t]*RequestExecutionLevel[ \t]+admin"
            "\\1RequestExecutionLevel highest"
            _nsis_tpl "${_nsis_tpl}"
        )

        # Подключаем nsDialogs и LogicLib для страницы выбора пользователей
        if(NOT _nsis_tpl MATCHES "nsDialogs\\.nsh")
            string(REGEX REPLACE
                "(!include[ \t]+\"MUI2?\\.nsh\")"
                "\\1\n!include \"nsDialogs.nsh\"\n!include \"LogicLib.nsh\""
                _nsis_tpl "${_nsis_tpl}"
            )
        endif()

        # Страница приветствия (без абзаца про системные файлы) + выбор пользователей
        if(NOT _nsis_tpl MATCHES "DeepDiveScopePageCreate")
            string(REPLACE
                "!insertmacro MUI_PAGE_WELCOME"
                "!define MUI_WELCOMEPAGE_TITLE_3LINES\n  !define MUI_WELCOMEPAGE_TEXT \"This wizard will guide you through the installation of $(^NameDA).\\r\\n\\r\\nClick Next to continue.\"\n  !insertmacro MUI_PAGE_WELCOME\n  Page custom DeepDiveScopePageCreate DeepDiveScopePageLeave"
                _nsis_tpl "${_nsis_tpl}"
            )
        endif()

        # Убираем страницу лицензии из мастера установки
        string(REGEX REPLACE
            "(^|\n)[ \t]*!insertmacro[ \t]+MUI_PAGE_LICENSE[^\r\n]*"
            "\\1  ; License page disabled"
            _nsis_tpl "${_nsis_tpl}"
        )

        # Финальная страница с запуском DeepDive.exe без прав администратора
        if(NOT _nsis_tpl MATCHES "MUI_FINISHPAGE_TITLE_3LINES")
            string(REPLACE
                "!insertmacro MUI_PAGE_FINISH"
                "!define MUI_FINISHPAGE_TITLE_3LINES\n  !ifndef MUI_FINISHPAGE_RUN\n    !define MUI_FINISHPAGE_RUN\n  !endif\n  !define MUI_FINISHPAGE_RUN_FUNCTION DeepDiveLaunchAsUser\n  !insertmacro MUI_PAGE_FINISH"
                _nsis_tpl "${_nsis_tpl}"
            )
        endif()

        # Оставляем только английский язык установщика
        string(REGEX REPLACE
            "(^|\n)[ \t]*!insertmacro[ \t]+MUI_LANGUAGE[ \t]+\"English\"[^\r\n]*"
            "\\1  __DEEPDIVE_MUI_ENGLISH__"
            _nsis_tpl "${_nsis_tpl}"
        )
        string(REGEX REPLACE
            "(^|\n)[ \t]*!insertmacro[ \t]+MUI_LANGUAGE[ \t]+[^\r\n]*"
            "\\1  ; non-English language disabled"
            _nsis_tpl "${_nsis_tpl}"
        )
        string(REPLACE
            "__DEEPDIVE_MUI_ENGLISH__"
            "!insertmacro MUI_LANGUAGE \"English\""
            _nsis_tpl "${_nsis_tpl}"
        )

        # Переименовываем оригинальные .onInit / un.onInit, чтобы обернуть их в nsis_multiuser.nsh
        string(REGEX REPLACE
            "(^|\n)[ \t]*Function[ \t]+\\.onInit([ \t]*($|\r?\n|;))"
            "\\1Function _CPack_orig_onInit\\2"
            _nsis_tpl "${_nsis_tpl}"
        )
        string(REGEX REPLACE
            "(^|\n)[ \t]*Function[ \t]+un\\.onInit([ \t]*($|\r?\n|;))"
            "\\1Function un._CPack_orig_onInit\\2"
            _nsis_tpl "${_nsis_tpl}"
        )
        if(NOT _nsis_tpl MATCHES "_CPack_orig_onInit")
            message(WARNING "Could not wrap .onInit in NSIS.template.in; multi-user hooks may not run")
        endif()

        # Подключаем nsis_multiuser.nsh в самом конце шаблона (после языков и оригинальных функций)
        if(NOT _nsis_tpl MATCHES "nsis_multiuser\\.nsh")
            string(APPEND _nsis_tpl
                "\n!define DEEPDIVE_REG_KEY \"Software\\@CPACK_PACKAGE_VENDOR@\\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@\"\n"
                "!define DEEPDIVE_UNINST_KEY \"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@\"\n"
                "!define DEEPDIVE_INSTALL_SUBDIR \"@CPACK_PACKAGE_INSTALL_DIRECTORY@\"\n"
                "!include \"${_nsis_multiuser_nsh}\"\n"
            )
        endif()

        file(WRITE "${DEEPDIVE_CPACK_TEMPLATE_DIR}/NSIS.template.in" "${_utf8_bom}${_nsis_tpl}")
        list(PREPEND CMAKE_MODULE_PATH "${DEEPDIVE_CPACK_TEMPLATE_DIR}")
    else()
        message(WARNING "Built-in NSIS.template.in not found; custom NSIS patches were not applied")
    endif()
endif()

# ==============================================================================
# Ubuntu / Debian DEB
# ==============================================================================
set(CPACK_DEBIAN_PACKAGE_NAME "deepdive")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "DeepDive Maintainers <noreply@example.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

# ==============================================================================
# macOS DMG (DragNDrop)
# ==============================================================================
set(CPACK_DMG_VOLUME_NAME "Deep Dive ${PROJECT_VERSION}")
set(CPACK_DMG_FORMAT "UDZO")

# Персональные опции на каждый генератор (имена файлов, иконки NSIS/DMG и префиксы)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/CPackOptions.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/CPackOptions.cmake"
    @ONLY
)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/CPackOptions.cmake")

include(CPack)
