# Pulp install/export and runtime-manifest helpers.
# Included by PulpUtils.cmake to preserve its public compatibility surface.

function(_pulp_json_string_array out_var)
    set(_json "[")
    set(_first TRUE)
    foreach(_value IN LISTS ARGN)
        if(_value MATCHES "[\"\\\\]")
            message(FATAL_ERROR
                "pulp_add_plugin: plugin runtime manifest values must not "
                "contain quotes or backslashes: '${_value}'")
        endif()
        if(NOT _first)
            string(APPEND _json ", ")
        endif()
        string(APPEND _json "\"${_value}\"")
        set(_first FALSE)
    endforeach()
    string(APPEND _json "]")
    set(${out_var} "${_json}" PARENT_SCOPE)
endfunction()

function(_pulp_configure_plugin_runtime_manifest target bundle_id)
    set(_capabilities ${PULP_${target}_CONTENT_CAPABILITIES})
    set(_kinds ${PULP_${target}_CONTENT_KINDS})
    set(_hot_reload_kinds ${PULP_${target}_CONTENT_HOT_RELOAD_KINDS})
    set(_manual_rescan_kinds ${PULP_${target}_CONTENT_MANUAL_RESCAN_KINDS})
    set(_pulp_valid_content_kinds presets themes samples sample-banks wavetables)

    if(NOT _capabilities AND NOT _kinds)
        if(_hot_reload_kinds OR _manual_rescan_kinds)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): CONTENT_HOT_RELOAD_KINDS and "
                "CONTENT_MANUAL_RESCAN_KINDS require CONTENT_CAPABILITIES "
                "and CONTENT_KINDS.")
        endif()
        set(PULP_${target}_PLUGIN_RUNTIME_MANIFEST "" CACHE INTERNAL "")
        return()
    endif()
    if(NOT _capabilities OR NOT _kinds)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): CONTENT_CAPABILITIES and "
            "CONTENT_KINDS must be provided together.")
    endif()
    if(NOT bundle_id)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): BUNDLE_ID is required when "
            "CONTENT_CAPABILITIES / CONTENT_KINDS are declared.")
    endif()

    foreach(_kind IN LISTS _kinds)
        if(NOT _kind IN_LIST _pulp_valid_content_kinds)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): unsupported CONTENT_KINDS "
                "'${_kind}'. Expected one of: presets, themes, samples, "
                "sample-banks, wavetables.")
        endif()
    endforeach()
    foreach(_kind IN LISTS _hot_reload_kinds)
        if(NOT _kind IN_LIST _kinds)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): CONTENT_HOT_RELOAD_KINDS "
                "'${_kind}' must also be listed in CONTENT_KINDS.")
        endif()
    endforeach()
    foreach(_kind IN LISTS _manual_rescan_kinds)
        if(NOT _kind IN_LIST _kinds)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): CONTENT_MANUAL_RESCAN_KINDS "
                "'${_kind}' must also be listed in CONTENT_KINDS.")
        endif()
    endforeach()

    _pulp_json_string_array(_capabilities_json ${_capabilities})
    _pulp_json_string_array(_kinds_json ${_kinds})
    _pulp_json_string_array(_hot_reload_json ${_hot_reload_kinds})
    _pulp_json_string_array(_manual_rescan_json ${_manual_rescan_kinds})

    set(_manifest "${CMAKE_CURRENT_BINARY_DIR}/${target}_pulp.plugin-runtime.json")
    file(WRITE "${_manifest}" "{\n")
    file(APPEND "${_manifest}" "  \"schema\": \"pulp.plugin-runtime.v1\",\n")
    file(APPEND "${_manifest}" "  \"pluginId\": \"${bundle_id}\",\n")
    file(APPEND "${_manifest}" "  \"content\": {\n")
    file(APPEND "${_manifest}" "    \"capabilities\": ${_capabilities_json},\n")
    file(APPEND "${_manifest}" "    \"kinds\": ${_kinds_json}")
    if(NOT "${_hot_reload_kinds}" STREQUAL "" OR NOT "${_manual_rescan_kinds}" STREQUAL "")
        file(APPEND "${_manifest}" ",\n")
        file(APPEND "${_manifest}" "    \"reload\": {\n")
        file(APPEND "${_manifest}" "      \"hotReloadKinds\": ${_hot_reload_json},\n")
        file(APPEND "${_manifest}" "      \"manualRescanKinds\": ${_manual_rescan_json}\n")
        file(APPEND "${_manifest}" "    }\n")
    else()
        file(APPEND "${_manifest}" "\n")
    endif()
    file(APPEND "${_manifest}" "  }\n")
    file(APPEND "${_manifest}" "}\n")

    set(PULP_${target}_PLUGIN_RUNTIME_MANIFEST "${_manifest}" CACHE INTERNAL "")
