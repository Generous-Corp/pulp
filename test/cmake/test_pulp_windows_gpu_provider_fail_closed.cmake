# ARM64EC/ARM64X must fail closed before a pure ARM64/x64 wgpu archive can be
# selected. This fixture exercises the real PulpFetchContent guard without a
# full dependency configure or network access.

set(_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/windows-gpu-provider-fail-closed")
file(MAKE_DIRECTORY "${_fixture_dir}")
set(_module "${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/PulpFetchContent.cmake")

foreach(_platform IN ITEMS ARM64EC ARM64X)
    set(_script "${_fixture_dir}/${_platform}.cmake")
    file(WRITE "${_script}" [=[
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_GENERATOR_PLATFORM "@PLATFORM@")
include("@MODULE@")
pulp_register_wgpu_native_precompiled_source(v24.0.3.1)
]=])
    file(READ "${_script}" _contents)
    string(REPLACE "@PLATFORM@" "${_platform}" _contents "${_contents}")
    string(REPLACE "@MODULE@" "${_module}" _contents "${_contents}")
    file(WRITE "${_script}" "${_contents}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -P "${_script}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(_rc EQUAL 0)
        message(FATAL_ERROR "${_platform} provider configure unexpectedly succeeded")
    endif()
    string(CONCAT _diagnostic "${_out}\n" "${_err}")
    if(NOT _diagnostic MATCHES "no pinned wgpu-native provider")
        message(FATAL_ERROR "${_platform} failed without the provider diagnostic:\n${_diagnostic}")
    endif()
endforeach()

message(STATUS "PASS: ARM64EC and ARM64X GPU provider paths fail closed")
