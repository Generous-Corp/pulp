# Records every dependency a configure REQUESTS, so a build can prove it never
# asked for a dependency it does not use. A download log cannot: a dependency
# already in a machine-wide FetchContent cache is never downloaded, so "nothing
# was downloaded" means either "not needed" or "already cached".
#
# Inject it at the first project() call and name the log:
#
#   cmake -S . -B <dir> \
#     -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=<repo>/tools/ci/dependency_request_recorder.cmake \
#     -DDEPENDENCY_REQUEST_LOG=<dir>/dependency-requests.jsonl
#
# Each request appends one JSON line, {"method": ..., "name": ...}:
#
# - FIND_PACKAGE and FETCHCONTENT_MAKEAVAILABLE_SERIAL: every find_package()
#   and FetchContent_MakeAvailable() that reaches the dependency provider. The
#   recorder fulfils nothing, so CMake's own handling runs exactly as it would
#   without it.
# - FETCHCONTENT_SOURCE_DIR: CMake skips the provider for a dependency whose
#   FETCHCONTENT_SOURCE_DIR_<NAME> cache entry is set (the usual way a shared
#   source cache is wired in), so at the end of the configure the recorder logs
#   every such dependency that was actually populated.
# - Anything a project fetches by other means (a raw CMake download, a provisioning
#   script) is logged only where the project calls
#   dependency_request_recorder_note(<method> <name>) at that point. Guard the
#   call with if(COMMAND dependency_request_recorder_note), because the command
#   exists only while the recorder is injected.
#
# The companion dependency_request_check.py reads the log. A fetch path that
# makes no note stays invisible, so a job that must prove a dependency absent
# also needs a positive control that the same log does show it.
#
# Pulp and Vellum carry byte-identical copies of this file, kept equal by hash,
# so change both together.

cmake_minimum_required(VERSION 3.24)

if(NOT DEPENDENCY_REQUEST_LOG)
    if(DEFINED ENV{DEPENDENCY_REQUEST_LOG} AND NOT "$ENV{DEPENDENCY_REQUEST_LOG}" STREQUAL "")
        set(DEPENDENCY_REQUEST_LOG "$ENV{DEPENDENCY_REQUEST_LOG}")
    else()
        set(DEPENDENCY_REQUEST_LOG "${CMAKE_BINARY_DIR}/dependency-requests.jsonl")
    endif()
endif()
set(DEPENDENCY_REQUEST_LOG "${DEPENDENCY_REQUEST_LOG}"
    CACHE FILEPATH "Where the dependency request recorder writes its JSON lines" FORCE)

# A fresh log per configure: a re-run must not inherit an earlier run's
# requests.
file(WRITE "${DEPENDENCY_REQUEST_LOG}" "")

function(_pulp_dependency_request_recorder method name)
    string(REPLACE "\\" "\\\\" _escaped "${name}")
    string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
    file(APPEND "${DEPENDENCY_REQUEST_LOG}"
        "{\"method\": \"${method}\", \"name\": \"${_escaped}\"}\n")
    # Fulfilling nothing hands the request back to CMake's default handling.
endfunction()

function(dependency_request_recorder_note method name)
    _pulp_dependency_request_recorder("${method}" "${name}")
endfunction()

# Runs once the top-level directory has been processed, after every
# FetchContent_MakeAvailable() in the project has run.
function(_pulp_dependency_request_recorder_source_dirs)
    include(FetchContent)
    get_cmake_property(_cache_names CACHE_VARIABLES)
    list(SORT _cache_names)
    foreach(_cache_name IN LISTS _cache_names)
        if(NOT _cache_name MATCHES "^FETCHCONTENT_SOURCE_DIR_(.+)$")
            continue()
        endif()
        if("${${_cache_name}}" STREQUAL "")
            continue()
        endif()
        string(TOLOWER "${CMAKE_MATCH_1}" _content)
        FetchContent_GetProperties(${_content})
        if(${_content}_POPULATED)
            _pulp_dependency_request_recorder(FETCHCONTENT_SOURCE_DIR "${_content}")
        endif()
    endforeach()
endfunction()

cmake_language(SET_DEPENDENCY_PROVIDER _pulp_dependency_request_recorder
    SUPPORTED_METHODS FIND_PACKAGE FETCHCONTENT_MAKEAVAILABLE_SERIAL)
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
    CALL _pulp_dependency_request_recorder_source_dirs)
