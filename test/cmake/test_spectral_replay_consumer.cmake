cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and PULP_BUILD_DIR are required")
endif()
file(SHA256 "${PULP_BUILD_DIR}/PulpStampSourceInstall.cmake" _producer)
set(_root "${PULP_BUILD_DIR}/a3-replay-sdk/${_producer}")
set(_prefix "${_root}/prefix")
set(_build "${_root}/build")
set(_fixture "${PULP_SOURCE_DIR}/test/fixtures/a3_spectral_replay")
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
set(_link "${PULP_PARENT_EXE_LINKER_FLAGS} ${PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS}")
execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${PULP_SOURCE_DIR}/examples/a3-spectral-replay-consumer" -B "${_build}" -G Ninja
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp" "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DCMAKE_BUILD_TYPE=${PULP_PARENT_BUILD_TYPE}"
    "-DCMAKE_CXX_FLAGS=${_flags}" "-DCMAKE_EXE_LINKER_FLAGS=${_link}"
    "-DA3_REPLAY_FIXTURE=${_fixture}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Installed replay configure failed (${_rc})\n${_out}\n${_err}")
endif()
execute_process(COMMAND bash "${PULP_SOURCE_DIR}/tools/ci/governed-build.sh"
    "${CMAKE_COMMAND}" --build "${_build}" --config "${PULP_PARENT_BUILD_TYPE}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Installed replay build failed (${_rc})\n${_out}\n${_err}")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${_build}" --output-on-failure
    RESULT_VARIABLE _positive OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _positive EQUAL 0)
    message(FATAL_ERROR "Installed replay positive failed (${_positive})\n${_out}\n${_err}")
endif()
# Prove the installed executable refuses changed saved inputs, and returns to
# success with exactly the original bytes. No source-tree target is executed.
set(_negative "${_root}/negative")
file(COPY "${_fixture}/" DESTINATION "${_negative}")
file(APPEND "${_negative}/control.f32" "X")
execute_process(COMMAND "${_build}/a3-spectral-replay" "${_negative}"
    RESULT_VARIABLE _broken OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _broken EQUAL 42)
    message(FATAL_ERROR "Installed changed-input control expected 42, got ${_broken}\n${_out}\n${_err}")
endif()
file(COPY "${_fixture}/control.f32" DESTINATION "${_negative}")
execute_process(COMMAND "${_build}/a3-spectral-replay" "${_negative}"
    RESULT_VARIABLE _restored OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _restored EQUAL 0)
    message(FATAL_ERROR "Installed restored input failed (${_restored})\n${_out}\n${_err}")
endif()
file(REMOVE "${_negative}/graph.pulpgraph")
execute_process(COMMAND "${_build}/a3-spectral-replay" "${_negative}"
    RESULT_VARIABLE _missing OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _missing EQUAL 42)
    message(FATAL_ERROR "Installed missing-input control expected 42, got ${_missing}\n${_out}\n${_err}")
endif()
message(STATUS "Installed A3 replay: positive=${_positive}, corrupt=${_broken}, restored=${_restored}, missing=${_missing}")
message(STATUS "Installed A3 producer=${_producer}")
