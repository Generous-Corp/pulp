if(TARGET sample-region-allpass-core AND TARGET pulp::inspect-standalone-runtime)
    set(_control_source_dir "${CMAKE_CURRENT_LIST_DIR}")
    foreach(_kind IN ITEMS editable frozen)
        set(_target sample-region-allpass-control-${_kind})
        add_executable(${_target} "${_control_source_dir}/control_${_kind}_standalone.cpp")
        target_link_libraries(${_target} PRIVATE
            sample-region-allpass-core pulp::inspect-standalone-runtime pulp::format)
        set(_capabilities
            dev.pulp.instance/read@1
            dev.pulp.session/control@1
            dev.pulp.state/read@1
            dev.pulp.state/parameter-gesture@1
            dev.pulp.graph/sample-region.read@1)
        if(_kind STREQUAL "editable")
            list(APPEND _capabilities dev.pulp.graph/sample-region.edit@1)
        endif()
        _pulp_cache_control_declarations(${_target} developer-local "${_capabilities}" FALSE)
        _pulp_configure_control_shipping(${_target}
            "dev.pulp.sample-region-allpass.${_kind}" "Sample Region Allpass ${_kind}")
        _pulp_attach_control_shipping(${_target} ${_target} Standalone)
        if(APPLE)
            find_program(_sample_region_codesign codesign REQUIRED)
            add_custom_command(TARGET ${_target} POST_BUILD
                COMMAND "${_sample_region_codesign}" --force --sign - --options library
                    "$<TARGET_FILE:${_target}>"
                COMMAND chmod 0700 "$<TARGET_FILE:${_target}>"
                COMMAND chmod 0600 "$<TARGET_FILE:${_target}>.inspector-capabilities.json"
                VERBATIM)
        endif()
    endforeach()
endif()
