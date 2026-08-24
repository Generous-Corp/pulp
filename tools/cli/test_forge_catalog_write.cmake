if(NOT DEFINED PULP_CLI OR NOT DEFINED FIXTURE_ROOT)
    message(FATAL_ERROR "PULP_CLI and FIXTURE_ROOT are required")
endif()

file(REMOVE_RECURSE "${FIXTURE_ROOT}")
file(MAKE_DIRECTORY "${FIXTURE_ROOT}/core" "${FIXTURE_ROOT}/docs/status")
file(WRITE "${FIXTURE_ROOT}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.20)\n")

execute_process(
    COMMAND "${PULP_CLI}" forge catalog export --write
    WORKING_DIRECTORY "${FIXTURE_ROOT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "forge catalog write failed (${result}): ${output}${error}")
endif()

set(snapshot "${FIXTURE_ROOT}/docs/status/forge-catalog.json")
if(NOT EXISTS "${snapshot}")
    message(FATAL_ERROR "forge catalog write did not create ${snapshot}")
endif()
file(READ "${snapshot}" contents)
if(NOT contents MATCHES "pulp.forge-catalog.v1")
    message(FATAL_ERROR "forge catalog write produced an invalid snapshot")
endif()
