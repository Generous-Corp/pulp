# Link graph for the transitive link-closure checks.
#
# Writes one `name<TAB>type<TAB>LINK_LIBRARIES<TAB>INTERFACE_LINK_LIBRARIES`
# record for every Pulp buildsystem target, with the properties unevaluated:
# a PRIVATE link of a static library is recorded as $<LINK_ONLY:dep>, and the
# closure walk follows it. The write is deferred to the end of the top-level
# directory so every target exists.
#
# The graph lands in ${CMAKE_BINARY_DIR}/link-closure-graph.tsv. Each closure
# check registered from a test manifest reads it, so the walk covers the real
# configured graph rather than a hand-listed approximation.

function(_pulp_link_closure_collect dir out_var)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        _pulp_link_closure_collect("${_sub}" _sub_targets)
        list(APPEND _targets ${_sub_targets})
    endforeach()
    set(${out_var} "${_targets}" PARENT_SCOPE)
endfunction()

function(_pulp_link_closure_write_graph)
    _pulp_link_closure_collect("${CMAKE_SOURCE_DIR}" _all)
    list(REMOVE_DUPLICATES _all)
    set(_content "")
    foreach(_target IN LISTS _all)
        if(NOT _target MATCHES "^pulp-")
            continue()
        endif()
        get_target_property(_type ${_target} TYPE)
        get_target_property(_link ${_target} LINK_LIBRARIES)
        get_target_property(_iface ${_target} INTERFACE_LINK_LIBRARIES)
        foreach(_var _link _iface)
            if(NOT ${_var} OR ${_var} MATCHES "-NOTFOUND$")
                set(${_var} "")
            endif()
        endforeach()
        string(APPEND _content "${_target}\t${_type}\t${_link}\t${_iface}\n")
    endforeach()
    file(WRITE "${CMAKE_BINARY_DIR}/link-closure-graph.tsv" "${_content}")
endfunction()

# Registers the deferred write once, however many manifests include this file.
get_property(_pulp_link_closure_registered GLOBAL PROPERTY PULP_LINK_CLOSURE_GRAPH_REGISTERED)
if(NOT _pulp_link_closure_registered)
    set_property(GLOBAL PROPERTY PULP_LINK_CLOSURE_GRAPH_REGISTERED TRUE)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _pulp_link_closure_write_graph)
endif()

if(Python3_Interpreter_FOUND)
    add_test(NAME link-closure-check-rules
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/scripts/test_link_closure_check.py)
    set_tests_properties(link-closure-check-rules PROPERTIES
        LABELS "link-closure" TIMEOUT 60)
endif()
