cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()

# Fresh staging prevents stale SDK files and keeps the mutation out of the
# checkout shared with parallel CTest processes. Retain the evidence on failure.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _run_id)
set(_fixture "${PULP_BUILD_DIR}/g6-voice-contract-${_run_id}")
set(_prefix "${_fixture}/prefix")
set(_source "${_fixture}/src")
set(_build "${_source}/build")
file(MAKE_DIRECTORY "${_source}" "${_fixture}/logs")

function(run_step label)
    execute_process(COMMAND ${ARGN}
        WORKING_DIRECTORY "${_source}" TIMEOUT 240
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    file(WRITE "${_fixture}/${label}.log" "${_out}\n${_err}")
    message(STATUS "G6 ${label} result=${_result}")
    if(NOT label STREQUAL "stage" OR NOT "${_result}" STREQUAL "0")
        message(STATUS "${_out}\n${_err}")
    endif()
    if(NOT "${_result}" STREQUAL "0")
        message(FATAL_ERROR "G6 ${label} failed; fixture=${_fixture}")
    endif()
endfunction()

set(_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_config "${PULP_PARENT_BUILD_TYPE}")
endif()
run_step(source-head git -C "${PULP_SOURCE_DIR}" rev-parse HEAD)
run_step(stage "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
    --prefix "${_prefix}" --config "${_config}")
file(READ "${_prefix}/sdk-provenance.json" _provenance)
message(STATUS "G6 installed SDK provenance: ${_provenance}")
foreach(_header mpe_synth_voice.hpp mpe_buffer.hpp mpe_voice_tracker.hpp)
    file(SHA256 "${PULP_SOURCE_DIR}/core/midi/include/pulp/midi/${_header}" _expected)
    file(SHA256 "${_prefix}/include/pulp/midi/${_header}" _installed)
    if(NOT _expected STREQUAL _installed)
        message(FATAL_ERROR "G6 installed header mismatch: ${_header}")
    endif()
    message(STATUS "G6 installed header ${_header} sha256=${_installed}")
endforeach()
foreach(_file CMakeLists.txt main.cpp)
    configure_file("${PULP_SOURCE_DIR}/examples/g6-voice-contract-consumer/${_file}"
        "${_source}/${_file}" COPYONLY)
    file(SHA256 "${_source}/${_file}" _sha)
    message(STATUS "G6 consumer ${_file} sha256=${_sha}")
endforeach()
# confirm_failure verifies against the index; no commit or Git identity needed.
run_step(index-init git init --quiet)
run_step(index-add git add CMakeLists.txt main.cpp)

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
    "${CMAKE_COMMAND}" --build "${_build}" --target pulp-g6-voice-contract)
run_step(positive "${_build}/pulp-g6-voice-contract")
run_step(ctest "${CMAKE_CTEST_COMMAND}" --test-dir "${_build}" --output-on-failure)

set(_break [=[perl -0pi -e 's/auto stale = event\(Kind::Pressure, 1\)\x3b/auto stale = event(Kind::Pressure, 9)\x3b/']=])
run_step(mutation "${CMAKE_COMMAND}" -E env "TMPDIR=${_fixture}/logs"
    bash "${PULP_SOURCE_DIR}/tools/scripts/confirm_failure.sh"
    --file main.cpp --break "${_break}"
    --build-dir build --target pulp-g6-voice-contract --jobs 1
    --test ./build/pulp-g6-voice-contract --keep-logs)

# Copy every phase into CTest's captured output, which PR CI uploads, and
# require the intended oracle failure rather than accepting any failed run.
file(GLOB _log_dirs "${_fixture}/logs/confirm-failure.*")
list(LENGTH _log_dirs _log_count)
if(NOT _log_count EQUAL 1)
    message(FATAL_ERROR "G6 expected exactly one mutation receipt directory")
endif()
list(GET _log_dirs 0 _logs)
foreach(_phase baseline broken restored)
    file(READ "${_logs}/${_phase}.log" _output)
    message(STATUS "G6 ${_phase} test output:\n${_output}")
    if(_phase STREQUAL "broken")
        set(_expected "g6-allocator-contract FAIL rule=stale-generation-expression")
    else()
        set(_expected "g6-allocator-contract PASS checks=")
    endif()
    string(FIND "${_output}" "${_expected}" _found)
    if(_found EQUAL -1)
        message(FATAL_ERROR "G6 ${_phase} missing required oracle: ${_expected}")
    endif()
endforeach()
foreach(_file CMakeLists.txt main.cpp)
    file(SHA256 "${PULP_SOURCE_DIR}/examples/g6-voice-contract-consumer/${_file}" _expected)
    file(SHA256 "${_source}/${_file}" _restored)
    if(NOT _expected STREQUAL _restored)
        message(FATAL_ERROR "G6 source restoration mismatch: ${_file}")
    endif()
endforeach()
message(STATUS "G6 installed-SDK positive/mutation/restored proof complete")
