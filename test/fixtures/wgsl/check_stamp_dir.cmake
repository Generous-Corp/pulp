# pulp-wgsl-validate must write its --stamp file even when the stamp's
# directory does not exist yet, which is what a Unix Makefiles build hands it.
#   cmake -DVALIDATOR=<exe> -DSHADER=<file.wgsl> -DWORK_DIR=<dir> -P check_stamp_dir.cmake
foreach(_var VALIDATOR SHADER WORK_DIR)
    if(NOT ${_var})
        message(FATAL_ERROR "check_stamp_dir: ${_var} is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${WORK_DIR}")
set(_stamp "${WORK_DIR}/nested/missing/shader.validated")
execute_process(
    COMMAND "${VALIDATOR}" --stamp "${_stamp}" "${SHADER}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "validator exited ${_rc}:\n${_out}${_err}")
endif()
if(NOT EXISTS "${_stamp}")
    message(FATAL_ERROR "validator exited 0 but wrote no stamp at ${_stamp}")
endif()
file(READ "${_stamp}" _text)
message(STATUS "stamp written: ${_text}")
