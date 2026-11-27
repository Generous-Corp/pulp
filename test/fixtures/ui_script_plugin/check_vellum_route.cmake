# Asserts which scripted-editor route a built plug-in image carries, from its
# full symbol table:
#
#   cmake -DNM=<nm> -DBINARY=<file> -DROUTE=vellum|legacy -P check_vellum_route.cmake
#
# ROUTE=vellum: the image defines the Vellum scripted editor factory and none of
# Pulp's own editor stack (pulp::view code, the format ViewBridge, or the legacy
# scripted editor builder), so a UI_SCRIPT plug-in declared with UI_ENGINE
# vellum needs nothing the view layer deletion removes.
# ROUTE=legacy: the image defines Pulp's own editor stack. Run on a listing
# from a plain UI_SCRIPT plug-in, it is the control that the census can see
# that stack.
#
# Only definitions count, never undefined references, and never a standard
# library symbol instantiated over one of these types.
cmake_minimum_required(VERSION 3.24)

foreach(_var NM BINARY ROUTE)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "${_var} is required")
    endif()
endforeach()
execute_process(COMMAND "${NM}" "${BINARY}"
    OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${NM} ${BINARY} failed (${_rc}): ${_err}")
endif()

set(_pattern_factory "4pulp6vellum23scripted_editor_factoryEv$")
set(_pattern_view "4pulp4view")
set(_pattern_bridge "4pulp6format10ViewBridge")
set(_pattern_builder "4pulp6format31install_scripted_editor_builderEv$")
set(_pattern_std_owned "^_?_Z(T[VIS]|Th[n0-9]*_|Tv[n0-9]*_)?(N[KVr]*(St|Sa|Ss)|St)")

set(_defined 0)
foreach(_f factory view bridge builder)
    set(_count_${_f} 0)
    set(_sample_${_f} "")
endforeach()
string(REPLACE "\n" ";" _lines "${_out}")
foreach(_line IN LISTS _lines)
    if(NOT _line MATCHES "^[0-9a-fA-F]+ +([A-Za-z]) +(.+)$")
        continue()
    endif()
    if(CMAKE_MATCH_1 STREQUAL "U" OR CMAKE_MATCH_1 STREQUAL "u")
        continue()
    endif()
    set(_name "${CMAKE_MATCH_2}")
    math(EXPR _defined "${_defined} + 1")
    if(_name MATCHES "${_pattern_std_owned}")
        continue()
    endif()
    foreach(_f factory view bridge builder)
        if(_name MATCHES "${_pattern_${_f}}")
            math(EXPR _count_${_f} "${_count_${_f}} + 1")
            list(LENGTH _sample_${_f} _n)
            if(_n LESS 3)
                list(APPEND _sample_${_f} "${_name}")
            endif()
        endif()
    endforeach()
endforeach()

set(_summary "defined=${_defined} factory=${_count_factory} view=${_count_view} "
    "bridge=${_count_bridge} builder=${_count_builder}")
string(JOIN "" _summary ${_summary})
message(STATUS "${BINARY}: ${_summary}")
if(_defined EQUAL 0)
    message(FATAL_ERROR "${BINARY} defines no symbols: the listing is blind")
endif()

if(ROUTE STREQUAL "vellum")
    if(_count_factory EQUAL 0)
        message(FATAL_ERROR "${BINARY} does not define the Vellum scripted editor factory")
    endif()
    foreach(_f view bridge builder)
        if(NOT _count_${_f} EQUAL 0)
            message(FATAL_ERROR
                "${BINARY} carries Pulp's own editor stack (${_f}): ${_sample_${_f}}")
        endif()
    endforeach()
elseif(ROUTE STREQUAL "legacy")
    foreach(_f view bridge builder)
        if(_count_${_f} EQUAL 0)
            message(FATAL_ERROR "${BINARY} defines no ${_f} symbol: the census cannot see it")
        endif()
    endforeach()
else()
    message(FATAL_ERROR "ROUTE must be vellum or legacy, not '${ROUTE}'")
endif()
