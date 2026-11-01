# The WebView backend detector and its per-OS probes each have one owning
# target. Two archives defining detect_webview_backend() is an ODR violation
# the linker does not report: a binary linking both gets whichever archive it
# reads first, so a WebView build and a WebView-free build can disagree.
#
# Input: VIEW_TARGET_SOURCES, one "<target>=<source>|<source>..." entry per
# core/view target, entries separated by ",".

if(NOT DEFINED VIEW_TARGET_SOURCES OR VIEW_TARGET_SOURCES STREQUAL "")
    message(FATAL_ERROR "view-webview-detector-owner: VIEW_TARGET_SOURCES is empty; the instrument read nothing")
endif()

set(_owners_detector "")
set(_owners_probe "")
string(REPLACE "," ";" _entries "${VIEW_TARGET_SOURCES}")
foreach(_entry IN LISTS _entries)
    string(FIND "${_entry}" "=" _eq)
    string(SUBSTRING "${_entry}" 0 ${_eq} _target)
    math(EXPR _from "${_eq} + 1")
    string(SUBSTRING "${_entry}" ${_from} -1 _sources)
    string(REPLACE "|" ";" _sources "${_sources}")
    foreach(_source IN LISTS _sources)
        if(_source MATCHES "(^|/)src/web_view_backend\\.cpp$")
            list(APPEND _owners_detector "${_target}")
        elseif(_source MATCHES "(^|/)platform/(win/webview2|linux/webkitgtk)_backend\\.cpp$")
            list(APPEND _owners_probe "${_target}")
        endif()
    endforeach()
endforeach()

if(NOT _owners_detector STREQUAL "pulp-view-platform")
    message(FATAL_ERROR "view-webview-detector-owner: src/web_view_backend.cpp must be compiled by pulp-view-platform alone, found in: ${_owners_detector}")
endif()
foreach(_owner IN LISTS _owners_probe)
    if(NOT _owner STREQUAL "pulp-view-webview-backend")
        message(FATAL_ERROR "view-webview-detector-owner: the per-OS WebView probe belongs to pulp-view-webview-backend, also compiled by: ${_owners_probe}")
    endif()
endforeach()
list(LENGTH _owners_probe _probe_count)
if(_probe_count GREATER 1)
    message(FATAL_ERROR "view-webview-detector-owner: the per-OS WebView probe is compiled more than once: ${_owners_probe}")
endif()
message(STATUS "view-webview-detector-owner: PASS (detector: ${_owners_detector}; probe: ${_owners_probe})")
