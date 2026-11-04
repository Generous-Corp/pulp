cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _run_id)
set(_fixture "${PULP_BUILD_DIR}/g2-processor-restore-${_run_id}")
set(_prefix "${_fixture}/prefix")
set(_source "${_fixture}/source")
set(_build "${_fixture}/consumer-build")
file(MAKE_DIRECTORY "${_source}" "${PULP_BUILD_DIR}/Testing/Temporary")
set(_receipt "${PULP_BUILD_DIR}/Testing/Temporary/g2-processor-restore-${_run_id}.log")

function(run_step label)
    execute_process(COMMAND ${ARGN}
        WORKING_DIRECTORY "${_fixture}" TIMEOUT 240
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    file(WRITE "${_fixture}/${label}.log" "${_out}\n${_err}")
    file(APPEND "${_receipt}" "${label}: ${_result}\n${_out}\n${_err}\n")
    if(NOT "${_result}" STREQUAL "0")
        message(FATAL_ERROR "G2 ${label} failed: ${_out}\n${_err}\nfixture=${_fixture}")
    endif()
    message(STATUS "G2 ${label}: passed")
endfunction()

set(_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_config "${PULP_PARENT_BUILD_TYPE}")
endif()
run_step(stage "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
    --prefix "${_prefix}" --config "${_config}")
file(READ "${_prefix}/sdk-provenance.json" _provenance)
file(APPEND "${_receipt}" "Installed provenance: ${_provenance}\n")
file(SHA256 "${PULP_SOURCE_DIR}/core/host/include/pulp/host/processor_restore.hpp" _expected)
file(SHA256 "${_prefix}/include/pulp/host/processor_restore.hpp" _actual)
if(NOT _actual STREQUAL _expected)
    message(FATAL_ERROR "G2 installed header differs from this checkout")
endif()
foreach(_file CMakeLists.txt main.cpp)
    configure_file("${PULP_SOURCE_DIR}/examples/g2-processor-restore-consumer/${_file}"
        "${_source}/${_file}" COPYONLY)
endforeach()

set(_args -S "${_source}" -B "${_build}" -G Ninja
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DCMAKE_BUILD_TYPE=${_config}"
    "-DCMAKE_CXX_FLAGS=${PULP_PARENT_CXX_FLAGS} ${PULP_PARENT_INSTRUMENTATION_CXX_FLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS=${PULP_PARENT_EXE_LINKER_FLAGS} ${PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS}")
if(PULP_PARENT_CXX_COMPILER)
    list(APPEND _args "-DCMAKE_CXX_COMPILER=${PULP_PARENT_CXX_COMPILER}")
endif()
string(TOLOWER "${_config}" _config_lower)
if(_config_lower STREQUAL "debug")
    list(APPEND _args -DPULP_ALLOW_DEBUG_SDK=ON)
endif()
if(PULP_PARENT_SANITIZER)
    list(APPEND _args "-DPULP_SANITIZER=${PULP_PARENT_SANITIZER}")
endif()
if(PULP_PARENT_OSX_ARCHITECTURES)
    list(APPEND _args "-DCMAKE_OSX_ARCHITECTURES=${PULP_PARENT_OSX_ARCHITECTURES}")
endif()
run_step(configure "${CMAKE_COMMAND}" ${_args})
run_step(build "${CMAKE_COMMAND}" -E env PULP_BUILD_JOBS=1
    bash "${PULP_SOURCE_DIR}/tools/ci/governed-build.sh"
    "${CMAKE_COMMAND}" --build "${_build}")
run_step(consumer "${_build}/pulp-processor-restore-consumer")
run_step(ctest "${CMAKE_CTEST_COMMAND}" --test-dir "${_build}" --output-on-failure)
message(STATUS "G2 installed-SDK restoration proof complete; graph v4 persistence not claimed")
