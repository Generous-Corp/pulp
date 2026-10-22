# Dev-environment gate check. DEV_OBJ and SHIP_OBJ are the same translation
# unit (test/fixtures/dev_env_gate_fixture.cpp, which includes view_bridge.hpp
# and reloadable_shell.hpp) compiled with PULP_DEV_BUILD=1 and =0. The dev
# object must carry both environment-variable literals — a positive control
# that keeps the check from rotting into a vacuous pass — and the ship object
# must carry neither, proving the getenv reads are compiled out rather than
# merely returning early.
# $<TARGET_OBJECTS:...> lists every object of the fixture target, and linking
# pulp::format adds its interface sources to that list; only the fixture's own
# translation unit is under test.
foreach(_which DEV SHIP)
    set(_objects "${${_which}_OBJ}")
    list(FILTER _objects INCLUDE REGEX "dev_env_gate_fixture\\.cpp\\.o$")
    list(LENGTH _objects _count)
    if(NOT _count EQUAL 1 OR NOT EXISTS "${_objects}")
        message(FATAL_ERROR "${_which} fixture object missing or ambiguous in '${${_which}_OBJ}'")
    endif()
    set(${_which}_OBJ "${_objects}")
endforeach()

# file(STRINGS) extracts the printable runs of a binary, which is where a
# string literal lands; a plain file(READ) would stop at the first NUL.
foreach(_literal PULP_DEV_HOT_RELOAD PULP_RELOAD_LOGIC_PATH)
    file(STRINGS "${DEV_OBJ}" dev_strings REGEX "${_literal}")
    file(STRINGS "${SHIP_OBJ}" ship_strings REGEX "${_literal}")
    if(dev_strings STREQUAL "")
        message(FATAL_ERROR
            "\"${_literal}\" MISSING from the dev fixture — the positive control is broken, "
            "the check would be vacuous")
    endif()
    if(NOT ship_strings STREQUAL "")
        message(FATAL_ERROR
            "\"${_literal}\" PRESENT in the ship (PULP_DEV_BUILD=0) fixture — the "
            "environment read was NOT compiled out of a release build")
    endif()
endforeach()
message(STATUS "dev gate: PULP_DEV_HOT_RELOAD + PULP_RELOAD_LOGIC_PATH read in dev, absent in release — OK")
