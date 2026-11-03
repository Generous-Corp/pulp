# Checks the record CMakeLists.txt writes for the editor UI package fixture:
# a plug-in that declares UI <package>, and nothing else about its editor,
# links no Pulp view, canvas or render target and no view-linked format
# library. The default-editor control must link them, which is what shows the
# check can see a view layer at all.
cmake_minimum_required(VERSION 3.24)
include("${RECORD}")

set(_view_pattern "^(pulp-format|pulp-format-view|pulp-view.*|pulp-canvas.*|pulp-render.*)$")

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
foreach(_variant A B)
    _view_targets(_found "${${_variant}_CLOSURE}")
    if(_found)
        list(APPEND _failures "UI package plug-in ${_variant} links view targets: ${_found}")
    endif()
    if(NOT "pulp-format-core" IN_LIST ${_variant}_CLOSURE)
        list(APPEND _failures "UI package plug-in ${_variant} lost pulp-format-core: ${${_variant}_CLOSURE}")
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
