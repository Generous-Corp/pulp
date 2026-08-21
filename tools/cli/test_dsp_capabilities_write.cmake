if(NOT DEFINED PULP_CLI OR NOT DEFINED SCRIPT_SOURCE OR NOT DEFINED FIXTURE_ROOT)
    message(FATAL_ERROR "PULP_CLI, SCRIPT_SOURCE, and FIXTURE_ROOT are required")
endif()

file(REMOVE_RECURSE "${FIXTURE_ROOT}")
file(MAKE_DIRECTORY
    "${FIXTURE_ROOT}/core/host/include/pulp/host"
    "${FIXTURE_ROOT}/docs/status"
    "${FIXTURE_ROOT}/tools/scripts")
file(WRITE "${FIXTURE_ROOT}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.20)\n")
file(COPY "${SCRIPT_SOURCE}" DESTINATION "${FIXTURE_ROOT}/tools/scripts")
file(WRITE "${FIXTURE_ROOT}/core/host/include/pulp/host/forge_fixture_catalog.hpp" [=[
inline constexpr auto kFixtureTypeId = "fixture";
inline CustomNodeType make_fixture() {
    CustomNodeType node;
    node.baked_params.push_back({kDriveParam, 0.0, 1.0, 0.5});
    return node;
}
]=])

execute_process(
    COMMAND "${PULP_CLI}" dsp capabilities --write
    WORKING_DIRECTORY "${FIXTURE_ROOT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "DSP capability write failed (${result}): ${output}${error}")
endif()

set(snapshot "${FIXTURE_ROOT}/docs/status/dsp-capabilities.json")
if(NOT EXISTS "${snapshot}")
    message(FATAL_ERROR "DSP capability write did not create ${snapshot}")
endif()
file(READ "${snapshot}" contents)
if(NOT contents MATCHES "forge_fixture_catalog.hpp")
    message(FATAL_ERROR "DSP capability write produced an invalid fixture snapshot")
endif()
