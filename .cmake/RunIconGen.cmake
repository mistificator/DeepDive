if(WIN32 AND QT_BIN_DIR)
    set(ENV{PATH} "${QT_BIN_DIR};$ENV{PATH}")
endif()

execute_process(
    COMMAND "${ICON_GEN}" "${OUT_DIR}"
    RESULT_VARIABLE _res
)

if(NOT _res EQUAL 0)
    message(FATAL_ERROR "DeepDiveIconGen failed with exit code ${_res}")
endif()
