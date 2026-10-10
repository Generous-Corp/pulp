# Configure-time boundary test for the generic view core.
# The target property and source include scan are intentionally independent
# controls: a clean link list cannot hide a forbidden header edge, and a clean
# source tree cannot hide a forbidden direct target edge.

if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED PULP_VIEW_CORE_LINKS)
    message(FATAL_ERROR "pulp-view-core-audio-free: missing test inputs")
endif()

set(_forbidden "(^|[|;])(pulp::)?(signal|audio|host|midi|format)([|;]|$)")
if(PULP_VIEW_CORE_LINKS MATCHES "${_forbidden}")
    message(FATAL_ERROR
        "pulp-view-core-audio-free: forbidden pulp-view-core LINK_LIBRARIES=${PULP_VIEW_CORE_LINKS}")
endif()

function(scan_forbidden_includes ROOT OUT_VAR)
    file(GLOB_RECURSE _files LIST_DIRECTORIES false
        "${ROOT}/*.h" "${ROOT}/*.hh" "${ROOT}/*.hpp"
        "${ROOT}/*.c" "${ROOT}/*.cc" "${ROOT}/*.cpp" "${ROOT}/*.cxx"
        "${ROOT}/*.m" "${ROOT}/*.mm")
    set(_bad)
    foreach(_file IN LISTS _files)
        if(_file MATCHES "/core/view/audio/")
            continue()
        endif()
        file(READ "${_file}" _contents)
        if(_contents MATCHES "#[ \t]*include[ \t]*<pulp/(audio|signal|host|midi|format)/")
            list(APPEND _bad "${_file}")
        endif()
    endforeach()
    set(${OUT_VAR} "${_bad}" PARENT_SCOPE)
endfunction()

scan_forbidden_includes("${PULP_SOURCE_DIR}/core/view" _bad_files)
if(_bad_files)
    string(REPLACE ";" "\n  " _bad_display "${_bad_files}")
    message(FATAL_ERROR
        "pulp-view-core-audio-free: forbidden core/view include(s):\n  ${_bad_display}")
endif()

# Negative control: prove the include instrument rejects a forbidden header.
set(_negative "${CMAKE_BINARY_DIR}/pulp-view-core-audio-free-negative.hpp")
file(WRITE "${_negative}" "#include <pulp/audio/buffer.hpp>\n")
scan_forbidden_includes("${CMAKE_BINARY_DIR}" _negative_bad)
file(REMOVE "${_negative}")
if(NOT _negative_bad)
    message(FATAL_ERROR
        "pulp-view-core-audio-free: negative include control was not detected")
endif()

message(STATUS "pulp-view-core-audio-free: PASS (LINK_LIBRARIES and include lint; negative control detected)")
