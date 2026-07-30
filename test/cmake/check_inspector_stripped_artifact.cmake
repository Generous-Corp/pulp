if(NOT DEFINED NM OR NM STREQUAL "")
    message(FATAL_ERROR "CMake did not provide an nm executable")
endif()

execute_process(
    COMMAND "${NM}" -g "${STRIPPED_ARTIFACT}"
    RESULT_VARIABLE stripped_result
    OUTPUT_VARIABLE stripped_symbols
    ERROR_VARIABLE stripped_error)
if(NOT stripped_result EQUAL 0)
    message(FATAL_ERROR
        "nm failed for stripped artifact ${STRIPPED_ARTIFACT}: "
        "${stripped_error}")
endif()

string(REPLACE "\n" ";" stripped_lines "${stripped_symbols}")
foreach(line IN LISTS stripped_lines)
    if(line MATCHES "[ \\t]U[ \\t]")
        continue()
    endif()
    if(line MATCHES "_?ZN4pulp7inspect")
        message(FATAL_ERROR
            "ordinary pulp-format consumer contains inspector symbol: ${line}")
    endif()
endforeach()

if(DEFINED INSPECTOR_ARCHIVE AND NOT INSPECTOR_ARCHIVE STREQUAL "")
    execute_process(
        COMMAND "${NM}" -g "${INSPECTOR_ARCHIVE}"
        RESULT_VARIABLE inspector_result
        OUTPUT_VARIABLE inspector_symbols
        ERROR_VARIABLE inspector_error)
    if(NOT inspector_result EQUAL 0)
        message(FATAL_ERROR
            "nm failed for inspector positive control ${INSPECTOR_ARCHIVE}: "
            "${inspector_error}")
    endif()
    if(NOT inspector_symbols MATCHES "_?ZN4pulp7inspect")
        message(FATAL_ERROR
            "inspector positive control has no pulp::inspect symbol; "
            "the stripped-artifact check would be vacuous")
    endif()
endif()
