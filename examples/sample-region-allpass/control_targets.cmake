# The full standalone control host is the GPU-backed macOS implementation;
# non-Apple/minimal hosts deliberately fail closed and cannot make these
# product capabilities reachable. Do not emit a shipping manifest for them.
if(TARGET sample-region-allpass-core AND TARGET pulp::inspect-standalone-runtime
   AND APPLE AND PULP_ENABLE_GPU AND NOT IOS AND NOT PULP_IOS)
    set(_control_source_dir "${CMAKE_CURRENT_LIST_DIR}")
    foreach(_kind IN ITEMS editable frozen)
        set(_target sample-region-allpass-control-${_kind})
        add_executable(${_target} "${_control_source_dir}/control_${_kind}_standalone.cpp")
        target_link_libraries(${_target} PRIVATE
            sample-region-allpass-core pulp::inspect-standalone-runtime pulp::standalone
            pulp::format)
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
            # These hosts are ad-hoc signed with --options library below.
            # Library validation admits only platform-signed or same-Team-ID
            # libraries, and an ad-hoc signature has no Team ID, so any
            # third-party dylib on the link makes the binary unexecutable:
            # dyld refuses it at exec ("different Team IDs") and the process
            # dies before main(). These link pulp::inspect-standalone-runtime,
            # which reaches libwgpu_native.dylib, yet import zero symbols from
            # it - the wgpu code they do use is statically linked. Dropping the
            # unused load command is what keeps them runnable; -dead_strip
            # removes unused CODE but never an LC_LOAD_DYLIB entry.
            target_link_options(${_target} PRIVATE LINKER:-dead_strip_dylibs)
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
