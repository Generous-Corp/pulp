# Configure-time boundary test for the generic view core.
# The target property and source include scan are intentionally independent
# controls: a clean link list cannot hide a forbidden header edge, and a clean
# source tree cannot hide a forbidden direct target edge.

foreach(_input PULP_SOURCE_DIR PULP_VIEW_CORE_LINKS PULP_VIEW_LINKS
        PULP_VIEW_NATIVE_LINKS PULP_VIEW_AUDIO_LINKS)
    if(NOT DEFINED ${_input})
        message(FATAL_ERROR "pulp-view-core-audio-free: missing test input ${_input}")
    endif()
endforeach()

function(link_matches LINKS NAMES OUT_VAR)
    if(LINKS MATCHES "(^|[|;])(pulp::|pulp-)?(${NAMES})([|;]|$)")
        set(${OUT_VAR} TRUE PARENT_SCOPE)
    else()
        set(${OUT_VAR} FALSE PARENT_SCOPE)
    endif()
endfunction()

# The link instrument must see each edge it guards; prove it on synthetic lists
# shaped like CMake's (aliased and plain names), so a regex that never matches
# cannot pass the checks below.
foreach(_control "pulp::canvas|pulp::audio" "pulp-host" "pulp::view-graph-editor")
    link_matches("${_control}" "audio|host|view-graph-editor" _hit)
    if(NOT _hit)
        message(FATAL_ERROR
            "pulp-view-core-audio-free: link negative control '${_control}' was not detected")
    endif()
endforeach()
link_matches("pulp::view-audio" "audio" _hit)
if(_hit)
    message(FATAL_ERROR
        "pulp-view-core-audio-free: link control matched view-audio as audio")
endif()

link_matches("${PULP_VIEW_CORE_LINKS}" "signal|audio|host|midi|format|view-audio|view-graph-editor" _hit)
if(_hit)
    message(FATAL_ERROR
        "pulp-view-core-audio-free: forbidden pulp-view-core LINK_LIBRARIES=${PULP_VIEW_CORE_LINKS}")
endif()

# The audio widgets reach audio/signal but never the plug-in host stack.
link_matches("${PULP_VIEW_AUDIO_LINKS}" "host|format|view-graph-editor" _hit)
if(_hit)
    message(FATAL_ERROR
        "pulp-view-core-audio-free: pulp-view-audio reaches the host, LINK_LIBRARIES=${PULP_VIEW_AUDIO_LINKS}")
endif()

# Both umbrellas carry the audio widgets and never the host-linked graph editor.
foreach(_umbrella VIEW VIEW_NATIVE)
    set(_links "${PULP_${_umbrella}_LINKS}")
    link_matches("${_links}" "view-audio" _has_audio)
    if(NOT _has_audio)
        message(FATAL_ERROR
            "pulp-view-core-audio-free: ${_umbrella} umbrella does not link pulp::view-audio, LINK_LIBRARIES=${_links}")
    endif()
    link_matches("${_links}" "host|view-graph-editor" _hit)
    if(_hit)
        message(FATAL_ERROR
            "pulp-view-core-audio-free: ${_umbrella} umbrella links the host stack, LINK_LIBRARIES=${_links}")
    endif()
endforeach()

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

message(STATUS "pulp-view-core-audio-free: PASS (view-core, view-audio and umbrella LINK_LIBRARIES; include lint; negative controls detected)")
