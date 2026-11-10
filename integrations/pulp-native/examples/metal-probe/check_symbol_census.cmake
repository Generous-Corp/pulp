# Counts the UI-stack code a built binary defines, from its full symbol table:
#
#   cmake -DNM=<nm> -DBINARY=<file> -DEXPECT=absent|present [-DREQUIRED=<name>]
#         [-DREPORT=<json>] -P check_symbol_census.cmake
#
# Families: skia (Sk* classes), dawn (dawn::native, wgpu*), vellum, rmlui
# (Rml::) and pulp_view (pulp::view). Only definitions count, never undefined
# references.
#
# EXPECT=absent fails if any family has a definition, and REQUIRED names a
# symbol the binary must define, the control that the listing is not empty.
# EXPECT=present fails unless pulp_view has definitions: the control that the
# census can see a family when it is there.
cmake_minimum_required(VERSION 3.24)

execute_process(COMMAND "${NM}" "${BINARY}"
    OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${NM} ${BINARY} failed (${_rc}): ${_err}")
endif()

set(_families skia dawn vellum rmlui pulp_view)
set(_pattern_skia "(^_?Sk[A-Z][A-Za-z0-9]+|[0-9]+Sk[A-Z][A-Za-z0-9]+)")
set(_pattern_dawn "(4dawn6native|^_?wgpu[A-Z])")
set(_pattern_vellum "6vellum")
set(_pattern_rmlui "3Rml[0-9]")
set(_pattern_pulp_view "4pulp4view")
foreach(_f IN LISTS _families)
    set(_count_${_f} 0)
    set(_sample_${_f} "")
endforeach()
set(_has_required FALSE)
set(_defined 0)

string(REPLACE "\n" ";" _lines "${_out}")
foreach(_line IN LISTS _lines)
    if(NOT _line MATCHES "^[0-9a-fA-F]+ +([A-Za-z]) +(.+)$")
        continue()
    endif()
    set(_type "${CMAKE_MATCH_1}")
    set(_name "${CMAKE_MATCH_2}")
    if(_type STREQUAL "U" OR _type STREQUAL "u")
        continue()
    endif()
    math(EXPR _defined "${_defined} + 1")
    if(REQUIRED AND _name MATCHES "^_?${REQUIRED}$")
        set(_has_required TRUE)
    endif()
    foreach(_f IN LISTS _families)
        if(_name MATCHES "${_pattern_${_f}}")
            math(EXPR _count_${_f} "${_count_${_f}} + 1")
            list(LENGTH _sample_${_f} _n)
            if(_n LESS 3)
                list(APPEND _sample_${_f} "${_name}")
            endif()
        endif()
    endforeach()
endforeach()

set(_json "{\"binary\": \"${BINARY}\", \"defined_symbols\": ${_defined}")
foreach(_f IN LISTS _families)
    string(APPEND _json ", \"${_f}\": ${_count_${_f}}")
endforeach()
string(APPEND _json "}\n")
if(REPORT)
    file(WRITE "${REPORT}" "${_json}")
endif()
message(STATUS "symbol census: ${_json}")

if(_defined EQUAL 0)
    message(FATAL_ERROR "${BINARY} defines no symbols: the listing is blind")
endif()
if(EXPECT STREQUAL "absent")
    if(REQUIRED AND NOT _has_required)
        message(FATAL_ERROR "${BINARY} does not define ${REQUIRED}: the listing is blind")
    endif()
    foreach(_f IN LISTS _families)
        if(_count_${_f} GREATER 0)
            message(FATAL_ERROR
                "${BINARY} defines ${_count_${_f}} ${_f} symbol(s), e.g. ${_sample_${_f}}")
        endif()
    endforeach()
elseif(EXPECT STREQUAL "present")
    if(_count_pulp_view EQUAL 0)
        message(FATAL_ERROR "${BINARY} defines no pulp::view symbol: the census cannot see one")
    endif()
else()
    message(FATAL_ERROR "EXPECT must be absent or present")
endif()
