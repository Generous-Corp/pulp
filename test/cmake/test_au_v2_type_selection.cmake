#[[
Pure-logic smoke test for the AU v2 component-type selection that lives
inside `_pulp_add_au()` in tools/cmake/PulpUtils.cmake. Run under `cmake -P`
so it doesn't need a real Pulp configure — that full tree is too heavy to
spin up inside ctest for a five-line branch.

Mirrors the selector exactly. If the implementation in PulpUtils.cmake
changes, update this fixture in the same commit — the test is the
specification for which component type each (category, accepts_midi)
pair emits.

Expected mapping:
  (Instrument,  *)     -> aumu
  (MidiEffect,  *)     -> aumi
  (Effect,      true)  -> aumf   <-- the fix
  (Effect,      false) -> aufx

Also pins the public CMake-option contract: `ACCEPTS_MIDI` is the only
pulp_add_plugin MIDI packaging option. MIDI output remains
PluginDescriptor/format metadata, not a parallel `PRODUCES_MIDI` CMake flag.
]]

function(_select_au_type out_var category accepts_midi)
    if("${category}" STREQUAL "Instrument")
        set(${out_var} "aumu" PARENT_SCOPE)
    elseif("${category}" STREQUAL "MidiEffect")
        set(${out_var} "aumi" PARENT_SCOPE)
    elseif(accepts_midi)
        set(${out_var} "aumf" PARENT_SCOPE)
    else()
        set(${out_var} "aufx" PARENT_SCOPE)
    endif()
endfunction()

set(_fail 0)

_select_au_type(t "Instrument" 1)
if(NOT t STREQUAL "aumu")
    message("FAIL: Instrument + accepts_midi=1 -> expected aumu, got ${t}")
    set(_fail 1)
endif()

_select_au_type(t "Instrument" 0)
if(NOT t STREQUAL "aumu")
    message("FAIL: Instrument + accepts_midi=0 -> expected aumu, got ${t}")
    set(_fail 1)
endif()

_select_au_type(t "MidiEffect" 0)
if(NOT t STREQUAL "aumi")
    message("FAIL: MidiEffect -> expected aumi, got ${t}")
    set(_fail 1)
endif()

_select_au_type(t "Effect" 1)
if(NOT t STREQUAL "aumf")
    message("FAIL: Effect + accepts_midi=1 -> expected aumf, got ${t}")
    set(_fail 1)
endif()

_select_au_type(t "Effect" 0)
if(NOT t STREQUAL "aufx")
    message("FAIL: Effect + accepts_midi=0 -> expected aufx, got ${t}")
    set(_fail 1)
endif()

# Empty `accepts_midi` must behave like falsy.
_select_au_type(t "Effect" "")
if(NOT t STREQUAL "aufx")
    message("FAIL: Effect + accepts_midi=empty -> expected aufx, got ${t}")
    set(_fail 1)
endif()

if(_fail)
    message(FATAL_ERROR "AU v2 component-type selection smoke failed.")
endif()

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_utils "${_repo_root}/tools/cmake/PulpUtils.cmake")
set(_reference "${_repo_root}/docs/reference/cmake.md")
set(_status "${_repo_root}/docs/status/cmake-functions.yaml")

foreach(_path IN LISTS _utils _reference _status)
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "Required file missing: ${_path}")
    endif()
endforeach()

file(READ "${_utils}" _utils_text)
string(REGEX MATCH "cmake_parse_arguments\\(PLUGIN[ \t\r\n]+\"([^\"]*)\"" _parse_match "${_utils_text}")
if(NOT _parse_match)
    message(FATAL_ERROR "Could not find pulp_add_plugin cmake_parse_arguments option list")
endif()

set(_plugin_options "${CMAKE_MATCH_1}")
if(NOT _plugin_options MATCHES "(^|;)ACCEPTS_MIDI($|;)")
    message(FATAL_ERROR "pulp_add_plugin must parse ACCEPTS_MIDI")
endif()
if(_plugin_options MATCHES "(^|;)PRODUCES_MIDI($|;)")
    message(FATAL_ERROR "pulp_add_plugin must not parse inert PRODUCES_MIDI")
endif()
if(NOT _utils_text MATCHES "PRODUCES_MIDI is ignored")
    message(FATAL_ERROR "pulp_add_plugin must warn when callers pass inert PRODUCES_MIDI")
endif()

file(READ "${_reference}" _reference_text)
if(NOT _reference_text MATCHES "`ACCEPTS_MIDI`")
    message(FATAL_ERROR "docs/reference/cmake.md must document ACCEPTS_MIDI")
endif()
if(_reference_text MATCHES "\\|[ \t]*`PRODUCES_MIDI`")
    message(FATAL_ERROR "docs/reference/cmake.md must not document PRODUCES_MIDI as a CMake option")
endif()

file(READ "${_status}" _status_text)
if(NOT _status_text MATCHES "[ \t-]ACCEPTS_MIDI")
    message(FATAL_ERROR "docs/status/cmake-functions.yaml must list ACCEPTS_MIDI")
endif()
if(_status_text MATCHES "[ \t-]PRODUCES_MIDI")
    message(FATAL_ERROR "docs/status/cmake-functions.yaml must not list PRODUCES_MIDI")
endif()

message(STATUS "AU v2 type selection and pulp_add_plugin MIDI-option contract: OK")
