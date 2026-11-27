# Runs the three slot probes and checks the layout contract:
#   LEGACY   built with PULP_FORMAT_LEGACY_VIEW=1 (the default SDK)
#   UI_FREE  built with PULP_FORMAT_LEGACY_VIEW=0 (an SDK whose UI is Vellum)
#   SHIFTED  built with =0 against a processor.hpp missing one legacy slot
# The first two must report the same slots and size; the third must not, which
# proves the probe can see a moved slot at all. The fingerprint must carry each
# build's mode.

foreach(_probe LEGACY UI_FREE SHIFTED)
    if(NOT DEFINED ${_probe} OR NOT EXISTS "${${_probe}}")
        message(FATAL_ERROR "slot probe ${_probe} not found: '${${_probe}}'")
    endif()
    execute_process(COMMAND "${${_probe}}" RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "slot probe ${_probe} exited ${_rc}: ${_err}")
    endif()
    string(REGEX MATCHALL "slot [^\n]+|size [^\n]+" _layout "${_out}")
    list(LENGTH _layout _lines)
    if(NOT _lines EQUAL 8)
        message(FATAL_ERROR "slot probe ${_probe} printed ${_lines} layout lines, expected 8:\n${_out}")
    endif()
    set(_layout_${_probe} "${_layout}")
    string(REGEX MATCH "abi_flags [^\n]*" _flags_${_probe} "${_out}")
    message(STATUS "${_probe}: ${_layout}")
endforeach()

if(NOT _layout_LEGACY STREQUAL _layout_UI_FREE)
    message(FATAL_ERROR "Processor's vtable layout differs between the two modes:\n"
        "  legacy:  ${_layout_LEGACY}\n  ui-free: ${_layout_UI_FREE}")
endif()
if(_layout_SHIFTED STREQUAL _layout_UI_FREE)
    message(FATAL_ERROR "control: removing a legacy slot did not move any probed slot, "
        "so the probe cannot see a layout change:\n  ${_layout_SHIFTED}")
endif()
if(NOT _flags_LEGACY MATCHES "legacyview=1" OR NOT _flags_UI_FREE MATCHES "legacyview=0")
    message(FATAL_ERROR "the build fingerprint does not carry the mode:\n"
        "  legacy:  ${_flags_LEGACY}\n  ui-free: ${_flags_UI_FREE}")
endif()
message(STATUS "Processor slots agree across modes; the shifted control moved them")
