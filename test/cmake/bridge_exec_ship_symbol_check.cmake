# Ship-safety check for the shell exec/execAsync JS bridge. DEV_OBJ and SHIP_OBJ
# are the same translation unit (widget_bridge/platform_services_api.cpp)
# compiled with PULP_BRIDGE_EXEC_ENABLED=1 and =0. The verdict comes from the
# object's symbol table alone: the dev object must name the registrar — a
# positive control that keeps the check from rotting into a vacuous pass — and
# the ship object must not, proving the /bin/sh body is compiled out rather than
# merely unreferenced. String literals are not inspected: whether one survives
# in an object depends on the compiler's constant pooling, not on the gate.
find_program(NM_EXE nm)
if(NOT NM_EXE)
    message(FATAL_ERROR "nm not found; the bridge exec ship-safety check cannot run")
endif()
if(NOT EXISTS "${DEV_OBJ}" OR NOT EXISTS "${SHIP_OBJ}")
    message(FATAL_ERROR "fixture objects missing: dev='${DEV_OBJ}' ship='${SHIP_OBJ}'")
endif()

set(_symbol "register_platform_services_exec_api")

execute_process(COMMAND ${NM_EXE} "${DEV_OBJ}" OUTPUT_VARIABLE dev_syms
                RESULT_VARIABLE dev_rc ERROR_VARIABLE dev_err)
execute_process(COMMAND ${NM_EXE} "${SHIP_OBJ}" OUTPUT_VARIABLE ship_syms
                RESULT_VARIABLE ship_rc ERROR_VARIABLE ship_err)
if(NOT dev_rc EQUAL 0 OR NOT ship_rc EQUAL 0)
    message(FATAL_ERROR "nm failed: dev='${dev_err}' ship='${ship_err}'")
endif()

string(FIND "${dev_syms}" "${_symbol}" dev_has_symbol)
string(FIND "${ship_syms}" "${_symbol}" ship_has_symbol)
if(dev_has_symbol EQUAL -1)
    message(FATAL_ERROR
        "${_symbol} MISSING from the dev fixture — the positive control is broken, "
        "the check would be vacuous")
endif()
if(NOT ship_has_symbol EQUAL -1)
    message(FATAL_ERROR
        "${_symbol} PRESENT in the ship (gate-off) fixture — the shell exec bridge "
        "was NOT compiled out of a shipping build")
endif()
message(STATUS "ship-safety: shell exec bridge present in dev, absent in ship — OK")
