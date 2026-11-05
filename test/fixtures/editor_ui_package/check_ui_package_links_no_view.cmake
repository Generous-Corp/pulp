# Checks the record CMakeLists.txt writes for the editor UI package fixture:
# a plug-in that declares UI <package>, and nothing else about its editor,
# links no Pulp view, canvas, render or GPU-device target and no view-linked
# format or standalone library, in its plug-in modules and in its standalone
# (whose Dawn may then come only from the UI package). The default-editor
# control must link them, which is what shows the check can see a view layer
# at all.
cmake_minimum_required(VERSION 3.24)
include("${RECORD}")

set(_view_pattern "^(pulp-format|pulp-format-view|pulp-view.*|pulp-canvas.*|pulp-render.*|pulp-gpu-device.*|pulp-standalone|pulp-standalone-native)$")

function(_view_targets out closure)
    set(_found "")
    foreach(_item IN LISTS closure)
        if(_item MATCHES "${_view_pattern}")
            list(APPEND _found "${_item}")
        endif()
    endforeach()
    set(${out} "${_found}" PARENT_SCOPE)
endfunction()

set(_failures "")
set(_package_variants A B)
if(DEFINED AU_CLOSURE)
    list(APPEND _package_variants AU)
endif()
foreach(_variant IN LISTS _package_variants)
    _view_targets(_found "${${_variant}_CLOSURE}")
    if(_found)
        list(APPEND _failures "UI package plug-in ${_variant} links view targets: ${_found}")
    endif()
    if(NOT "pulp-format-core" IN_LIST ${_variant}_CLOSURE)
        list(APPEND _failures "UI package plug-in ${_variant} lost pulp-format-core: ${${_variant}_CLOSURE}")
    endif()
endforeach()

# The UI package plug-in's standalone: the view-free standalone library, and
# nothing of the view layer.
_view_targets(_found "${A_STANDALONE_CLOSURE}")
if(_found)
    list(APPEND _failures "UI package plug-in A's standalone links view targets: ${_found}")
endif()
if(NOT "pulp-standalone-contract" IN_LIST A_STANDALONE_CLOSURE)
    list(APPEND _failures "UI package plug-in A's standalone does not link pulp-standalone-contract: ${A_STANDALONE_CLOSURE}")
endif()
# Its control: the default-editor plug-in's standalone links the view
# standalone and the view layer.
_view_targets(_standalone_control "${ViewControl_STANDALONE_CLOSURE}")
foreach(_needed pulp-standalone pulp-view)
    if(NOT _needed IN_LIST _standalone_control)
        list(APPEND _failures "the default-editor control's standalone does not link ${_needed}, so the standalone check is blind: ${ViewControl_STANDALONE_CLOSURE}")
    endif()
endforeach()

# Positive control.
_view_targets(_control "${ViewControl_CLOSURE}")
set(_control_families "")
foreach(_family pulp-view pulp-canvas)
    set(_seen FALSE)
    foreach(_item IN LISTS _control)
        if(_item MATCHES "^${_family}")
            set(_seen TRUE)
        endif()
    endforeach()
    if(NOT _seen)
        list(APPEND _failures "the default-editor control links no ${_family}* target, so the check is blind: ${ViewControl_CLOSURE}")
    endif()
endforeach()

if(_failures)
    list(JOIN _failures "\n" _text)
    message(FATAL_ERROR "${_text}")
endif()
list(LENGTH _control _count)
message(STATUS "UI package plug-ins link no view target; the default-editor control links ${_count}")
