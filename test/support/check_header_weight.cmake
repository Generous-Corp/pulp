# Preprocesses a translation unit that includes only HEADER, with the
# consumer's include directories and definitions, and fails when the result
# pulls in a header matching FORBID or exceeds MAX_LINES preprocessed lines.
# Prints the measured size either way, so a header's weight is visible in the
# test log. COMPILER, FLAGS (a list), HEADER, FORBID and MAX_LINES are passed
# with -D; WORK is a scratch directory.
foreach(_var COMPILER HEADER MAX_LINES WORK)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "check_header_weight: ${_var} is required")
    endif()
endforeach()
file(MAKE_DIRECTORY "${WORK}")
string(MAKE_C_IDENTIFIER "${HEADER}" _id)
set(_tu "${WORK}/${_id}.cpp")
file(WRITE "${_tu}" "#include <${HEADER}>\n")
execute_process(
    COMMAND "${COMPILER}" ${FLAGS} -x c++ -std=c++20 -E -H "${_tu}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "check_header_weight: ${HEADER} does not preprocess on its own:\n${_err}")
endif()
string(REGEX MATCHALL "\n" _newlines "${_out}")
list(LENGTH _newlines _lines)
string(REGEX MATCHALL "(^|\n)\\.+ [^\n]+" _included "${_err}")
list(LENGTH _included _headers)
message(STATUS "${HEADER}: ${_lines} preprocessed lines, ${_headers} headers")
if(DEFINED FORBID AND NOT "${FORBID}" STREQUAL "")
    foreach(_line IN LISTS _included)
        if(_line MATCHES "${FORBID}")
            string(STRIP "${_line}" _line)
            message(FATAL_ERROR "${HEADER} pulls in a header it must not: ${_line}")
        endif()
    endforeach()
endif()
if(_lines GREATER MAX_LINES)
    message(FATAL_ERROR "${HEADER}: ${_lines} preprocessed lines, over its budget of ${MAX_LINES}")
endif()
