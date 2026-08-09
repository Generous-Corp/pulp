cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and PULP_BUILD_DIR are required")
endif()
set(_root "${PULP_BUILD_DIR}/control-host-catalog-transaction")
set(_catalog "${_root}/catalog")
set(_source "${_root}/source-host")
set(_manifest "${_root}/source-manifest.json")
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_root}")
file(WRITE "${_source}" "host-v1")
file(WRITE "${_manifest}" "manifest-v1")
execute_process(COMMAND "${CMAKE_COMMAND}"
    -DPULP_CONTROL_HOST_ID=author-host
    -DPULP_CONTROL_HOST_SOURCE=${_source}
    -DPULP_CONTROL_HOST_MANIFEST=${_manifest}
    -DPULP_CONTROL_HOST_ROOT=${_catalog}
    -P "${PULP_SOURCE_DIR}/tools/cmake/install_control_host.cmake"
    RESULT_VARIABLE _install_result)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR "initial control host install failed")
endif()
file(WRITE "${_source}" "host-v2")
file(WRITE "${_manifest}" "manifest-v2")
execute_process(COMMAND "${CMAKE_COMMAND}"
    -DPULP_CONTROL_HOST_ID=author-host
    -DPULP_CONTROL_HOST_SOURCE=${_source}
    -DPULP_CONTROL_HOST_MANIFEST=${_manifest}
    -DPULP_CONTROL_HOST_ROOT=${_catalog}
    -DPULP_CONTROL_HOST_TEST_FAIL_BEFORE_PUBLISH=ON
    -P "${PULP_SOURCE_DIR}/tools/cmake/install_control_host.cmake"
    RESULT_VARIABLE _failed_update_result
    ERROR_QUIET)
if(_failed_update_result EQUAL 0)
    message(FATAL_ERROR "synthetic catalog publish failure unexpectedly succeeded")
endif()
file(READ "${_catalog}/author-host/host" _retained_host)
file(READ "${_catalog}/author-host/host.inspector-capabilities.json" _retained_manifest)
if(NOT _retained_host STREQUAL "host-v1" OR NOT _retained_manifest STREQUAL "manifest-v1")
    message(FATAL_ERROR "failed catalog update did not restore the exact prior pair")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}"
    -DPULP_CONTROL_HOST_ID=author-host
    -DPULP_CONTROL_HOST_ROOT=${_catalog}
    -P "${PULP_SOURCE_DIR}/tools/cmake/remove_control_host.cmake"
    RESULT_VARIABLE _remove_result)
if(NOT _remove_result EQUAL 0 OR EXISTS "${_catalog}/author-host")
    message(FATAL_ERROR "atomic catalog removal failed")
endif()
