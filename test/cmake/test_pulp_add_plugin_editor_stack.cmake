# pulp_add_plugin() picks a plug-in's editor stack: lite unless the plug-in
# declares UI_SCRIPT, GPU_UI or FULL_EDITOR. Each case configures a throwaway
# project against the public helper (with stand-in Pulp targets) and reads the
# recorded PULP_<target>_EDITOR_STACK; LITE_EDITOR combined with a keyword that
# needs the full stack must fail with the combination diagnostic.
if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and FIXTURE_DIR are required")
endif()
file(MAKE_DIRECTORY "${FIXTURE_DIR}")
set(_preamble
    "cmake_minimum_required(VERSION 3.24)\n"
    "project(EditorStack NONE)\n"
    "add_library(pulp-format INTERFACE)\n"
    "add_library(pulp::format ALIAS pulp-format)\n"
    "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpUtils.cmake\")\n")

function(_configure name call out_result out_text)
    set(_src "${FIXTURE_DIR}/${name}-src")
    set(_bin "${FIXTURE_DIR}/${name}-build")
    file(REMOVE_RECURSE "${_src}" "${_bin}")
    file(MAKE_DIRECTORY "${_src}")
    file(WRITE "${_src}/ui.js" "// stand-in scripted UI\n")
    file(WRITE "${_src}/CMakeLists.txt" ${_preamble} "${call}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_src}" -B "${_bin}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
    set(${out_result} "${_result}" PARENT_SCOPE)
    set(${out_text} "${_text}" PARENT_SCOPE)
endfunction()

function(_expect_stack name args expected)
    _configure(${name}
        "pulp_add_plugin(P${name} BUNDLE_ID dev.pulp.${name} ${args})\nmessage(STATUS \"editor-stack=\${PULP_P${name}_EDITOR_STACK}\")"
        _result _text)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${name}: configure failed:\n${_text}")
    endif()
    string(FIND "${_text}" "editor-stack=${expected} " _at)
    if(_at LESS 0)
        message(FATAL_ERROR "${name}: expected the ${expected} editor stack:\n${_text}")
    endif()
endfunction()

function(_expect_rejected name args needle)
    _configure(${name} "pulp_add_plugin(P${name} BUNDLE_ID dev.pulp.${name} ${args})" _result _text)
    if(_result EQUAL 0)
        message(FATAL_ERROR "${name}: configure succeeded; the conflict was ignored:\n${_text}")
    endif()
    string(FIND "${_text}" "${needle}" _at)
    if(_at LESS 0)
        message(FATAL_ERROR "${name}: failed without '${needle}':\n${_text}")
    endif()
endfunction()

_expect_stack(default "" lite)
_expect_stack(explicitlite "LITE_EDITOR" lite)
_expect_stack(gpuui "GPU_UI" full)
_expect_stack(fulleditor "FULL_EDITOR" full)
_expect_stack(scripted "UI_SCRIPT ui.js" full)
_expect_rejected(litegpu "LITE_EDITOR GPU_UI" "LITE_EDITOR cannot be combined with GPU_UI")
_expect_rejected(litescript "LITE_EDITOR UI_SCRIPT ui.js" "LITE_EDITOR cannot be combined with UI_SCRIPT")
message(STATUS "editor-stack selection: all cases passed")
