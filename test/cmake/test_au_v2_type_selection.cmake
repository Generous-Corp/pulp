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
else()
    message(STATUS "AU v2 component-type selection: all cases pass.")
endif()
