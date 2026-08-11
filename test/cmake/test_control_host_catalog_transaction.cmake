cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and PULP_BUILD_DIR are required")
endif()
set(_root "${PULP_BUILD_DIR}/control-host-catalog-transaction")
set(_catalog "${_root}/catalog")
set(_source "${_root}/source-host")
set(_manifest "${_root}/source-manifest.json")
set(_runtime "${_root}/runtime")
if(WIN32)
    set(_runtime_fixture "fixture.dll")
elseif(APPLE)
    set(_runtime_fixture "fixture.dylib")
else()
    set(_runtime_fixture "fixture.so")
endif()
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_root}" "${_runtime}")
file(COPY_FILE "/usr/bin/true" "${_source}")
file(SHA256 "${_source}" _host_v1_digest)
file(WRITE "${_manifest}" "manifest-v1")
file(WRITE "${_runtime}/${_runtime_fixture}" "runtime-v1")
execute_process(COMMAND "${CMAKE_COMMAND}"
    -DPULP_CONTROL_HOST_ID=author-host
    -DPULP_CONTROL_HOST_SOURCE=${_source}
    -DPULP_CONTROL_HOST_MANIFEST=${_manifest}
    -DPULP_CONTROL_HOST_ROOT=${_catalog}
    -DPULP_CONTROL_HOST_RUNTIME_DIR=${_runtime}
    -P "${PULP_SOURCE_DIR}/tools/cmake/install_control_host.cmake"
    RESULT_VARIABLE _install_result)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR "initial control host install failed")
endif()
file(COPY_FILE "/usr/bin/false" "${_source}")
file(SHA256 "${_source}" _host_v2_digest)
file(WRITE "${_manifest}" "manifest-v2")
file(WRITE "${_runtime}/${_runtime_fixture}" "runtime-v2")
execute_process(COMMAND "${CMAKE_COMMAND}"
    -DPULP_CONTROL_HOST_ID=author-host
    -DPULP_CONTROL_HOST_SOURCE=${_source}
    -DPULP_CONTROL_HOST_MANIFEST=${_manifest}
    -DPULP_CONTROL_HOST_ROOT=${_catalog}
    -DPULP_CONTROL_HOST_RUNTIME_DIR=${_runtime}
    -DPULP_CONTROL_HOST_TEST_FAIL_BEFORE_PUBLISH=ON
    -P "${PULP_SOURCE_DIR}/tools/cmake/install_control_host.cmake"
    RESULT_VARIABLE _failed_update_result
    ERROR_QUIET)
if(_failed_update_result EQUAL 0)
    message(FATAL_ERROR "synthetic catalog publish failure unexpectedly succeeded")
endif()
file(READ "${_catalog}/author-host/active" _retained_version)
string(STRIP "${_retained_version}" _retained_version)
file(SHA256 "${_catalog}/author-host/${_retained_version}/host" _retained_host_digest)
file(READ "${_catalog}/author-host/${_retained_version}/host.inspector-capabilities.json"
    _retained_manifest)
if(NOT _retained_host_digest STREQUAL _host_v1_digest OR
   NOT _retained_manifest STREQUAL "manifest-v1")
    message(FATAL_ERROR "failed catalog update did not restore the exact prior pair")
endif()
file(READ "${_catalog}/author-host/${_retained_version}/${_runtime_fixture}" _retained_runtime)
if(NOT _retained_runtime STREQUAL "runtime-v1")
    message(FATAL_ERROR "failed catalog update did not retain the prior runtime closure")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}"
    -DPULP_CONTROL_HOST_ID=author-host
    -DPULP_CONTROL_HOST_SOURCE=${_source}
    -DPULP_CONTROL_HOST_MANIFEST=${_manifest}
    -DPULP_CONTROL_HOST_ROOT=${_catalog}
    -DPULP_CONTROL_HOST_RUNTIME_DIR=${_runtime}
    -P "${PULP_SOURCE_DIR}/tools/cmake/install_control_host.cmake"
    RESULT_VARIABLE _update_result)
if(NOT _update_result EQUAL 0)
    message(FATAL_ERROR "successful catalog update failed")
endif()
file(READ "${_catalog}/author-host/active" _updated_version)
string(STRIP "${_updated_version}" _updated_version)
if(_updated_version STREQUAL _retained_version OR
   NOT EXISTS "${_catalog}/author-host/${_retained_version}/host")
    message(FATAL_ERROR "catalog update was not an atomic immutable version switch")
endif()
file(SHA256 "${_catalog}/author-host/${_updated_version}/host" _updated_host_digest)
file(READ "${_catalog}/author-host/${_updated_version}/${_runtime_fixture}" _updated_runtime)
if(NOT _updated_host_digest STREQUAL _host_v2_digest OR
   NOT _updated_runtime STREQUAL "runtime-v2")
    message(FATAL_ERROR "catalog update did not publish the exact host/runtime closure")
endif()
file(APPEND "${_catalog}/author-host/${_retained_version}/host" "corrupt")
file(COPY_FILE "/usr/bin/true" "${_source}")
file(WRITE "${_manifest}" "manifest-v1")
file(WRITE "${_runtime}/${_runtime_fixture}" "runtime-v1")
execute_process(COMMAND "${CMAKE_COMMAND}"
    -DPULP_CONTROL_HOST_ID=author-host
    -DPULP_CONTROL_HOST_SOURCE=${_source}
    -DPULP_CONTROL_HOST_MANIFEST=${_manifest}
    -DPULP_CONTROL_HOST_ROOT=${_catalog}
    -DPULP_CONTROL_HOST_RUNTIME_DIR=${_runtime}
    -P "${PULP_SOURCE_DIR}/tools/cmake/install_control_host.cmake"
    RESULT_VARIABLE _corrupt_reinstall_result
    ERROR_QUIET)
if(_corrupt_reinstall_result EQUAL 0)
    message(FATAL_ERROR "corrupt immutable catalog version was selected on reinstall")
endif()
file(READ "${_catalog}/author-host/active" _active_after_corruption)
string(STRIP "${_active_after_corruption}" _active_after_corruption)
if(NOT _active_after_corruption STREQUAL _updated_version)
    message(FATAL_ERROR "failed corrupt-version repair changed the active selection")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}"
    -DPULP_CONTROL_HOST_ID=author-host
    -DPULP_CONTROL_HOST_ROOT=${_catalog}
    -P "${PULP_SOURCE_DIR}/tools/cmake/remove_control_host.cmake"
    RESULT_VARIABLE _remove_result)
if(NOT _remove_result EQUAL 0 OR EXISTS "${_catalog}/author-host/active")
    message(FATAL_ERROR "atomic catalog removal failed")
endif()