endfunction()

function(_pulp_attach_plugin_runtime_manifest target format_target)
    set(_manifest "${PULP_${target}_PLUGIN_RUNTIME_MANIFEST}")
    if(NOT _manifest)
        return()
    endif()
    if(NOT TARGET ${format_target})
        return()
    endif()

    if("${format_target}" MATCHES "_LV2$")
        add_custom_command(TARGET ${format_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_manifest}"
                "$<TARGET_FILE_DIR:${format_target}>/pulp.plugin-runtime.json"
            COMMENT "Embedding pulp.plugin-runtime.json into ${format_target} LV2 bundle"
            VERBATIM
        )
    elseif(APPLE AND PULP_IOS)
        add_custom_command(TARGET ${format_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:${format_target}>/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_manifest}"
                "$<TARGET_BUNDLE_DIR:${format_target}>/Resources/pulp.plugin-runtime.json"
            COMMENT "Embedding pulp.plugin-runtime.json into ${format_target} flat bundle resources"
            VERBATIM
        )
    elseif(APPLE)
        add_custom_command(TARGET ${format_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:${format_target}>/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_manifest}"
                "$<TARGET_BUNDLE_DIR:${format_target}>/Contents/Resources/pulp.plugin-runtime.json"
            COMMENT "Embedding pulp.plugin-runtime.json into ${format_target}"
            VERBATIM
        )
    else()
        add_custom_command(TARGET ${format_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_manifest}"
                "$<TARGET_FILE_DIR:${format_target}>/$<TARGET_FILE_BASE_NAME:${format_target}>.pulp.plugin-runtime.json"
            COMMENT "Writing pulp.plugin-runtime sidecar for ${format_target}"
            VERBATIM
        )
    endif()
endfunction()

function(_pulp_apply_macho_exports target file_stem)
    if(NOT APPLE)
        return()
    endif()
    if("${ARGN}" STREQUAL "")
        return()
    endif()

    set(_pulp_export_file "${CMAKE_CURRENT_BINARY_DIR}/${file_stem}.exports")
    file(WRITE "${_pulp_export_file}" "")
    foreach(_pulp_symbol IN LISTS ARGN)
        file(APPEND "${_pulp_export_file}" "${_pulp_symbol}\n")
    endforeach()

    target_link_options(${target} PRIVATE
        "LINKER:-exported_symbols_list,${_pulp_export_file}")
endfunction()

