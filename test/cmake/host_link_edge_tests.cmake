# pulp::host is the plug-in hosting and SignalGraph library. It links the
# format core, not the pulp::format umbrella: the umbrella carries
# pulp::format-view and with it Pulp's whole view stack, which no host consumer
# should inherit. This walks the host's link closure and has ctest check it.
# pulp_link_closure(<out-var> <root-target> [LEAVES <target>...])
#
# The set of target names reachable from <root-target> through
# INTERFACE_LINK_LIBRARIES (and LINK_LIBRARIES for a real library), aliases
# resolved, generator-expression wrappers such as $<LINK_ONLY:x> unwrapped, and
# the root itself included. A target in LEAVES is listed but not walked.
# Call it after every target it can reach has been defined.
function(pulp_link_closure out_var root)
    cmake_parse_arguments(PARSE_ARGV 2 _plc "" "" "LEAVES")
    set(_pending "${root}")
    set(_seen "")
    while(_pending)
        list(POP_FRONT _pending _name)
        if(_name MATCHES "^\\$<")
            string(REGEX REPLACE "^\\$<[A-Z_]+:(.*)>$" "\\1" _name "${_name}")
        endif()
        if(NOT _name OR _name MATCHES "[<>$]" OR "${_name}" IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_name}")
        if(NOT TARGET "${_name}" OR "${_name}" IN_LIST _plc_LEAVES)
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
            if(_type STREQUAL "INTERFACE_LIBRARY" AND _prop STREQUAL "LINK_LIBRARIES")
                continue()
            endif()
            get_target_property(_value "${_node}" ${_prop})
            if(_value)
                list(APPEND _deps ${_value})
            endif()
        endforeach()
        list(APPEND _pending ${_deps})
    endwhile()
    set(${out_var} "${_seen}" PARENT_SCOPE)
endfunction()


if(TARGET pulp::host)
    set(_host_forbidden
        pulp::format pulp::format-view
        pulp::view pulp::view-core pulp::view-tooling pulp::view-audio pulp::view-script
        pulp::view-native pulp::view-graph-editor pulp::canvas pulp::render)
    pulp_link_closure(_host_closure pulp::host)
    set(_host_closure_file "${CMAKE_CURRENT_BINARY_DIR}/host_link_closure.txt")
    string(REPLACE ";" "\n" _host_closure_text "${_host_closure}")
    file(WRITE "${_host_closure_file}" "${_host_closure_text}\n")
    # The negative control is the same closure with the umbrella added back.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/host_link_closure_with_umbrella.txt"
        "${_host_closure_text}\npulp::format\n")

    # The check script takes comma-separated lists: a semicolon in an add_test
    # argument would split it.
    string(REPLACE ";" "," _host_forbidden_arg "${_host_forbidden}")
    set(_host_required_arg "pulp::host,pulp::format-core,pulp::runtime")
    add_test(NAME pulp-host-link-edges
        COMMAND ${CMAKE_COMMAND}
            "-DCLOSURE_FILE=${_host_closure_file}"
            "-DREQUIRED=${_host_required_arg}"
            "-DFORBIDDEN=${_host_forbidden_arg}"
            -DWHAT=pulp::host
            -P "${PULP_ROOT_DIR}/tools/cmake/check_link_closure.cmake")
    # The control passes only when the check fails because the umbrella is
    # present: the regular expression decides, the exit code does not.
    add_test(NAME pulp-host-link-edges-negative-control
        COMMAND ${CMAKE_COMMAND}
            "-DCLOSURE_FILE=${CMAKE_CURRENT_BINARY_DIR}/host_link_closure_with_umbrella.txt"
            "-DREQUIRED=${_host_required_arg}"
            "-DFORBIDDEN=${_host_forbidden_arg}"
            -DWHAT=pulp::host
            -P "${PULP_ROOT_DIR}/tools/cmake/check_link_closure.cmake")
    set_tests_properties(pulp-host-link-edges-negative-control PROPERTIES
        PASS_REGULAR_EXPRESSION "pulp::host reaches pulp::format([^-]|$)")
    set_tests_properties(pulp-host-link-edges pulp-host-link-edges-negative-control
        PROPERTIES LABELS "host-link-edges")
endif()
