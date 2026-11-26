cmake_minimum_required(VERSION 3.20)

# ==============================================================================
# Версионирование: MAJOR.MINOR.PATCH.N
#   MAJOR.MINOR.PATCH — из ближайшего достижимого git-тега (vX.Y.Z или X.Y.Z)
#   N (TWEAK)         — абсолютное число коммитов в текущей ветке Git
# ==============================================================================
set(DEEPDIVE_VERSION_MAJOR 1)
set(DEEPDIVE_VERSION_MINOR 0)
set(DEEPDIVE_VERSION_PATCH 0)
set(DEEPDIVE_VERSION_TWEAK 0)
set(DEEPDIVE_GIT_TAG "untagged")
set(DEEPDIVE_GIT_HASH "unknown")

find_package(Git QUIET)

# 1. Читаем MAJOR.MINOR.PATCH (приоритет: -DDEEPDIVE_VERSION_TAG, затем git tag, иначе fallback 1.0.0)
set(_raw_version_tag "")
if(DEFINED DEEPDIVE_VERSION_TAG AND NOT "${DEEPDIVE_VERSION_TAG}" STREQUAL "")
    set(_raw_version_tag "${DEEPDIVE_VERSION_TAG}")
elseif(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
                --match "v[0-9]*.[0-9]*.[0-9]*"
                --match "[0-9]*.[0-9]*.[0-9]*"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_tag
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _git_tag_res
        ERROR_QUIET
    )
    if(_git_tag_res EQUAL 0 AND NOT "${_git_tag}" STREQUAL "")
        set(_raw_version_tag "${_git_tag}")
    endif()
endif()

