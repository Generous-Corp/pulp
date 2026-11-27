# Inspect control stack closure: the control stack (protocol, broker, grants,
# executors, telemetry, observability) builds without pulp::view, pulp::canvas
# or pulp::render. The view side of inspect/ (overlay, window, motion tools,
# script inspector, Standalone UI adapters) is a separate set of targets that
# moves with the UI layer and is named, not checked, here.
#
#   inspect-control-link-closure         transitive link walk of the control roots
#   inspect-control-link-closure-format  the roots that link pulp::format, with the
#                                        one edge format still owes named
#   inspect-control-link-closure-control a control chain that reaches pulp::view-core
#                                        through an intermediate static archive by a
#                                        PRIVATE link; the walk must report it
#   inspect-view-side-link-closure-control  the real view-side target must be
#                                        reported, so the walk sees real edges
#   inspect-control-includes             no control-stack source includes a UI header
#
# Registered from quality_tests.cmake; the link graph is link_closure_graph.cmake.

if(Python3_Interpreter_FOUND AND TARGET pulp-inspect-runtime AND TARGET pulp-inspect-ui-runtime)
    set(_pulp_inspect_closure_checker "${CMAKE_SOURCE_DIR}/tools/scripts/link_closure_check.py")
    set(_pulp_inspect_closure_graph "${CMAKE_BINARY_DIR}/link-closure-graph.tsv")
    set(_pulp_inspect_forbidden
        --forbid "pulp-view*" --forbid "pulp-canvas*" --forbid "pulp-render"
        --forbid "pulp-design*" --forbid "pulp-bundled*" --forbid "pulp-vellum*")

    set(_pulp_inspect_control_roots
        pulp-inspect-protocol pulp-inspect-control pulp-inspect-runtime
        pulp-inspect-telemetry pulp-inspect-observability-runtime pulp-inspect-authoring)
    foreach(_optional IN ITEMS pulp-inspect-client pulp-control-broker
            pulp-inspect-sample-region-runtime pulp-inspect-graph-runtime)
        if(TARGET ${_optional})
            list(APPEND _pulp_inspect_control_roots ${_optional})
        endif()
    endforeach()
    set(_pulp_inspect_root_args "")
    foreach(_root IN LISTS _pulp_inspect_control_roots)
        list(APPEND _pulp_inspect_root_args --root ${_root})
    endforeach()
    add_test(NAME inspect-control-link-closure
        COMMAND ${Python3_EXECUTABLE} "${_pulp_inspect_closure_checker}"
                --graph "${_pulp_inspect_closure_graph}"
                ${_pulp_inspect_root_args} ${_pulp_inspect_forbidden})

    # pulp::format links pulp-format-view, which carries the view layer until the
    # Processor editor seam leaves it. These roots are checked with that one edge
    # allowed and named in the output; any other path to the view layer fails.
    set(_pulp_inspect_format_root_args "")
    foreach(_root IN ITEMS pulp-inspect-offline-runtime pulp-inspect-agent-toolkit)
        if(TARGET ${_root})
            list(APPEND _pulp_inspect_format_root_args --root ${_root})
        endif()
    endforeach()
    if(_pulp_inspect_format_root_args)
        add_test(NAME inspect-control-link-closure-format
            COMMAND ${Python3_EXECUTABLE} "${_pulp_inspect_closure_checker}"
                    --graph "${_pulp_inspect_closure_graph}"
                    ${_pulp_inspect_format_root_args} ${_pulp_inspect_forbidden}
                    --allow "pulp-format>pulp-format-view")
    endif()

    add_library(pulp-inspect-closure-control-leaf STATIC EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/link_closure_control/empty.cpp)
    target_link_libraries(pulp-inspect-closure-control-leaf PRIVATE pulp::view-core)
    add_library(pulp-inspect-closure-control INTERFACE)
    target_link_libraries(pulp-inspect-closure-control INTERFACE
        pulp::inspect-runtime pulp-inspect-closure-control-leaf)
    add_test(NAME inspect-control-link-closure-control
        COMMAND ${Python3_EXECUTABLE} "${_pulp_inspect_closure_checker}"
                --graph "${_pulp_inspect_closure_graph}"
                --root pulp-inspect-closure-control ${_pulp_inspect_forbidden})
    add_test(NAME inspect-view-side-link-closure-control
        COMMAND ${Python3_EXECUTABLE} "${_pulp_inspect_closure_checker}"
                --graph "${_pulp_inspect_closure_graph}"
                --root pulp-inspect-ui-runtime ${_pulp_inspect_forbidden})
    set_tests_properties(inspect-control-link-closure-control PROPERTIES
        PASS_REGULAR_EXPRESSION
            "FAIL pulp-inspect-closure-control reaches pulp-view-core: .*pulp-inspect-closure-control-leaf -> pulp-view-core")
    set_tests_properties(inspect-view-side-link-closure-control PROPERTIES
        PASS_REGULAR_EXPRESSION "FAIL pulp-inspect-ui-runtime reaches pulp-view")

    add_test(NAME inspect-control-includes
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scripts/inspect_control_include_check.py"
                --inspect-dir "${CMAKE_SOURCE_DIR}/inspect")
    add_test(NAME inspect-control-includes-rules
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scripts/test_inspect_control_include_check.py")
    set_tests_properties(
        inspect-control-link-closure inspect-control-link-closure-control
        inspect-view-side-link-closure-control inspect-control-includes
        inspect-control-includes-rules
        PROPERTIES LABELS "inspect;control;link-closure" TIMEOUT 60)
    if(TEST inspect-control-link-closure-format)
        set_tests_properties(inspect-control-link-closure-format
            PROPERTIES LABELS "inspect;control;link-closure" TIMEOUT 60)
    endif()
    unset(_pulp_inspect_closure_checker)
    unset(_pulp_inspect_closure_graph)
    unset(_pulp_inspect_forbidden)
    unset(_pulp_inspect_control_roots)
    unset(_pulp_inspect_root_args)
    unset(_pulp_inspect_format_root_args)
endif()
