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
file(MAKE_DIRECTORY "${PULP_BUILD_DIR}/Testing/Temporary")
set(_receipt "${PULP_BUILD_DIR}/Testing/Temporary/g6-voice-contract-${_run_id}.log")

# This directory is already uploaded by PR CI. Unlike LastTest.log, this
# dedicated transcript survives subsequent CTest invocations in the same job.
function(report content)
    message(STATUS "${content}")
    file(APPEND "${_receipt}" "${content}\n")
endfunction()

function(run_step label)
    execute_process(COMMAND ${ARGN}
        WORKING_DIRECTORY "${_fixture}" TIMEOUT 240
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    file(WRITE "${_fixture}/${label}.log" "${_out}\n${_err}")
    report("G6 ${label} result=${_result}")
    if(NOT label STREQUAL "stage" OR NOT "${_result}" STREQUAL "0")
        report("${_out}\n${_err}")
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
file(READ "${_fixture}/source-head.log" _head)
string(STRIP "${_head}" _head)
run_step(source-clean git -C "${PULP_SOURCE_DIR}" diff --quiet HEAD --)

function(assert_committed_file relative actual)
    execute_process(COMMAND git -C "${PULP_SOURCE_DIR}" show "${_head}:${relative}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _blob ERROR_VARIABLE _err)
    if(NOT "${_result}" STREQUAL "0")
        message(FATAL_ERROR "G6 committed source unavailable: ${relative}: ${_err}")
    endif()
    string(SHA256 _expected "${_blob}")
    file(SHA256 "${actual}" _actual)
    if(NOT _expected STREQUAL _actual)
        message(FATAL_ERROR "G6 committed-head hash mismatch: ${relative}")
    endif()
    report("G6 committed ${relative} sha256=${_actual}")
endfunction()

run_step(stage "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
    --prefix "${_prefix}" --config "${_config}")
file(READ "${_prefix}/sdk-provenance.json" _provenance)
report("G6 installed SDK provenance: ${_provenance}")
string(JSON _sdk_head GET "${_provenance}" source_git_sha)
string(JSON _sdk_dirty GET "${_provenance}" source_git_dirty)
string(JSON _sdk_dirty_type TYPE "${_provenance}" source_git_dirty)
if(NOT _sdk_head STREQUAL _head OR _sdk_dirty OR
   NOT _sdk_dirty_type STREQUAL "BOOLEAN")
    message(FATAL_ERROR "G6 requires a clean SDK built from checkout HEAD ${_head}")
endif()
foreach(_header mpe_synth_voice.hpp mpe_buffer.hpp mpe_voice_tracker.hpp)
    assert_committed_file("core/midi/include/pulp/midi/${_header}"
        "${_prefix}/include/pulp/midi/${_header}")
endforeach()
foreach(_file CMakeLists.txt main.cpp)
    configure_file("${PULP_SOURCE_DIR}/examples/g6-voice-contract-consumer/${_file}"
        "${_source}/${_file}" COPYONLY)
    assert_committed_file("examples/g6-voice-contract-consumer/${_file}"
        "${_source}/${_file}")
endforeach()
# confirm_failure verifies against the index; no commit or Git identity needed.
run_step(index-init git init --quiet)
run_step(index-add git add src/CMakeLists.txt src/main.cpp
    prefix/include/pulp/midi/mpe_synth_voice.hpp)

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

set(_break [=[perl -0pi -e 's/v\.active\(\) && v\.note_id\(\) == id/v.active() \&\& v.note_id() != id/']=])
run_step(mutation "${CMAKE_COMMAND}" -E env "TMPDIR=${_fixture}/logs"
    bash "${PULP_SOURCE_DIR}/tools/scripts/confirm_failure.sh"
    --file prefix/include/pulp/midi/mpe_synth_voice.hpp --break "${_break}"
    --build-dir src/build --target pulp-g6-voice-contract --jobs 1
    --test ./src/build/pulp-g6-voice-contract --keep-logs)

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
    report("G6 ${_phase} test output:\n${_output}")
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
    assert_committed_file("examples/g6-voice-contract-consumer/${_file}"
        "${_source}/${_file}")
endforeach()
assert_committed_file("core/midi/include/pulp/midi/mpe_synth_voice.hpp"
    "${_prefix}/include/pulp/midi/mpe_synth_voice.hpp")
report("G6 installed-SDK positive/mutation/restored proof complete")
