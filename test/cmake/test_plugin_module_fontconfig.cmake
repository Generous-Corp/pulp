# Build a module through the shared plug-in module helper and inspect its ELF
# dependencies. Linux must carry fontconfig and resolve its Fc* reference.
# Other Unix platforms only prove that the helper is a no-op.
if(NOT PULP_SOURCE_DIR OR NOT WORK_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and WORK_DIR are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/src")
file(WRITE "${WORK_DIR}/src/probe.c"
    "extern void FcInit(void);\n"
    "__attribute__((constructor)) static void probe_fontconfig(void) { FcInit(); }\n")
file(WRITE "${WORK_DIR}/src/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.24)\n"
    "project(PluginFontconfigProbe C)\n"
    "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpLinkFontconfig.cmake\")\n"
    "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpPluginFormats.cmake\")\n"
    "add_library(FontconfigProbe MODULE probe.c)\n"
    "_pulp_plugin_module_link_hygiene(FontconfigProbe)\n"
    "get_target_property(_libs FontconfigProbe LINK_LIBRARIES)\n"
    "if(NOT _libs)\n set(_libs \"\")\nendif()\n"
    "file(WRITE \"${WORK_DIR}/build/module-link-libraries.txt\" \"\${_libs}\")\n")

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK_DIR}/src" -B "${WORK_DIR}/build"
    RESULT_VARIABLE configure_rc OUTPUT_VARIABLE configure_out ERROR_VARIABLE configure_err)
if(NOT configure_rc EQUAL 0)
    message(FATAL_ERROR "probe configure failed:\n${configure_out}${configure_err}")
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK_DIR}/build"
        RESULT_VARIABLE build_rc OUTPUT_VARIABLE build_out ERROR_VARIABLE build_err)
    if(NOT build_rc EQUAL 0)
        message(FATAL_ERROR "probe build failed:\n${build_out}${build_err}")
    endif()
    find_program(_ldd ldd REQUIRED)
    find_program(_readelf readelf REQUIRED)
    find_program(_nm nm REQUIRED)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DLDD=${_ldd}" "-DREADELF=${_readelf}"
        "-DMODULE=${WORK_DIR}/build/libFontconfigProbe.so"
        "-DNM=${_nm}" -DREQUIRE_FONTCONFIG=ON
        "-P" "${PULP_SOURCE_DIR}/test/cmake/check_plugin_module_no_unresolved.cmake"
        RESULT_VARIABLE check_rc OUTPUT_VARIABLE check_out ERROR_VARIABLE check_err)
    if(NOT check_rc EQUAL 0)
        message(FATAL_ERROR "Linux module fontconfig check failed:\n${check_out}${check_err}")
    endif()
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    file(READ "${WORK_DIR}/build/module-link-libraries.txt" _libs)
    if(NOT _libs STREQUAL "_libs-NOTFOUND" AND NOT _libs STREQUAL "")
        message(FATAL_ERROR "fontconfig helper changed the macOS module link interface: '${_libs}'")
    endif()
endif()
