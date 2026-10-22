# Runs the editor probe on one built bundle and asserts that the format adapter
# built the plug-in's scripted editor: the adapter logs mode=scripted (and
# gpu=true when the build has the GPU editor host), and the script's own
# console line proves ui/main.js ran. An adapter that falls back to AutoUi logs
# mode=autoui and never prints the marker.
#
#   cmake -DPROBE=<exe> -DFORMAT=<vst3|clap|au> -DBUNDLE=<path> -DEXPECT_GPU=<bool>
#         -P check_editor_mode.cmake

foreach(_var PROBE FORMAT BUNDLE)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "${_var} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${PROBE}" "${FORMAT}" "${BUNDLE}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    TIMEOUT 90)
set(_log "${_out}${_err}")

if(NOT _result EQUAL 0)
    message(FATAL_ERROR "editor probe failed (${_result}) for ${FORMAT} ${BUNDLE}:\n${_log}")
endif()

string(REGEX MATCH "editor: [^\n]*mode=([a-z]+), gpu=([a-z]+)" _line "${_log}")
if(NOT _line)
    message(FATAL_ERROR "no adapter editor log line for ${FORMAT}:\n${_log}")
endif()
set(_mode "${CMAKE_MATCH_1}")
set(_gpu "${CMAKE_MATCH_2}")
if(NOT _mode STREQUAL "scripted")
    message(FATAL_ERROR
        "${FORMAT} adapter built mode=${_mode}, not the UI_SCRIPT editor:\n${_line}\n\n${_log}")
endif()
if(EXPECT_GPU AND NOT _gpu STREQUAL "true")
    message(FATAL_ERROR "${FORMAT} scripted editor is not on the GPU host:\n${_line}\n\n${_log}")
endif()
if(NOT _log MATCHES "script-ui\\[[a-z]+\\] pulp-ui-script-fixture-mounted")
    message(FATAL_ERROR "${FORMAT} editor never ran ui/main.js:\n${_log}")
endif()
message(STATUS "${FORMAT}: ${_line}")
