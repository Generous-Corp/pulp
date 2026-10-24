# Ship-safety check for the shell exec/execAsync JS bridge. DEV_OBJ and SHIP_OBJ
# are the same translation unit (widget_bridge/platform_services_api.cpp)
# compiled with PULP_BRIDGE_EXEC_ENABLED=1 and =0. The dev object must define
# the registrar and carry the "execAsync" literal — a positive control that
# keeps the check from rotting into a vacuous pass — and the ship object must
# carry neither, proving the /bin/sh body is compiled out rather than merely
# unreferenced.
find_program(NM_EXE nm)
if(NOT NM_EXE)
    message(WARNING "nm not found; skipping bridge exec ship-safety check")
    return()
endif()
if(NOT EXISTS "${DEV_OBJ}" OR NOT EXISTS "${SHIP_OBJ}")
    message(FATAL_ERROR "fixture objects missing: dev='${DEV_OBJ}' ship='${SHIP_OBJ}'")
endif()

set(_symbol "register_platform_services_exec_api")
set(_literal "execAsync")

execute_process(COMMAND ${NM_EXE} "${DEV_OBJ}" OUTPUT_VARIABLE dev_syms
                RESULT_VARIABLE dev_rc ERROR_VARIABLE dev_err)
execute_process(COMMAND ${NM_EXE} "${SHIP_OBJ}" OUTPUT_VARIABLE ship_syms
                RESULT_VARIABLE ship_rc ERROR_VARIABLE ship_err)
if(NOT dev_rc EQUAL 0 OR NOT ship_rc EQUAL 0)
    message(FATAL_ERROR "nm failed: dev='${dev_err}' ship='${ship_err}'")
endif()

# file(STRINGS) extracts the printable runs of a binary, which is where a
# string literal lands; a plain file(READ) would stop at the first NUL.
file(STRINGS "${DEV_OBJ}" dev_strings REGEX "${_literal}")
file(STRINGS "${SHIP_OBJ}" ship_strings REGEX "${_literal}")

string(FIND "${dev_syms}" "${_symbol}" dev_has_symbol)
string(FIND "${ship_syms}" "${_symbol}" ship_has_symbol)
if(dev_has_symbol EQUAL -1)
    message(FATAL_ERROR
        "${_symbol} MISSING from the dev fixture — the positive control is broken, "
        "the check would be vacuous")
endif()
if(dev_strings STREQUAL "")
    message(FATAL_ERROR
        "\"${_literal}\" MISSING from the dev fixture — the positive control is broken, "
        "the check would be vacuous")
endif()
if(NOT ship_has_symbol EQUAL -1)
    message(FATAL_ERROR
        "${_symbol} PRESENT in the ship (gate-off) fixture — the shell exec bridge "
        "was NOT compiled out of a shipping build")
endif()
if(NOT ship_strings STREQUAL "")
    message(FATAL_ERROR
        "\"${_literal}\" PRESENT in the ship (gate-off) fixture — the shell exec bridge "
        "body was NOT compiled out of a shipping build")
endif()
message(STATUS "ship-safety: shell exec bridge present in dev, absent in ship — OK")
