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

# Names a property value refers to: every word of the value with generator
# expression syntax stripped, so `$<LINK_ONLY:dep>`, `$<TARGET_OBJECTS:objs>`
# and a plain `dep` all yield the target name. "::" is kept inside a name.
function(_pulp_link_closure_words value out_var)
    string(REPLACE "::" "@NS@" _text "${value}")
    string(REGEX MATCHALL "[A-Za-z0-9_+.@-]+" _words "${_text}")
    set(_restored "")
    foreach(_word IN LISTS _words)
        string(REPLACE "@NS@" "::" _word "${_word}")
        list(APPEND _restored "${_word}")
    endforeach()
    set(${out_var} "${_restored}" PARENT_SCOPE)
endfunction()

# Every buildsystem target, plus every target they reach by name (imported
# targets, aliases and the targets of $<TARGET_OBJECTS:...>), with the
# properties a closure walk follows: LINK_LIBRARIES, INTERFACE_LINK_LIBRARIES,
# SOURCES and INTERFACE_SOURCES, unevaluated. An alias records one edge to the
# target it names. The record is `name<TAB>type<TAB>link<TAB>iface<TAB>sources<TAB>refs`, where refs lists
# the words that were targets when written, so a checker can tell a target with
# a missing record from an external library name.
function(_pulp_link_closure_write_graph)
    _pulp_link_closure_collect("${CMAKE_SOURCE_DIR}" _queue)
    list(REMOVE_DUPLICATES _queue)
    set(_seen "")
    set(_content "")
    while(_queue)
        list(POP_FRONT _queue _target)
        if(_target IN_LIST _seen OR NOT TARGET "${_target}")
            continue()
        endif()
        list(APPEND _seen "${_target}")
        get_target_property(_type ${_target} TYPE)
        get_target_property(_aliased ${_target} ALIASED_TARGET)
        set(_link "")
        set(_iface "")
        set(_sources "")
        if(_aliased)
            set(_iface "${_aliased}")
        else()
            foreach(_entry IN ITEMS "_link;LINK_LIBRARIES" "_iface;INTERFACE_LINK_LIBRARIES"
                    "_src;SOURCES" "_isrc;INTERFACE_SOURCES")
                list(GET _entry 0 _var)
                list(GET _entry 1 _property)
                get_target_property(_value ${_target} ${_property})
                if(NOT _value OR _value MATCHES "-NOTFOUND$")
                    set(_value "")
                endif()
                set(${_var} "${_value}")
            endforeach()
            set(_sources "${_src};${_isrc}")
        endif()
        set(_refs "")
        foreach(_value IN ITEMS "${_link}" "${_iface}" "${_sources}")
            _pulp_link_closure_words("${_value}" _words)
            foreach(_word IN LISTS _words)
                if(TARGET "${_word}")
                    list(APPEND _refs "${_word}")
                    if(NOT _word IN_LIST _seen)
                        list(APPEND _queue "${_word}")
                    endif()
                endif()
            endforeach()
        endforeach()
        list(REMOVE_DUPLICATES _refs)
        string(REGEX REPLACE "[\t\n\r]+" ";" _link "${_link}")
        string(REGEX REPLACE "[\t\n\r]+" ";" _iface "${_iface}")
        string(REGEX REPLACE "[\t\n\r]+" ";" _sources "${_sources}")
        string(APPEND _content "${_target}\t${_type}\t${_link}\t${_iface}\t${_sources}\t${_refs}\n")
    endwhile()
    file(WRITE "${CMAKE_BINARY_DIR}/link-closure-graph.tsv" "${_content}")
endfunction()

# Registers the deferred write once, however many manifests include this file.
get_property(_pulp_link_closure_registered GLOBAL PROPERTY PULP_LINK_CLOSURE_GRAPH_REGISTERED)
if(NOT _pulp_link_closure_registered)
    set_property(GLOBAL PROPERTY PULP_LINK_CLOSURE_GRAPH_REGISTERED TRUE)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _pulp_link_closure_write_graph)
endif()

if(Python3_Interpreter_FOUND)
    add_test(NAME link-closure-graph-writer
        COMMAND ${CMAKE_COMMAND}
                -DSOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/../fixtures/link_closure_graph_project
                -DBINARY_DIR=${CMAKE_BINARY_DIR}/link-closure-graph-project
                -DCHECKER=${CMAKE_SOURCE_DIR}/tools/scripts/link_closure_check.py
                -DPYTHON=${Python3_EXECUTABLE}
                -DTEST_DIR=${CMAKE_CURRENT_LIST_DIR}/..
                -P ${CMAKE_CURRENT_LIST_DIR}/check_link_closure_graph_writer.cmake)
    set_tests_properties(link-closure-graph-writer PROPERTIES
        LABELS "link-closure" TIMEOUT 120)
    add_test(NAME link-closure-check-rules
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/scripts/test_link_closure_check.py)
    set_tests_properties(link-closure-check-rules PROPERTIES
        LABELS "link-closure" TIMEOUT 60)
endif()