# Create the opt-in per-plugin install target. This remains private because the
# public install policy is driven by pulp_add_plugin(...), while the implementation
# is isolated here with the other install/export concerns.
function(_pulp_add_plugin_install_target target plugin_name)
# ── Install targets ────────────────────────────────────────────────
# Platform-appropriate install locations for each format
if(APPLE)
    set(_vst3_dir "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
    set(_clap_dir "$ENV{HOME}/Library/Audio/Plug-Ins/CLAP")
    set(_au_dir "$ENV{HOME}/Library/Audio/Plug-Ins/Components")
    set(_aax_dir "/Library/Application Support/Avid/Audio/Plug-Ins")
elseif(WIN32)
    set(_vst3_dir "$ENV{COMMONPROGRAMFILES}/VST3")
    set(_clap_dir "$ENV{COMMONPROGRAMFILES}/CLAP")
    set(_aax_dir "$ENV{COMMONPROGRAMFILES}/Avid/Audio/Plug-Ins")
elseif(UNIX)
    set(_vst3_dir "$ENV{HOME}/.vst3")
    set(_clap_dir "$ENV{HOME}/.clap")
endif()

# Custom install target: pulp-install-<target>
set(_install_commands "")
set(_install_dependencies "")
if(TARGET ${target}_VST3 AND DEFINED _vst3_dir)
    _pulp_control_shipping_scan_dependency(_vst3_scan ${target}_VST3)
    list(APPEND _install_dependencies ${_vst3_scan})
    list(APPEND _install_commands
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/VST3/${plugin_name}.vst3"
            "${_vst3_dir}/${plugin_name}.vst3")
endif()
if(TARGET ${target}_CLAP AND DEFINED _clap_dir)
    _pulp_control_shipping_scan_dependency(_clap_scan ${target}_CLAP)
    list(APPEND _install_dependencies ${_clap_scan})
    list(APPEND _install_commands
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/CLAP/${plugin_name}.clap"
            "${_clap_dir}/${plugin_name}.clap")
endif()
if(TARGET ${target}_AU AND DEFINED _au_dir)
    _pulp_control_shipping_scan_dependency(_au_scan ${target}_AU)
    list(APPEND _install_dependencies ${_au_scan})
    list(APPEND _install_commands
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/AU/${plugin_name}.component"
            "${_au_dir}/${plugin_name}.component")
endif()
if(TARGET ${target}_AAX AND DEFINED _aax_dir)
    _pulp_control_shipping_scan_dependency(_aax_scan ${target}_AAX)
    list(APPEND _install_dependencies ${_aax_scan})
    list(APPEND _install_commands
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/AAX/${plugin_name}.aaxplugin"
            "${_aax_dir}/${plugin_name}.aaxplugin")
endif()

# ── AUv3 install ─────────────────────────────────────────────────────
# The AU v3 packaging shape on macOS is a containing .app that holds
# the .appex + its framework. macOS discovers AU v3 extensions via
# Launch Services + PlugInKit, so the .app belongs in /Applications
# (or ~/Applications); the system folder under ~/Library/Audio/...
# is AU v2 only.
#
# After copying, we register the extension with `pluginkit -a` and
# flush the AudioComponent cache so DAWs see the new component on
# next relaunch without a full logout. `pulp doctor --au-cache`
# documents the same `killall -9 AudioComponentRegistrar` step.
if(TARGET ${target}_AUv3Host AND APPLE AND NOT PULP_IOS)
    # The embed target depends on all three binaries and assembles their
    # final host bundle. Depending on the binaries alone can install an
    # unassembled app when this specific target is built from clean state.
    _pulp_control_shipping_scan_dependency(_auv3_framework_scan
        ${target}_AUv3Framework)
    _pulp_control_shipping_scan_dependency(_auv3_extension_scan ${target}_AUv3)
    _pulp_control_shipping_scan_dependency(_auv3_host_scan ${target}_AUv3Host)
    list(APPEND _install_dependencies ${target}_AUv3Host_Embed
        ${_auv3_framework_scan} ${_auv3_extension_scan} ${_auv3_host_scan})
    set(_auv3_install_dir "$ENV{HOME}/Applications")
    list(APPEND _install_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_auv3_install_dir}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "${_auv3_install_dir}/${plugin_name}.app"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "$<TARGET_BUNDLE_DIR:${target}_AUv3Host>"
            "${_auv3_install_dir}/${plugin_name}.app"
        COMMAND /usr/bin/pluginkit -a
            "${_auv3_install_dir}/${plugin_name}.app/Contents/PlugIns/${plugin_name}.appex"
        # The one step that needs shell syntax gets its own fixed `sh -c`
        # string, so the install target can pass VERBATIM for the bundle
        # paths above, which carry the plug-in name unescaped.
        COMMAND /bin/sh -c
            "/usr/bin/killall -9 AudioComponentRegistrar || echo 'AudioComponentRegistrar not running; AU host launch will refresh the cache'"
    )
endif()

if(_install_commands)
    add_custom_target(pulp-install-${target}
        ${_install_commands}
        DEPENDS ${_install_dependencies}
        COMMENT "Installing ${plugin_name} to system plugin folders"
        VERBATIM
    )
    # Installed bundles must carry a current build-identity record.
    foreach(_pulp_build_info_format VST3 CLAP AU AAX)
        if(TARGET ${target}_${_pulp_build_info_format}_BuildInfo)
            add_dependencies(pulp-install-${target}
                ${target}_${_pulp_build_info_format}_BuildInfo)
        endif()
    endforeach()
endif()

endfunction()
