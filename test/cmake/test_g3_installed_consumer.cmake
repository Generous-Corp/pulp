cmake_minimum_required(VERSION 3.24)
# Stages the producer's SDK install, configures one G3 example as an external
# find_package(Pulp COMPONENTS host) project, builds it, and runs its ctest.
if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR
        OR NOT DEFINED PULP_CONSUMER_EXAMPLE)
    message(FATAL_ERROR "PULP_BUILD_DIR, PULP_SOURCE_DIR and PULP_CONSUMER_EXAMPLE are required")
endif()
set(_example_dir "${PULP_SOURCE_DIR}/examples/${PULP_CONSUMER_EXAMPLE}")
if(NOT EXISTS "${_example_dir}/CMakeLists.txt")
    message(FATAL_ERROR "No consumer project at ${_example_dir}")
endif()
# Source-install markers are immutable. Give each producer identity its own
# prefix so rerunning this gate after a new source head cannot reuse old bytes.
file(SHA256 "${PULP_BUILD_DIR}/PulpStampSourceInstall.cmake" _producer_identity)
set(_root "${PULP_BUILD_DIR}/${PULP_CONSUMER_EXAMPLE}-smoke/${_producer_identity}")
set(_prefix "${_root}/prefix")
set(_build "${_root}/build")
if(NOT PULP_PARENT_BUILD_TYPE)
    set(PULP_PARENT_BUILD_TYPE Release)
endif()

function(run_step label)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${PULP_CONSUMER_EXAMPLE} ${label} failed (${_rc})\n${_out}\n${_err}")
    endif()
endfunction()

run_step(staging "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
    --prefix "${_prefix}" --config "${PULP_PARENT_BUILD_TYPE}")
set(_args -S "${_example_dir}" -B "${_build}" -G Ninja
    "-DCMAKE_PREFIX_PATH=${_prefix}" "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_BUILD_TYPE=${PULP_PARENT_BUILD_TYPE}"
    "-DCMAKE_CXX_FLAGS=${PULP_PARENT_CXX_FLAGS} ${PULP_PARENT_INSTRUMENTATION_CXX_FLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS=${PULP_PARENT_EXE_LINKER_FLAGS} ${PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS}")
if(PULP_PARENT_CXX_COMPILER)
    list(APPEND _args "-DCMAKE_CXX_COMPILER=${PULP_PARENT_CXX_COMPILER}")
endif()
string(TOLOWER "${PULP_PARENT_BUILD_TYPE}" _config_lower)
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
    "${CMAKE_COMMAND}" --build "${_build}" --config "${PULP_PARENT_BUILD_TYPE}")
# A consumer that registers no test would make ctest pass vacuously.
run_step(ctest "${CMAKE_CTEST_COMMAND}" --test-dir "${_build}" --output-on-failure
    --no-tests=error)
message(STATUS "${PULP_CONSUMER_EXAMPLE} installed-SDK consumer proof complete")
