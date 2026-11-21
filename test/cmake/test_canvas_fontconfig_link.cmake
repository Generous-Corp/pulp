# The installed package config attaches libfontconfig to Pulp::canvas, after the
# Skia archive, so a consumer that links Pulp's static view or canvas libraries
# straight into a plug-in module names it as a needed library.
#
# The test configures a throwaway project with stand-in Skia and canvas targets,
# applies the helper the package config calls, and reads the resulting link
# interface back. It runs on every platform because it drives
# pulp_fontconfig_needed_on_target, the part with no platform gate. A second
# case runs the platform-gated wrapper and expects it to touch nothing off Linux.
if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and FIXTURE_DIR are required")
endif()

function(_run name body out_libs)
    set(_src "${FIXTURE_DIR}/${name}-src")
    set(_bin "${FIXTURE_DIR}/${name}-build")
    file(REMOVE_RECURSE "${_src}" "${_bin}")
    file(MAKE_DIRECTORY "${_src}")
    file(WRITE "${_src}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.24)\n"
        "project(CanvasFontconfig NONE)\n"
        "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpLinkFontconfig.cmake\")\n"
        "add_library(skia::skia INTERFACE IMPORTED)\n"
        "add_library(Pulp::canvas INTERFACE IMPORTED)\n"
        "set_property(TARGET Pulp::canvas APPEND PROPERTY INTERFACE_LINK_LIBRARIES skia::skia)\n"
        "${body}\n"
        "get_target_property(_libs Pulp::canvas INTERFACE_LINK_LIBRARIES)\n"
        "message(STATUS \"LIBS=[\${_libs}]\")\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_src}" -B "${_bin}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${name}: configure failed:\n${_out}${_err}")
    endif()
    string(REGEX MATCH "LIBS=\\[([^]]*)\\]" _m "${_out}")
    if(NOT _m)
        message(FATAL_ERROR "${name}: the link interface was not printed, so the probe reads nothing:\n${_out}")
    endif()
    set(${out_libs} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# Without the helper the interface is exactly the stand-in Skia: the baseline the
# next case must differ from.
_run(baseline "" _baseline)
if(NOT _baseline STREQUAL "skia::skia")
    message(FATAL_ERROR "baseline interface is '${_baseline}', expected 'skia::skia'")
endif()

_run(attached "pulp_fontconfig_needed_on_target(Pulp::canvas /fake/libfontconfig.so)" _attached)
set(_expected "skia::skia;-Wl,--push-state,--no-as-needed;/fake/libfontconfig.so;-Wl,--pop-state")
if(NOT _attached STREQUAL _expected)
    message(FATAL_ERROR "attached interface is '${_attached}', expected '${_expected}'. "
        "fontconfig must follow the Skia archive and sit inside --no-as-needed.")
endif()

# The wrapper the package config calls: Linux gets the library (or a warning when
# the dev package is absent); every other platform is untouched.
_run(wrapper "pulp_attach_fontconfig_to_canvas(Pulp::canvas)" _wrapped)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    string(FIND "${_wrapped}" "--no-as-needed" _at)
    if(_at LESS 0)
        find_library(_fc NAMES fontconfig)
        if(_fc)
            message(FATAL_ERROR "libfontconfig is installed at ${_fc} but the wrapper did not attach it: '${_wrapped}'")
        endif()
    endif()
else()
    if(NOT _wrapped STREQUAL "skia::skia")
        message(FATAL_ERROR "the wrapper changed the interface off Linux: '${_wrapped}'")
    endif()
endif()
message(STATUS "canvas fontconfig link: all cases passed")
