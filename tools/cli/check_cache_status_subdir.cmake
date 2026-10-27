# `pulp cache status` lists every entry of $PULP_HOME/cache. A browser-capture
# probe leaves a DIRECTORY there, which the listing used to size with
# std::filesystem::file_size and abort on ("Is a directory"). Seed an isolated
# PULP_HOME with a file and a nested directory and require both to be listed,
# the directory with the bytes of the files below it.
if(NOT DEFINED CLI OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "check_cache_status_subdir.cmake requires CLI and WORK_DIR")
endif()

set(_home "${WORK_DIR}/pulp-home")
file(REMOVE_RECURSE "${_home}")
file(MAKE_DIRECTORY "${_home}/cache/browser-probe/profile")
file(WRITE "${_home}/cache/top.bin" "0123456789")
string(REPEAT "x" 1536 _a)
string(REPEAT "y" 1536 _b)
file(WRITE "${_home}/cache/browser-probe/probe.bin" "${_a}")
file(WRITE "${_home}/cache/browser-probe/profile/state.bin" "${_b}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PULP_HOME=${_home}" "PULP_UPDATE_CHECK_DISABLED=1"
            "${CLI}" cache status
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
file(REMOVE_RECURSE "${_home}")

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "pulp cache status exited ${_rc} with a directory in the cache:\n${_out}${_err}")
endif()
# 1536 + 1536 bytes below the directory, 10 in the file.
foreach(_expected "browser-probe/ (3 KB)" "top.bin (10 B)")
    string(FIND "${_out}" "${_expected}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "pulp cache status did not list '${_expected}':\n${_out}${_err}")
    endif()
endforeach()
message(STATUS "cache status lists a directory entry with its contents' size")
