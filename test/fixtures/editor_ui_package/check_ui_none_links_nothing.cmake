# Checks the record CMakeLists.txt writes for the editor UI package fixture:
#   - UI none and no UI argument configure the same target graph and sources;
#   - neither carries the generated hook source or a UI package;
#   - the UI package plug-in differs from them by exactly its package and the
#     generated hook source, which is the positive control: a record that
#     shows no difference there means the check is blind, not that UI none is
#     clean.
cmake_minimum_required(VERSION 3.24)
include("${RECORD}")

set(_failures "")
macro(_fail message)
    list(APPEND _failures "${message}")
endmacro()

if(NOT None_CLOSURE STREQUAL Plain_CLOSURE)
    _fail("UI none and no UI link different targets:\n  none:  ${None_CLOSURE}\n  plain: ${Plain_CLOSURE}")
endif()
if(NOT None_SOURCES STREQUAL Plain_SOURCES)
    _fail("UI none and no UI compile different sources:\n  none:  ${None_SOURCES}\n  plain: ${Plain_SOURCES}")
endif()

foreach(_variant None Plain)
    if(${_variant}_PACKAGE AND NOT ${_variant}_PACKAGE MATCHES "-NOTFOUND$")
        _fail("${_variant} records UI package ${${_variant}_PACKAGE}")
    endif()
    foreach(_source IN LISTS ${_variant}_SOURCES)
        if(_source MATCHES "/pulp_editor_package/")
            _fail("${_variant} compiles the generated hook source ${_source}")
        endif()
    endforeach()
endforeach()

# Positive control.
set(_hook_sources "")
foreach(_source IN LISTS A_SOURCES)
    if(_source MATCHES "/pulp_editor_package/")
        list(APPEND _hook_sources "${_source}")
    endif()
endforeach()
list(LENGTH _hook_sources _hook_count)
if(NOT _hook_count EQUAL 1)
    _fail("the UI package plug-in compiles ${_hook_count} generated hook sources, expected 1: ${A_SOURCES}")
endif()
if(NOT A_PACKAGE STREQUAL "pulp-test-ui-package-a")
    _fail("the UI package plug-in records UI package '${A_PACKAGE}'")
endif()
set(_only_in_a ${A_CLOSURE})
list(REMOVE_ITEM _only_in_a ${None_CLOSURE})
set(_only_in_none ${None_CLOSURE})
list(REMOVE_ITEM _only_in_none ${A_CLOSURE})
if(NOT _only_in_a STREQUAL "pulp-test-ui-package-a" OR _only_in_none)
    _fail("the UI package plug-in should add exactly its package to the link:\n  only with the package: ${_only_in_a}\n  only without it: ${_only_in_none}")
endif()

if(_failures)
    list(JOIN _failures "\n" _text)
    message(FATAL_ERROR "${_text}")
endif()
list(LENGTH None_CLOSURE _count)
message(STATUS "UI none and no UI link the same ${_count} targets; the UI package adds only pulp-test-ui-package-a")
