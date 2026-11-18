# Artifact half of the Linux optional-WebView contract: the plug-in's NEEDED
# list names no GTK, WebKit or JavaScriptCore library, and the control binary,
# which links pulp::view-webview, names them.
cmake_minimum_required(VERSION 3.24)

foreach(_required PLUGIN WEBVIEW_CONTROL)
    if(NOT ${_required})
        message(FATAL_ERROR "linux webview optional: ${_required} is required")
    endif()
endforeach()
find_program(_readelf readelf REQUIRED)

function(_needed out file)
    execute_process(COMMAND "${_readelf}" -d "${file}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _text ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "readelf -d ${file} failed (${_rc}): ${_err}")
    endif()
    string(REGEX MATCHALL "\\(NEEDED\\)[^\n]*" _lines "${_text}")
    set(${out} "${_lines}" PARENT_SCOPE)
endfunction()

set(_pattern "libgtk-|libgdk-|libwebkit2gtk|libjavascriptcoregtk|libsoup-")

_needed(_control_needed "${WEBVIEW_CONTROL}")
if(NOT _control_needed)
    message(FATAL_ERROR "the control has no NEEDED entries: the scanner read nothing")
endif()
set(_control_hits "")
foreach(_line IN LISTS _control_needed)
    if(_line MATCHES "${_pattern}")
        list(APPEND _control_hits "${_line}")
    endif()
endforeach()
if(NOT _control_hits)
    message(FATAL_ERROR
        "the positive control links pulp::view-webview but shows no GTK/WebKit NEEDED entry, so this scan is blind: ${_control_needed}")
endif()

_needed(_plugin_needed "${PLUGIN}")
if(NOT _plugin_needed)
    message(FATAL_ERROR "the plug-in has no NEEDED entries: the scanner read nothing")
endif()
foreach(_line IN LISTS _plugin_needed)
    if(_line MATCHES "${_pattern}")
        message(FATAL_ERROR
            "a plug-in that does not use the WebView needs a GTK/WebKit library: ${_line}")
    endif()
endforeach()
message(STATUS "plug-in NEEDED has no GTK/WebKit entry (${PLUGIN}); the control does: ${_control_hits}")