if(_raw_version_tag MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(DEEPDIVE_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(DEEPDIVE_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(DEEPDIVE_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(DEEPDIVE_GIT_TAG "${_raw_version_tag}")
else()
    message(WARNING "No valid semver git tag (vX.Y.Z) found; falling back to "
                    "${DEEPDIVE_VERSION_MAJOR}.${DEEPDIVE_VERSION_MINOR}.${DEEPDIVE_VERSION_PATCH}")
endif()

# 2. Читаем TWEAK (абсолютный номер коммита) и короткий хэш
if(DEFINED DEEPDIVE_BUILD_NUMBER AND DEEPDIVE_BUILD_NUMBER MATCHES "^[0-9]+$")
    set(DEEPDIVE_VERSION_TWEAK "${DEEPDIVE_BUILD_NUMBER}")
elseif(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_commit_count
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _git_count_res
        ERROR_QUIET
    )
    if(_git_count_res EQUAL 0 AND _git_commit_count MATCHES "^[0-9]+$")
        set(DEEPDIVE_VERSION_TWEAK "${_git_commit_count}")
    endif()
endif()

if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_short_hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _git_hash_res
        ERROR_QUIET
    )
    if(_git_hash_res EQUAL 0 AND NOT "${_git_short_hash}" STREQUAL "")
        set(DEEPDIVE_GIT_HASH "${_git_short_hash}")
    endif()
endif()

project(DeepDive
    VERSION "${DEEPDIVE_VERSION_MAJOR}.${DEEPDIVE_VERSION_MINOR}.${DEEPDIVE_VERSION_PATCH}.${DEEPDIVE_VERSION_TWEAK}"
    DESCRIPTION "Procedural 3D Underwater World Tech Demo (Qt6 / OpenGL 3.3)"
    LANGUAGES CXX
)

message(STATUS "DeepDive version: ${PROJECT_VERSION} (tag: ${DEEPDIVE_GIT_TAG}, commit: ${DEEPDIVE_GIT_HASH})")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Multimedia)

# ==============================================================================
# Генерация version.h, иконок и платформенных метаданных
# ==============================================================================
set(DEEPDIVE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(DEEPDIVE_ICON_DIR "${DEEPDIVE_GENERATED_DIR}/icons")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/.cmake/version.h.in"
    "${DEEPDIVE_GENERATED_DIR}/version.h"
    @ONLY
)

# Консольный генератор иконок и NSIS-баннеров (работает на этапе сборки, не попадает в пакет)
add_executable(DeepDiveIconGen
    tools/IconGenerator.cpp
    src/AppIcon.h
)
target_include_directories(DeepDiveIconGen PRIVATE src)
target_link_libraries(DeepDiveIconGen PRIVATE Qt6::Gui)

set(DEEPDIVE_GENERATED_ICONS
    "${DEEPDIVE_ICON_DIR}/deepdive.svg"
    "${DEEPDIVE_ICON_DIR}/deepdive.ico"
    "${DEEPDIVE_ICON_DIR}/deepdive.icns"
    "${DEEPDIVE_ICON_DIR}/nsis_welcome.bmp"
    "${DEEPDIVE_ICON_DIR}/nsis_header.bmp"
    "${DEEPDIVE_ICON_DIR}/deepdive_16.png"
    "${DEEPDIVE_ICON_DIR}/deepdive_32.png"
    "${DEEPDIVE_ICON_DIR}/deepdive_48.png"
    "${DEEPDIVE_ICON_DIR}/deepdive_64.png"
    "${DEEPDIVE_ICON_DIR}/deepdive_128.png"
    "${DEEPDIVE_ICON_DIR}/deepdive_256.png"
    "${DEEPDIVE_ICON_DIR}/deepdive_512.png"
)

add_custom_command(
    OUTPUT ${DEEPDIVE_GENERATED_ICONS}
    COMMAND "${CMAKE_COMMAND}"
            "-DICON_GEN=$<TARGET_FILE:DeepDiveIconGen>"
            "-DQT_BIN_DIR=$<TARGET_FILE_DIR:Qt6::Core>"
            "-DOUT_DIR=${DEEPDIVE_ICON_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/.cmake/RunIconGen.cmake"
    DEPENDS
        DeepDiveIconGen
        "${CMAKE_CURRENT_SOURCE_DIR}/src/AppIcon.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/IconGenerator.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/.cmake/RunIconGen.cmake"
    COMMENT "Generating DeepDive vector & platform icons"
    VERBATIM
)
add_custom_target(DeepDiveIcons DEPENDS ${DEEPDIVE_GENERATED_ICONS})

set(DEEPDIVE_PLATFORM_SOURCES "")
if(WIN32)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/.cmake/version.rc.in"
        "${DEEPDIVE_GENERATED_DIR}/version.rc"
        @ONLY
    )
    set_source_files_properties("${DEEPDIVE_GENERATED_DIR}/version.rc" PROPERTIES
        OBJECT_DEPENDS "${DEEPDIVE_ICON_DIR}/deepdive.ico"
    )
    list(APPEND DEEPDIVE_PLATFORM_SOURCES "${DEEPDIVE_GENERATED_DIR}/version.rc")
elseif(APPLE)
    set_source_files_properties("${DEEPDIVE_ICON_DIR}/deepdive.icns" PROPERTIES
        GENERATED TRUE
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    list(APPEND DEEPDIVE_PLATFORM_SOURCES "${DEEPDIVE_ICON_DIR}/deepdive.icns")
elseif(UNIX)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/.cmake/deepdive.desktop.in"
        "${DEEPDIVE_GENERATED_DIR}/deepdive.desktop"
        @ONLY
    )
endif()

# ==============================================================================
# Основной исполняемый таргет
# ==============================================================================
add_executable(DeepDive WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/AppIcon.h
    src/GpuCapabilities.h
    src/Noise.h
    src/ShadersGeometry.h
    src/ShadersAtmosphere.h
    src/WorldGenerator.h
    src/WorldGeneratorFlora.cpp
    src/WorldGeneratorFauna.cpp
    src/WorldGeneratorEnvironment.cpp
    src/UnderwaterAudio.h
    src/UnderwaterAudio.cpp
    src/DeepDiveWidget.h
    src/DeepDiveWidget.cpp
    src/DeepDiveEngine.h
    src/DeepDiveEngine.cpp
    src/DeepDiveRender.h
    src/DeepDiveRender.cpp
    src/GamepadInput.h

    .github/workflows/build-release.yml

    ${DEEPDIVE_PLATFORM_SOURCES}
)

add_dependencies(DeepDive DeepDiveIcons)

target_include_directories(DeepDive PRIVATE
    src
    "${DEEPDIVE_GENERATED_DIR}"
)

target_link_libraries(DeepDive PRIVATE
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Multimedia
)

if(WIN32)
    target_link_libraries(DeepDive PRIVATE dxgi)
elseif(APPLE)
    target_compile_definitions(DeepDive PRIVATE GL_SILENCE_DEPRECATION)
    set_target_properties(DeepDive PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Deep Dive"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.deepdive.techdemo"
        MACOSX_BUNDLE_ICON_FILE "deepdive.icns"
        MACOSX_BUNDLE_BUNDLE_VERSION "${DEEPDIVE_VERSION_TWEAK}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_STRING "Deep Dive ${PROJECT_VERSION}"
    )
endif()

# ==============================================================================
# Правила установки (install) и деплой зависимостей Qt6
# ==============================================================================
if(WIN32)
    install(TARGETS DeepDive RUNTIME DESTINATION ".")

    # Копируем MSVC Runtime DLL рядом с exe (нужно и для NSIS, и для portable ZIP)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
    set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
    include(InstallRequiredSystemLibraries)

    get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}" REQUIRED)

    install(CODE "
        set(_staging \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
        set(_exe \"\${_staging}/DeepDive.exe\")
        if(\"\${CMAKE_INSTALL_CONFIG_NAME}\" STREQUAL \"Debug\")
            set(_deploy_mode \"--debug\")
        else()
            set(_deploy_mode \"--release\")
        endif()
        message(STATUS \"Running windeployqt on \${_exe}\")
        execute_process(
            COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                \${_deploy_mode}
                --no-translations
                --no-opengl-sw
                --no-system-d3d-compiler
                --no-compiler-runtime
                \"\${_exe}\"
            RESULT_VARIABLE _windeploy_res
        )
        if(NOT _windeploy_res EQUAL 0)
            message(FATAL_ERROR \"windeployqt failed with code \${_windeploy_res}\")
        endif()
    ")

elseif(APPLE)
    install(TARGETS DeepDive BUNDLE DESTINATION ".")

    get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
    find_program(MACDEPLOYQT_EXECUTABLE macdeployqt HINTS "${_qt_bin_dir}" REQUIRED)

    install(CODE "
        set(_staging \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
        set(_bundle \"\${_staging}/DeepDive.app\")
        message(STATUS \"Running macdeployqt on \${_bundle}\")
        execute_process(
            COMMAND \"${MACDEPLOYQT_EXECUTABLE}\" \"\${_bundle}\" -always-overwrite
            RESULT_VARIABLE _macdeploy_res
        )
        if(NOT _macdeploy_res EQUAL 0)
            message(FATAL_ERROR \"macdeployqt failed with code \${_macdeploy_res}\")
        endif()
        # Apple Silicon требует валидную подпись после правки rpath утилитой macdeployqt
        execute_process(
            COMMAND codesign --force --deep --sign - \"\${_bundle}\"
            RESULT_VARIABLE _sign_res
        )
        if(NOT _sign_res EQUAL 0)
            message(FATAL_ERROR \"codesign failed with code \${_sign_res}\")
        endif()
    ")

else()
    include(GNUInstallDirs)
    install(TARGETS DeepDive RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(
        FILES "${DEEPDIVE_GENERATED_DIR}/deepdive.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    )
    install(
        FILES "${DEEPDIVE_ICON_DIR}/deepdive.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
    )
    foreach(_icon_sz 16 32 48 64 128 256 512)
        install(
            FILES "${DEEPDIVE_ICON_DIR}/deepdive_${_icon_sz}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_icon_sz}x${_icon_sz}/apps"
            RENAME "deepdive.png"
        )
    endforeach()
    install(
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}"
        OPTIONAL
    )
endif()

# ==============================================================================
# Настройка CPack
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
# Явно задаём 4-компонентную версию 1.0.0.N (по умолчанию CPack обрезает TWEAK)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Deep Dive")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
endif()

# Windows NSIS
set(CPACK_NSIS_DISPLAY_NAME "Deep Dive")
set(CPACK_NSIS_PACKAGE_NAME "Deep Dive ${PROJECT_VERSION}")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
set(CPACK_NSIS_INSTALLED_ICON_NAME "DeepDive.exe")
set(CPACK_PACKAGE_EXECUTABLES "DeepDive" "Deep Dive")
set(CPACK_CREATE_DESKTOP_LINKS "DeepDive")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_MODIFY_PATH OFF)

# Ubuntu / Debian DEB
set(CPACK_DEBIAN_PACKAGE_NAME "deepdive")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "DeepDive Maintainers <noreply@example.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

# macOS DMG (DragNDrop)
set(CPACK_DMG_VOLUME_NAME "Deep Dive ${PROJECT_VERSION}")
set(CPACK_DMG_FORMAT "UDZO")

# Персональные опции на каждый генератор (имена файлов, иконки NSIS/DMG и префиксы)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/.cmake/CPackOptions.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/CPackOptions.cmake"
    @ONLY
)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/CPackOptions.cmake")

include(CPack)
