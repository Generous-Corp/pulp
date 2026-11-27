# Builds the fixture project with the real graph writer, then runs the checker
# on each root. Every route to pulp-canvas must be reported (exit 1) and the
# clean root must pass (exit 0), so a writer or checker that stops seeing a
# route, or a checker that rejects everything, fails here.
# Inputs: -DSOURCE_DIR -DBINARY_DIR -DCHECKER -DPYTHON -DTEST_DIR
file(REMOVE_RECURSE "${BINARY_DIR}")
file(MAKE_DIRECTORY "${BINARY_DIR}/.cmake/api/v1/query")
file(WRITE "${BINARY_DIR}/.cmake/api/v1/query/codemodel-v2" "")
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
    COMMAND "${PYTHON}" "${CHECKER}" --graph "${_graph}" --root route_scoped
            --forbid "pulp::canvas"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 1 OR NOT _err MATCHES "helper_import -> pulp-canvas")
    message(FATAL_ERROR "a directory-scoped imported target must be traversed "
                        "(rc=${_rc})\n${_out}\n${_err}")
endif()
execute_process(
    COMMAND "${PYTHON}" "${CHECKER}" --graph "${_graph}" --root clean_root
            --forbid "pulp::canvas"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "clean_root must pass (rc=${_rc})\n${_out}\n${_err}")
endif()

# An incomplete dump must never pass: drop the gpu_helper record, which route_name
# reaches by an ordinary (non-pulp, non-namespaced) target name.
file(READ "${_graph}" _full)
string(REGEX REPLACE "(^|\n)gpu_helper\t[^\n]*" "" _gutted "${_full}")
if(_gutted STREQUAL _full)
    message(FATAL_ERROR "the graph has no gpu_helper record to remove")
endif()
file(WRITE "${BINARY_DIR}/gutted.tsv" "${_gutted}")
execute_process(
    COMMAND "${PYTHON}" "${CHECKER}" --graph "${BINARY_DIR}/gutted.tsv" --root route_name
            --forbid "pulp::canvas"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 2 OR NOT _err MATCHES "gpu_helper")
    message(FATAL_ERROR "a missing intermediate record must be INCONCLUSIVE "
                        "(rc=${_rc})\n${_out}\n${_err}")
endif()
message(STATUS "link-closure-graph-writer: 4 routes reported, scoped import traversed, control passes, gutted graph inconclusive")
