# Release-arm environment-literal check for the macOS view hosts and the SKP
# capture. Each *_DEV / *_SHIP pair is one translation unit compiled with
# PULP_DEV_BUILD=1 and =0 (test/cmake/format_reload_tests.cmake). The =1 object
# must carry every environment-variable literal its source reads — the positive
# control — and the =0 object none of them, proving the reads are compiled out.
# The SKP pair is optional: a non-GPU configure has no skp_capture.cpp object.
function(_pick_object var pattern)
    set(_objects "${${var}}")
    list(FILTER _objects INCLUDE REGEX "${pattern}")
    list(LENGTH _objects _count)
    if(NOT _count EQUAL 1 OR NOT EXISTS "${_objects}")
        message(FATAL_ERROR "${var}: fixture object missing or ambiguous in '${${var}}'")
    endif()
    set(${var} "${_objects}" PARENT_SCOPE)
endfunction()

# file(STRINGS) extracts the printable runs of a binary, which is where a
# string literal lands; a plain file(READ) would stop at the first NUL.
function(_check_pair label dev ship)
    foreach(_literal IN LISTS ARGN)
        file(STRINGS "${dev}" _dev_strings REGEX "${_literal}")
        file(STRINGS "${ship}" _ship_strings REGEX "${_literal}")
        if(_dev_strings STREQUAL "")
            message(FATAL_ERROR
                "${label}: \"${_literal}\" MISSING from the PULP_DEV_BUILD=1 object — the "
                "positive control is broken, the check would be vacuous")
        endif()
        if(NOT _ship_strings STREQUAL "")
            message(FATAL_ERROR
                "${label}: \"${_literal}\" PRESENT in the PULP_DEV_BUILD=0 object — the "
                "environment read was NOT compiled out of a release build")
        endif()
    endforeach()
    message(STATUS "release arm: ${label} reads ${ARGN} at 1, none at 0 — OK")
endfunction()

_pick_object(WINDOW_HOST_DEV "window_host_mac\\.mm\\.o$")
_pick_object(WINDOW_HOST_SHIP "window_host_mac\\.mm\\.o$")
_check_pair(window_host_mac.mm "${WINDOW_HOST_DEV}" "${WINDOW_HOST_SHIP}"
    PULP_TEST_POINTER_DRAG PULP_PARTIAL_RENDERING_DEBUG PULP_PARTIAL_REPAINT)

_pick_object(PLUGIN_VIEW_HOST_DEV "plugin_view_host_mac\\.mm\\.o$")
_pick_object(PLUGIN_VIEW_HOST_SHIP "plugin_view_host_mac\\.mm\\.o$")
_check_pair(plugin_view_host_mac.mm "${PLUGIN_VIEW_HOST_DEV}" "${PLUGIN_VIEW_HOST_SHIP}"
    PULP_EMBED_GPU_FRAME_STAT)

if(DEFINED SKP_CAPTURE_DEV AND DEFINED SKP_CAPTURE_SHIP)
    _pick_object(SKP_CAPTURE_DEV "skp_capture\\.cpp\\.o$")
    _pick_object(SKP_CAPTURE_SHIP "skp_capture\\.cpp\\.o$")
    _check_pair(skp_capture.cpp "${SKP_CAPTURE_DEV}" "${SKP_CAPTURE_SHIP}" PULP_SKP_CAPTURE_DIR)
else()
    message(STATUS "release arm: skp_capture.cpp SKIPPED (no GPU configure; pulp-render absent)")
endif()
