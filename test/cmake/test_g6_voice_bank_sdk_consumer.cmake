cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _run_id)
set(_fixture "${PULP_BUILD_DIR}/g6-voice-bank-${_run_id}")
set(_prefix "${_fixture}/prefix")
set(_source "${_fixture}/src")
set(_build "${_source}/build")
file(MAKE_DIRECTORY "${_source}")

function(run_step label)
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${_fixture}" TIMEOUT 300
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    file(WRITE "${_fixture}/${label}.log" "${_out}\n${_err}")
    message(STATUS "G6 voice bank ${label} result=${_result}")
    if(NOT "${_result}" STREQUAL "0")
        message(FATAL_ERROR "G6 voice bank ${label} failed; fixture=${_fixture}\n${_out}\n${_err}")
    endif()
endfunction()

run_step(source-head git -C "${PULP_SOURCE_DIR}" rev-parse HEAD)
file(READ "${_fixture}/source-head.log" _head)
string(STRIP "${_head}" _head)
run_step(stage "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
    --prefix "${_prefix}" --config "${PULP_PARENT_BUILD_TYPE}")
file(READ "${_prefix}/sdk-provenance.json" _provenance)
string(JSON _sdk_head GET "${_provenance}" source_git_sha)
string(JSON _sdk_dirty GET "${_provenance}" source_git_dirty)
if(NOT _sdk_head STREQUAL _head OR _sdk_dirty)
    message(FATAL_ERROR "G6 installed SDK provenance mismatch: head=${_head} provenance=${_provenance}")
endif()

foreach(_file CMakeLists.txt main.cpp fixture.hpp README.md a1_reference_vectors.json
        a1_reference_vectors.hpp generate_a1_vectors.py verify_a1_reference.py)
    configure_file("${PULP_SOURCE_DIR}/examples/g6-voice-bank-consumer/${_file}"
        "${_source}/${_file}" COPYONLY)
endforeach()
foreach(_relative
        core/host/include/pulp/host/voice_bank_binding.hpp
        core/host/include/pulp/host/signal_graph_subgraph.hpp
        core/host/include/pulp/host/voice_template.hpp
        examples/g6-voice-bank-consumer/CMakeLists.txt
        examples/g6-voice-bank-consumer/main.cpp
        examples/g6-voice-bank-consumer/fixture.hpp
        examples/g6-voice-bank-consumer/a1_reference_vectors.json
        examples/g6-voice-bank-consumer/a1_reference_vectors.hpp
        examples/g6-voice-bank-consumer/generate_a1_vectors.py
        examples/g6-voice-bank-consumer/README.md
        examples/g6-voice-bank-consumer/verify_a1_reference.py)
    file(SHA256 "${PULP_SOURCE_DIR}/${_relative}" _expected)
    if(_relative MATCHES "^examples/g6-voice-bank-consumer/")
        get_filename_component(_name "${_relative}" NAME)
        set(_actual "${_source}/${_name}")
    else()
        get_filename_component(_name "${_relative}" NAME)
        set(_actual "${_prefix}/include/pulp/host/${_name}")
    endif()
    file(SHA256 "${_actual}" _actual_hash)
    if(NOT _expected STREQUAL _actual_hash)
        message(FATAL_ERROR "G6 committed-head hash mismatch: ${_relative}")
    endif()
endforeach()
run_step(a1-vectors-check python3 "${_source}/generate_a1_vectors.py" --check)
run_step(a1-reference python3 "${_source}/verify_a1_reference.py")

set(_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_config "${PULP_PARENT_BUILD_TYPE}")
endif()
# Installed static libraries from an instrumented producer (a sanitizer lane)
# keep references to its runtime, so the consumer compiles and links with the
# producer's instrumentation.
set(_args -S "${_source}" -B "${_build}" -G Ninja
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DCMAKE_BUILD_TYPE=${_config}"
    "-DCMAKE_CXX_COMPILER=${PULP_PARENT_CXX_COMPILER}"
    "-DCMAKE_CXX_FLAGS=${PULP_PARENT_CXX_FLAGS} ${PULP_PARENT_INSTRUMENTATION_CXX_FLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS=${PULP_PARENT_EXE_LINKER_FLAGS} ${PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS}")
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
    "${CMAKE_COMMAND}" --build "${_build}" --target pulp-g6-voice-bank-consumer)
run_step(positive "${_build}/pulp-g6-voice-bank-consumer")
run_step(ctest "${CMAKE_CTEST_COMMAND}" --test-dir "${_build}" --output-on-failure)
message(STATUS "G6 installed public graph voice-bank consumer proof complete")
