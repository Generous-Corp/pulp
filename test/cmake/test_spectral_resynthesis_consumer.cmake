cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()
# Source-install markers are immutable. Give each producer identity its own
# prefix so rerunning this gate after a new source head cannot reuse old bytes.
file(SHA256 "${PULP_BUILD_DIR}/PulpStampSourceInstall.cmake" _producer_identity)
set(_root "${PULP_BUILD_DIR}/spectral-resynthesis-consumer-smoke/${_producer_identity}")
set(_prefix "${_root}/prefix")
set(_build "${_root}/build")
if(NOT PULP_PARENT_BUILD_TYPE)
    set(PULP_PARENT_BUILD_TYPE Release)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
    --prefix "${_prefix}" --config "${PULP_PARENT_BUILD_TYPE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "SDK staging failed (${_rc})\n${_out}\n${_err}")
endif()
set(_flags "${PULP_PARENT_CXX_FLAGS} ${PULP_PARENT_INSTRUMENTATION_CXX_FLAGS}")
set(_link_flags "${PULP_PARENT_EXE_LINKER_FLAGS} ${PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS}")
execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${PULP_SOURCE_DIR}/examples/spectral-resynthesis-consumer" -B "${_build}" -G Ninja
    "-DCMAKE_PREFIX_PATH=${_prefix}" "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_BUILD_TYPE=${PULP_PARENT_BUILD_TYPE}"
    "-DCMAKE_CXX_FLAGS=${_flags}" "-DCMAKE_EXE_LINKER_FLAGS=${_link_flags}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Spectral resynthesis consumer configure failed (${_rc})\n${_out}\n${_err}")
endif()
execute_process(COMMAND bash "${PULP_SOURCE_DIR}/tools/ci/governed-build.sh"
    "${CMAKE_COMMAND}" --build "${_build}" --config "${PULP_PARENT_BUILD_TYPE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Spectral resynthesis consumer build failed (${_rc})\n${_out}\n${_err}")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${_build}" --output-on-failure
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Spectral resynthesis consumer checks failed (${_rc})\n${_out}\n${_err}")
endif()
