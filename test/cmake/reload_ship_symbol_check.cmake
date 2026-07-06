# Ship-safety symbol-absence check (live-swap item 1.12). nm the two fixture
# binaries: the dev build must contain the reload watcher symbol; the gate-off
# ship build must NOT. A positive+negative control so the marker can't silently
# rot into a vacuous pass.
find_program(NM_EXE nm)
if(NOT NM_EXE)
    message(WARNING "nm not found; skipping ship-safety symbol check")
    return()
endif()

execute_process(COMMAND ${NM_EXE} "${DEV_BIN}" OUTPUT_VARIABLE dev_syms
                RESULT_VARIABLE dev_rc ERROR_VARIABLE dev_err)
execute_process(COMMAND ${NM_EXE} "${SHIP_BIN}" OUTPUT_VARIABLE ship_syms
                RESULT_VARIABLE ship_rc ERROR_VARIABLE ship_err)
if(NOT dev_rc EQUAL 0 OR NOT ship_rc EQUAL 0)
    message(FATAL_ERROR "nm failed: dev='${dev_err}' ship='${ship_err}'")
endif()

string(FIND "${dev_syms}" "watcher_loop" dev_has)
string(FIND "${ship_syms}" "watcher_loop" ship_has)
if(dev_has EQUAL -1)
    message(FATAL_ERROR "watcher_loop MISSING from the dev fixture — the marker is broken, the test would be vacuous")
endif()
if(NOT ship_has EQUAL -1)
    message(FATAL_ERROR "watcher_loop PRESENT in the ship (gate-off) fixture — the dev watcher was NOT compiled out of a shipping build")
endif()
message(STATUS "ship-safety: reload watcher present in dev, absent in ship — OK")
