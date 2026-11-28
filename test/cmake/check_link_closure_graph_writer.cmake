# Builds the fixture project with the real graph writer, then runs the checker
# on each root. Every route to pulp-canvas must be reported (exit 1) and the
# clean root must pass (exit 0), so a writer or checker that stops seeing a
# route, or a checker that rejects everything, fails here.
# Inputs: -DSOURCE_DIR -DBINARY_DIR -DCHECKER -DPYTHON -DTEST_DIR
file(REMOVE_RECURSE "${BINARY_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${BINARY_DIR}"
            -DPULP_TEST_DIR=${TEST_DIR}
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "fixture configure failed\n${_out}\n${_err}")
endif()
set(_graph "${BINARY_DIR}/link-closure-graph.tsv")
if(NOT EXISTS "${_graph}")
    message(FATAL_ERROR "the graph writer produced no ${_graph}")
endif()
# The fixture graph is smaller than a real one; the walker's floor is for real graphs.
file(READ "${_graph}" _text)
foreach(_pad RANGE 1 25)
    string(APPEND _text "pulp-pad-${_pad}\tSTATIC_LIBRARY\t\t\t\n")
endforeach()
file(WRITE "${_graph}" "${_text}")

foreach(_root IN ITEMS route_name route_path route_objects route_imported)
    execute_process(
        COMMAND "${PYTHON}" "${CHECKER}" --graph "${_graph}" --root ${_root}
                --forbid "pulp::canvas"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 1 OR NOT _err MATCHES "reaches pulp-canvas")
        message(FATAL_ERROR "${_root} must be reported as reaching pulp-canvas "
                            "(rc=${_rc})\n${_out}\n${_err}")
    endif()
endforeach()
execute_process(
    COMMAND "${PYTHON}" "${CHECKER}" --graph "${_graph}" --root clean_root
            --forbid "pulp::canvas"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "clean_root must pass (rc=${_rc})\n${_out}\n${_err}")
endif()
message(STATUS "link-closure-graph-writer: 4 routes reported, control passes")
