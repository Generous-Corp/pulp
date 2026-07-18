if(NOT DEFINED NM OR NM STREQUAL "")
    message(FATAL_ERROR "CMake did not provide an nm executable")
endif()

execute_process(
    COMMAND "${NM}" -g "${ARCHIVE}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE nm_output
    ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "nm failed for ${ARCHIVE}: ${nm_error}")
endif()

string(REPLACE "\n" ";" nm_lines "${nm_output}")
foreach(line IN LISTS nm_lines)
    if(line MATCHES "[ \\t]U[ \\t]")
        continue()
    endif()
    if(line MATCHES "(^|[ \\t])_?drwav_[A-Za-z0-9_]+$")
        message(FATAL_ERROR "pulp-audio exports private dr_wav symbol: ${line}")
    endif()
endforeach()
