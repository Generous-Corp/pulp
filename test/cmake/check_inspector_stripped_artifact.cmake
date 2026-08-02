if(NOT DEFINED SYMBOL_TOOL OR SYMBOL_TOOL STREQUAL "")
    message(FATAL_ERROR "CMake did not provide a symbol inspection tool")
endif()

function(inspect_symbols artifact result_var output_var error_var)
    if(SYMBOL_MODE STREQUAL "DUMPBIN")
        execute_process(
            COMMAND "${SYMBOL_TOOL}" /symbols "${artifact}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error)
    else()
        execute_process(
            COMMAND "${SYMBOL_TOOL}" -g "${artifact}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error)
    endif()
    set("${result_var}" "${result}" PARENT_SCOPE)
    set("${output_var}" "${output}" PARENT_SCOPE)
    set("${error_var}" "${error}" PARENT_SCOPE)
endfunction()

function(line_is_defined_inspector_symbol line result_var)
    set(is_match FALSE)
    if(SYMBOL_MODE STREQUAL "NM")
        if(NOT line MATCHES "[ \\t]U[ \\t]" AND
           line MATCHES "_?ZN4pulp7inspect")
            set(is_match TRUE)
        endif()
    else()
        # MSVC and clang-cl use the same COFF decoration. dumpbin reports
        # undefined archive entries as UNDEF; llvm-nm uses the usual U column.
        if(NOT line MATCHES "UNDEF" AND
           NOT line MATCHES "[ \\t]U[ \\t]" AND
           line MATCHES "@inspect@pulp@@")
            set(is_match TRUE)
        endif()
    endif()
    set("${result_var}" "${is_match}" PARENT_SCOPE)
endfunction()

inspect_symbols("${STRIPPED_ARTIFACT}"
    stripped_result stripped_symbols stripped_error)
if(NOT stripped_result EQUAL 0)
    message(FATAL_ERROR
        "symbol inspection failed for stripped artifact ${STRIPPED_ARTIFACT}: "
        "${stripped_error}")
endif()

string(REPLACE "\n" ";" stripped_lines "${stripped_symbols}")
foreach(line IN LISTS stripped_lines)
    line_is_defined_inspector_symbol("${line}" is_inspector_symbol)
    if(is_inspector_symbol)
        message(FATAL_ERROR
            "ordinary pulp-format consumer contains inspector symbol: ${line}")
    endif()
endforeach()

if(DEFINED INSPECTOR_ARCHIVE AND NOT INSPECTOR_ARCHIVE STREQUAL "")
    inspect_symbols("${INSPECTOR_ARCHIVE}"
        inspector_result inspector_symbols inspector_error)
    if(NOT inspector_result EQUAL 0)
        message(FATAL_ERROR
            "symbol inspection failed for inspector positive control "
            "${INSPECTOR_ARCHIVE}: "
            "${inspector_error}")
    endif()
    set(found_positive_control FALSE)
    string(REPLACE "\n" ";" inspector_lines "${inspector_symbols}")
    foreach(line IN LISTS inspector_lines)
        line_is_defined_inspector_symbol("${line}" is_inspector_symbol)
        if(is_inspector_symbol)
            set(found_positive_control TRUE)
            break()
        endif()
    endforeach()
    if(NOT found_positive_control)
        message(FATAL_ERROR
            "inspector positive control has no pulp::inspect symbol; "
            "the stripped-artifact check would be vacuous")
    endif()
endif()
