# Checks that a plug-in module exports none of the editor-hook symbols.
#
#   cmake -DNM=<nm> -DBINARY=<module> -DFORBIDDEN=<substr;...> -DREQUIRED=<substr>
#       -P check_hook_not_exported.cmake
#
# The hook pulp_add_plugin(... UI <package>) generates has internal linkage,
# and pulp-format-core's install_plugin_editor_factory_hook() and
# plugin_editor_factory() are hidden, so no module may export them: a second
# plug-in in the same host could otherwise bind to this one's choice. Every
# plug-in module also exports only its format's entry points (an
# exported-symbols list on macOS, a version script on Linux), which covers the
# same ground a second time; Windows DLLs export nothing that is not declared
# for export. This reads what the built module actually exports.
#
# REQUIRED names a symbol the module must export (its entry point): the
# control that shows the symbol listing is not empty.
cmake_minimum_required(VERSION 3.24)

execute_process(COMMAND "${NM}" -g "${BINARY}"
    OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${NM} -g ${BINARY} failed (${_rc}): ${_err}")
endif()

string(REPLACE "\n" ";" _lines "${_out}")
set(_exported "")
foreach(_line IN LISTS _lines)
    # "<address> <type> <name>"; undefined references carry type U and no
    # address. Only defined symbols are exports.
    if(_line MATCHES "^[0-9a-fA-F]+ +([A-Za-z]) +(.+)$")
        if(NOT CMAKE_MATCH_1 STREQUAL "U")
            list(APPEND _exported "${CMAKE_MATCH_2}")
        endif()
    endif()
endforeach()

set(_found_required FALSE)
set(_violations "")
foreach(_symbol IN LISTS _exported)
    if(_symbol MATCHES "${REQUIRED}")
        set(_found_required TRUE)
    endif()
    foreach(_forbidden IN LISTS FORBIDDEN)
        string(FIND "${_symbol}" "${_forbidden}" _at)
        if(_at GREATER_EQUAL 0)
            list(APPEND _violations "${_symbol}")
        endif()
    endforeach()
endforeach()

if(NOT _found_required)
    message(FATAL_ERROR "${BINARY} exports no symbol matching ${REQUIRED}: the listing is blind")
endif()
if(_violations)
    list(JOIN _violations "\n  " _text)
    message(FATAL_ERROR "${BINARY} exports editor-hook symbols:\n  ${_text}")
endif()
list(LENGTH _exported _count)
message(STATUS "${BINARY}: ${_count} exported symbols, none of them an editor hook")
