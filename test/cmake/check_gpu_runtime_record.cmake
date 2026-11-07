# Drives pulp_check_gpu_runtime_record() through a matching, a mismatched and
# a missing record; each refusal runs in a child cmake so its FATAL_ERROR does
# not end this script.
#   cmake -DMODULE=<PulpGpuRuntimeRecord.cmake> -DWORK_DIR=<dir> -P check_gpu_runtime_record.cmake
cmake_minimum_required(VERSION 3.24)
include("${MODULE}")
if(MODE STREQUAL "check")
    pulp_check_gpu_runtime_record("${PREFIX}" "${COMMIT}")
    return()
endif()
set(_pin "14bd4c5e430bc8da6b4204f1daa8a1bdbc6b7f3a")
file(REMOVE_RECURSE "${WORK_DIR}")
foreach(_case match mismatch missing)
    file(MAKE_DIRECTORY "${WORK_DIR}/${_case}")
endforeach()
file(WRITE "${WORK_DIR}/match/gpu-runtime-artifact.json" "{\"source_commit\": \"${_pin}\"}")
file(WRITE "${WORK_DIR}/mismatch/gpu-runtime-artifact.json"
    "{\"source_commit\": \"4906158c8a6b0e1d252bf086d4f238b819cda923\"}")
foreach(_case match mismatch missing)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DMODE=check -DMODULE=${MODULE}
            -DPREFIX=${WORK_DIR}/${_case} -DCOMMIT=${_pin} -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE _result OUTPUT_QUIET ERROR_VARIABLE _error)
    if(_case STREQUAL "match" AND NOT _result EQUAL 0)
        message(FATAL_ERROR "a record naming the pin was refused: ${_error}")
    elseif(NOT _case STREQUAL "match" AND _result EQUAL 0)
        message(FATAL_ERROR "a ${_case} record was accepted")
    endif()
endforeach()
message(STATUS "gpu-runtime record check: ok")
