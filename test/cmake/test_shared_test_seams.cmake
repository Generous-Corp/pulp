# Contract for the shared test-seam and processor-target surfaces.
cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR is required")
endif()

set(_env_header "${PULP_SOURCE_DIR}/core/format/include/pulp/format/test_env.hpp")
if(NOT EXISTS "${_env_header}")
    message(FATAL_ERROR "PULP_TEST_ENV header is missing: ${_env_header}")
endif()
file(READ "${_env_header}" _env)
foreach(_needle
        "#define PULP_TEST_ENV(name)"
        "PULP_ENABLE_TEST_SEAMS"
        "std::getenv(name)")
    string(FIND "${_env}" "${_needle}" _found)
    if(_found EQUAL -1)
        message(FATAL_ERROR "PULP_TEST_ENV header lacks '${_needle}'")
    endif()
endforeach()

file(READ "${PULP_SOURCE_DIR}/tools/cmake/PulpPlugin.cmake" _plugin)
foreach(_needle
        "::Processor"
        "target_compile_definitions(\${target}_Core PUBLIC"
        "add_library(\${target}::Processor ALIAS")
    string(FIND "${_plugin}" "${_needle}" _found)
    if(_found EQUAL -1)
        message(FATAL_ERROR "pulp_add_plugin lacks processor target contract '${_needle}'")
    endif()
endforeach()

file(READ "${PULP_SOURCE_DIR}/tools/cmake/release_artifact_forbidden.json" _forbidden)
if(NOT _forbidden MATCHES "test-seam\\.forge-modular" OR
   NOT _forbidden MATCHES "test-seam\\.super-convolver")
    message(FATAL_ERROR "forbidden-marker list does not register product test-seam prefixes")
endif()

message(STATUS "shared test-seam contract OK")
