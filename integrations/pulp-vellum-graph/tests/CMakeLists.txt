find_package(Threads REQUIRED)
if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
    add_executable(pulp-test-pulp-vellum-graph
        ${CMAKE_CURRENT_SOURCE_DIR}/graph_editor_view_test.cpp)
    target_link_libraries(pulp-test-pulp-vellum-graph PRIVATE
        pulp::vellum-graph Catch2::Catch2WithMain Threads::Threads)
    include(Catch)
    catch_discover_tests(pulp-test-pulp-vellum-graph PROPERTIES LABELS pulp-vellum-graph)

    # The editor must link pulp::host and Vellum's view layer and none of
    # Pulp's own view stack directly. Walk the link closure now, while every
    # target exists, and let ctest run the check script over the result.
    # pulp::host is a leaf of the walk: it links the pulp::format umbrella,
    # which carries pulp::format-view and with it Pulp's view stack, so what is
    # reached through the host belongs to the host's own link line.
    function(_pulp_vellum_graph_closure out_var root)
        set(_pending "${root}")
        set(_seen "")
        while(_pending)
            list(POP_FRONT _pending _name)
            if(_name MATCHES "^\\$<")
                # $<LINK_ONLY:x> and $<BUILD_INTERFACE:x> wrap a real target.
                string(REGEX REPLACE "^\\$<[A-Z_]+:(.*)>$" "\\1" _name "${_name}")
            endif()
            if(NOT _name OR _name MATCHES "[<>$]" OR "${_name}" IN_LIST _seen)
                continue()
            endif()
            list(APPEND _seen "${_name}")
            if(NOT TARGET "${_name}")
                continue()
            endif()
            if(_name STREQUAL "pulp::host")
                continue()
            endif()
            get_target_property(_aliased "${_name}" ALIASED_TARGET)
            set(_node "${_name}")
            if(_aliased)
                set(_node "${_aliased}")
            endif()
            get_target_property(_type "${_node}" TYPE)
            set(_deps "")
            foreach(_prop IN ITEMS INTERFACE_LINK_LIBRARIES LINK_LIBRARIES)
                if(NOT _type STREQUAL "INTERFACE_LIBRARY" OR _prop STREQUAL "INTERFACE_LINK_LIBRARIES")
                    get_target_property(_value "${_node}" ${_prop})
                    if(_value)
                        list(APPEND _deps ${_value})
                    endif()
                endif()
            endforeach()
            list(APPEND _pending ${_deps})
        endwhile()
        set(${out_var} "${_seen}" PARENT_SCOPE)
    endfunction()

    _pulp_vellum_graph_closure(_graph_closure pulp::vellum-graph)
    set(_closure_file "${CMAKE_CURRENT_BINARY_DIR}/graph_link_closure.txt")
    string(REPLACE ";" "\n" _closure_text "${_graph_closure}")
    file(WRITE "${_closure_file}" "${_closure_text}\n")
    # The negative control: the same closure with a forbidden target added.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/graph_link_closure_with_view.txt"
        "${_closure_text}\npulp::view-core\n")

    add_test(NAME pulp-vellum-graph-link-edges
        COMMAND ${CMAKE_COMMAND} -DCLOSURE_FILE=${_closure_file}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_link_edges.cmake)
    add_test(NAME pulp-vellum-graph-link-edges-negative-control
        COMMAND ${CMAKE_COMMAND}
            -DCLOSURE_FILE=${CMAKE_CURRENT_BINARY_DIR}/graph_link_closure_with_view.txt
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_link_edges.cmake)
    # The control passes only when the check fails for the forbidden target:
    # the regular expression decides, the process exit code does not.
    set_tests_properties(pulp-vellum-graph-link-edges-negative-control PROPERTIES
        PASS_REGULAR_EXPRESSION "the graph editor reaches pulp::view-core")
    set_tests_properties(pulp-vellum-graph-link-edges
        pulp-vellum-graph-link-edges-negative-control PROPERTIES LABELS pulp-vellum-graph)
endif()
