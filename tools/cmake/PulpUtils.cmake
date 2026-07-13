# PulpUtils.cmake — Build utilities for Pulp projects
#
# Provides:
#   pulp_add_plugin()  — Create a plugin target with format adapters
#   pulp_add_app()     — Create a standalone application target
#   pulp_app_icon()    — Attach a generated app icon to a target
#   pulp_use_kit_ui()  — Attach reviewed kit UI resources to plugin formats
#   pulp_enable_midi_tuning_provider() — Attach optional tuning providers

include("${CMAKE_CURRENT_LIST_DIR}/PulpAppIcon.cmake")

function(_pulp_pick_target out_var)
    foreach(_candidate IN LISTS ARGN)
        if(TARGET "${_candidate}")
            set(${out_var} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

_pulp_pick_target(_PULP_FORMAT_TARGET Pulp::format pulp::format)
_pulp_pick_target(_PULP_VIEW_TARGET Pulp::view pulp::view)
_pulp_pick_target(_PULP_AUDIO_TARGET Pulp::audio pulp::audio)
_pulp_pick_target(_PULP_MIDI_TARGET Pulp::midi pulp::midi)
_pulp_pick_target(_PULP_STANDALONE_TARGET Pulp::standalone pulp::standalone)
_pulp_pick_target(_PULP_VST3_SDK_TARGET Pulp::vst3-sdk vst3-sdk)
_pulp_pick_target(_PULP_CLAP_TARGET Pulp::clap clap)
_pulp_pick_target(_PULP_LV2_TARGET Pulp::lv2-headers lv2-headers)
_pulp_pick_target(_PULP_AUSDK_TARGET Pulp::ausdk ausdk)
_pulp_pick_target(_PULP_AAX_LIBRARY_TARGET Pulp::aax-library pulp-aax-library)

if(NOT _PULP_FORMAT_TARGET)
    message(FATAL_ERROR
        "PulpUtils.cmake requires Pulp targets to exist first. "
        "Use add_subdirectory(Pulp) or find_package(Pulp) before including it.")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/PulpMidiTuning.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpPluginMetadata.cmake")

# Resolve PulpUtils.cmake's sibling helper dirs without reaching into
# CMAKE_SOURCE_DIR (which is the *consumer's* source tree when this
# file is loaded via find_package, not Pulp's). CMAKE_CURRENT_LIST_DIR
# at top level is this file's own dir, so we
# probe both possible layouts (in-tree source build + installed SDK)
# before falling back. The fallback path is kept as a last-resort
# escape hatch for stale checkouts that haven't run `cmake --install`.
#
#   In-tree:    tools/cmake/PulpUtils.cmake
#               -> ../../core/format/src    (Pulp source tree)
#               -> ../../tools/templates/auv3
#               -> ../../templates/ios-auv3
#   Installed:  <prefix>/lib/cmake/Pulp/PulpUtils.cmake
#               -> ../../../src/pulp/format (matches root install(FILES) DESTINATION src/pulp/format)
#               -> ../../../templates/auv3
#               -> ../../../templates/ios-auv3
if(DEFINED PULP_FORMAT_SOURCE_DIR AND EXISTS "${PULP_FORMAT_SOURCE_DIR}")
    set(_PULP_FORMAT_SOURCE_DIR "${PULP_FORMAT_SOURCE_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../core/format/src")
    set(_PULP_FORMAT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../core/format/src")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../src/pulp/format")
    set(_PULP_FORMAT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src/pulp/format")
else()
    # Last resort: leave unset so plugin targets fail with a clear "missing
    # source" error at link-time rather than silently picking up the
    # consumer's source tree. Override with -DPULP_FORMAT_SOURCE_DIR=... if
    # the SDK install is incomplete.
    set(_PULP_FORMAT_SOURCE_DIR "")
endif()

# Directory holding the macOS view/window/accessibility Objective-C cluster
# (window_host_mac*.mm, plugin_view_host_mac.mm, drag_drop_mac.mm,
# accessibility_mac.mm, text_accessibility_macos.mm). Probed the same way as
# _PULP_FORMAT_SOURCE_DIR so it works in-tree and from an installed SDK.
# _pulp_apply_view_mac_objc_suffix() compiles a per-binary copy of these into
# each shipped plug-in / app so two Pulp binaries in one host don't register
# colliding ObjC class names. Empty when the sources aren't shipped — the helper
# then degrades gracefully to pulp-view-core's shared (fixed-name) copies.
if(DEFINED PULP_VIEW_PLATFORM_MAC_DIR AND EXISTS "${PULP_VIEW_PLATFORM_MAC_DIR}")
    set(_PULP_VIEW_PLATFORM_MAC_DIR "${PULP_VIEW_PLATFORM_MAC_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../core/view/platform/mac")
    set(_PULP_VIEW_PLATFORM_MAC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../core/view/platform/mac")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../src/pulp/view/platform/mac")
    set(_PULP_VIEW_PLATFORM_MAC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src/pulp/view/platform/mac")
else()
    set(_PULP_VIEW_PLATFORM_MAC_DIR "")
endif()

# Directory holding the shared macOS render Objective-C cluster
# (metal_surface_mac.mm + render_loop_apple.mm and their private headers). These
# define ObjC classes compiled once into pulp-render, so they get the same
# per-binary suffix treatment as the view cluster. Probed in-tree and from an
# installed SDK; empty when the sources aren't shipped (helper then skips the
# render part and warns).
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../core/render/src/metal_surface_mac.mm")
    set(_PULP_RENDER_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../core/render/src")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../src/pulp/render/metal_surface_mac.mm")
    set(_PULP_RENDER_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src/pulp/render")
else()
    set(_PULP_RENDER_SRC_DIR "")
endif()

# Compile a per-binary copy of the macOS view/window/accessibility Objective-C
# cluster into `target`, with every ObjC class name suffixed by the (sanitized)
# target name. ObjC class names are process-global; the shared pulp-view-core
# static library compiles this cluster under fixed names, so two Pulp binaries
# (two plug-ins, or a plug-in + an app) loaded into one host would register the
# same names and the runtime would let the first-loaded copy shadow the rest.
# Each shipped binary instead compiles its own suffixed copy here. Because these
# objects link directly into `target`, they satisfy pulp-view-core's references
# first, so the library's fixed-name copies are never pulled into `target`.
# No-op off macOS, on iOS, or when the cluster sources aren't available.
function(_pulp_apply_view_mac_objc_suffix target)
    if(NOT APPLE OR IOS)
        return()
    endif()
    # When the cluster sources aren't present (an installed SDK that didn't ship
    # them), the per-binary suffix can't be applied and `target` falls back to
    # pulp-view-core's shared fixed-name copies — which reintroduces the
    # cross-plug-in ObjC class collision. That's a real regression for SDK
    # consumers, so warn loudly rather than degrade silently. PulpInstallRules.cmake
    # ships the cluster to src/pulp/view/platform/mac specifically to avoid this.
    set(_pulp_view_objc_warn
        "pulp: ${target}: macOS view ObjC sources not found — its view/window/"
        "accessibility classes keep their shared fixed names, so loading this "
        "binary alongside another Pulp plug-in in one host may collide. The SDK "
        "should ship core/view/platform/mac under src/pulp/view/platform/mac.")
    if(NOT _PULP_VIEW_PLATFORM_MAC_DIR)
        message(WARNING ${_pulp_view_objc_warn})
        return()
    endif()
    set(_pulp_view_objc_srcs
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/window_host_mac.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/window_host_mac_capture.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/window_host_mac_geometry.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/window_host_mac_text_input.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/plugin_view_host_mac.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/plugin_view_host_mac_text_input.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/drag_drop_mac.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/accessibility_mac.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/text_accessibility_macos.mm"
    )
    foreach(_src IN LISTS _pulp_view_objc_srcs)
        if(NOT EXISTS "${_src}")
            message(WARNING ${_pulp_view_objc_warn})
            return()
        endif()
    endforeach()

    # metal_surface_mac.mm's PulpMetalView (the CAMetalLayer GPU-surface NSView) is
    # the one shared-pulp-render macOS ObjC class that cross-plug-in-collides, so
    # it gets the same per-binary suffix (renamed to a distinct base,
    # PulpMetalSurfaceView). Only added when the render layer is actually present:
    # the TU relies on pulp::render's include/link, wired below ONLY under
    # PULP_HAS_SKIA — adding it in a no-Skia macOS build (a supported fallback)
    # would fail to compile, so gate on the same condition. (render_loop_apple.mm's
    # display-link class is iOS-only — inside TARGET_OS_IPHONE — so there is no
    # macOS copy to namespace. The .mm's quote-include of the render names header
    # resolves from its own source dir, so no extra include dir is needed.)
    set(_pulp_render_objc_srcs "")
    if(PULP_HAS_SKIA AND (TARGET pulp::render OR TARGET Pulp::render))
        if(_PULP_RENDER_SRC_DIR AND EXISTS "${_PULP_RENDER_SRC_DIR}/metal_surface_mac.mm")
            set(_pulp_render_objc_srcs "${_PULP_RENDER_SRC_DIR}/metal_surface_mac.mm")
        else()
            message(WARNING
                "pulp: ${target}: core/render/metal_surface_mac.mm not found — its "
                "GPU-surface ObjC class keeps its shared fixed name and may collide "
                "when co-loaded with another Pulp plug-in. The SDK should ship it "
                "under src/pulp/render.")
        endif()
    endif()

    # Sanitize the target name into a valid C identifier fragment. The format
    # target name (e.g. SuperConvolver_AU) is unique per shipped binary.
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _pulp_view_objc_suffix "${target}")

    target_sources(${target} PRIVATE ${_pulp_view_objc_srcs} ${_pulp_render_objc_srcs})
    # PULP_VIEW_OBJC_SUFFIX is consumed ONLY by pulp_mac_objc_names.h and
    # pulp_render_objc_names.h, which only the view + render cluster .mm files
    # include — so a target-wide define affects just those TUs, never the rest of
    # `target`. (A per-source COMPILE_DEFINITIONS
    # would be wrong here: when every plug-in format target lives in one CMake
    # directory, set_source_files_properties shares the same shared-source scope
    # across them and the last-added target's value would win for all.)
    target_compile_definitions(${target}
        PRIVATE PULP_VIEW_OBJC_SUFFIX=_${_pulp_view_objc_suffix})
    if(PULP_HAS_SKIA AND TARGET pulp::render)
        target_link_libraries(${target} PRIVATE pulp::render)
    elseif(PULP_HAS_SKIA AND TARGET Pulp::render)
        target_link_libraries(${target} PRIVATE Pulp::render)
    endif()
endfunction()

if(DEFINED PULP_VST3_INCLUDE_DIR AND EXISTS "${PULP_VST3_INCLUDE_DIR}")
    set(_PULP_VST3_SDK_DIR "${PULP_VST3_INCLUDE_DIR}")
elseif(DEFINED VST3_SDK_DIR AND EXISTS "${VST3_SDK_DIR}")
    set(_PULP_VST3_SDK_DIR "${VST3_SDK_DIR}")
else()
    set(_PULP_VST3_SDK_DIR "")
endif()

if(DEFINED PULP_AUV3_TEMPLATE_DIR AND EXISTS "${PULP_AUV3_TEMPLATE_DIR}")
    set(_PULP_AUV3_TEMPLATE_DIR "${PULP_AUV3_TEMPLATE_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../tools/templates/auv3")
    set(_PULP_AUV3_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../tools/templates/auv3")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../templates/auv3")
    set(_PULP_AUV3_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../templates/auv3")
else()
    set(_PULP_AUV3_TEMPLATE_DIR "")
endif()

if(DEFINED PULP_IOS_AUV3_TEMPLATE_DIR AND EXISTS "${PULP_IOS_AUV3_TEMPLATE_DIR}")
    set(_PULP_IOS_AUV3_TEMPLATE_DIR "${PULP_IOS_AUV3_TEMPLATE_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../templates/ios-auv3")
    set(_PULP_IOS_AUV3_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../templates/ios-auv3")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../templates/ios-auv3")
    set(_PULP_IOS_AUV3_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../templates/ios-auv3")
else()
    set(_PULP_IOS_AUV3_TEMPLATE_DIR "")
endif()

function(_pulp_normalize_ui_script_path out_var source_dir ui_script)
    if(NOT ui_script)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    if(IS_ABSOLUTE "${ui_script}")
        set(_ui_script_path "${ui_script}")
    else()
        set(_ui_script_path "${source_dir}/${ui_script}")
    endif()

    get_filename_component(_ui_script_path "${_ui_script_path}" ABSOLUTE)
    file(TO_CMAKE_PATH "${_ui_script_path}" _ui_script_path)
    set(${out_var} "${_ui_script_path}" PARENT_SCOPE)
endfunction()

function(_pulp_apply_ui_script_definition target ui_script_path)
    if(NOT ui_script_path)
        return()
    endif()

    target_compile_definitions(${target} PRIVATE
        PULP_UI_SCRIPT_PATH="${ui_script_path}"
    )
endfunction()

function(_pulp_apply_ui_theme_definition target theme_path)
    if(NOT theme_path)
        return()
    endif()

    target_compile_definitions(${target} PRIVATE
        PULP_UI_THEME_PATH="${theme_path}"
    )
endfunction()

function(_pulp_apply_ui_asset_roots_definition target asset_roots)
    if(NOT asset_roots)
        return()
    endif()

    target_compile_definitions(${target} PRIVATE
        PULP_UI_ASSET_ROOTS="${asset_roots}"
    )
endfunction()

function(_pulp_select_kit_export out_var helper_name kit_target property explicit_value label)
    get_target_property(_values "${kit_target}" "${property}")
    if(NOT _values)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    set(_selected "")
    if(explicit_value)
        foreach(_value IN LISTS _values)
            if(_value STREQUAL explicit_value)
                set(_selected "${_value}")
            endif()
        endforeach()
        if(NOT _selected)
            message(FATAL_ERROR
                "${helper_name}: ${label} '${explicit_value}' is not exported by the kit")
        endif()
    else()
        list(LENGTH _values _value_count)
        if(_value_count EQUAL 1)
            list(GET _values 0 _selected)
        elseif(_value_count GREATER 1)
            message(FATAL_ERROR
                "${helper_name}: kit exports multiple ${label} entries; pass ${label} <path>")
        endif()
    endif()

    set(${out_var} "${_selected}" PARENT_SCOPE)
endfunction()

function(_pulp_absolute_project_path out_var path)
    if(NOT path)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    if(IS_ABSOLUTE "${path}")
        set(_abs "${path}")
    else()
        set(_abs "${CMAKE_SOURCE_DIR}/${path}")
    endif()
    get_filename_component(_abs "${_abs}" ABSOLUTE)
    file(TO_CMAKE_PATH "${_abs}" _abs)
    set(${out_var} "${_abs}" PARENT_SCOPE)
endfunction()

function(_pulp_absolute_project_paths out_var)
    set(_out)
    foreach(_path IN LISTS ARGN)
        _pulp_absolute_project_path(_abs "${_path}")
        if(_abs)
            list(APPEND _out "${_abs}")
        endif()
    endforeach()
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

function(_pulp_pipe_join out_var)
    set(_joined "")
    foreach(_value IN LISTS ARGN)
        if(_value MATCHES "\\|")
            message(FATAL_ERROR
                "pulp_use_kit_ui: asset root paths must not contain '|': ${_value}")
        endif()
        if(_joined)
            string(APPEND _joined "|")
        endif()
        string(APPEND _joined "${_value}")
    endforeach()
    set(${out_var} "${_joined}" PARENT_SCOPE)
endfunction()

function(pulp_use_kit_ui target kit_target)
    set(options)
    set(oneValueArgs SCRIPT TOKENS)
    set(multiValueArgs)
    cmake_parse_arguments(KIT_UI "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(KIT_UI_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "pulp_use_kit_ui(${target} ${kit_target}): unknown arguments: ${KIT_UI_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT TARGET "${target}_Core")
        message(FATAL_ERROR
            "pulp_use_kit_ui(${target} ...): call this after pulp_add_plugin(${target} ...)")
    endif()
    if(NOT TARGET "${kit_target}")
        message(FATAL_ERROR
            "pulp_use_kit_ui(${target} ${kit_target}): kit target does not exist. "
            "Apply the kit and include(cmake/pulp-kits.cmake OPTIONAL) first.")
    endif()

    _pulp_select_kit_export(_selected_script
        "pulp_use_kit_ui(${target} ${kit_target})"
        "${kit_target}" PULP_UI_SCRIPTS "${KIT_UI_SCRIPT}" "SCRIPT")
    if(NOT _selected_script)
        message(FATAL_ERROR
            "pulp_use_kit_ui(${target} ${kit_target}): kit exports no PULP_UI_SCRIPTS")
    endif()

    _pulp_select_kit_export(_selected_tokens
        "pulp_use_kit_ui(${target} ${kit_target})"
        "${kit_target}" PULP_DESIGN_TOKENS "${KIT_UI_TOKENS}" "TOKENS")
    get_target_property(_selected_assets "${kit_target}" PULP_ASSETS)
    if(NOT _selected_assets)
        set(_selected_assets)
    endif()

    _pulp_absolute_project_path(_selected_script_abs "${_selected_script}")
    _pulp_absolute_project_path(_selected_tokens_abs "${_selected_tokens}")
    _pulp_absolute_project_paths(_selected_assets_abs ${_selected_assets})
    _pulp_pipe_join(_selected_assets_joined ${_selected_assets_abs})

    if(NOT EXISTS "${_selected_script_abs}")
        message(WARNING
            "pulp_use_kit_ui(${target} ${kit_target}): selected UI script is missing: "
            "${_selected_script_abs}")
    endif()
    if(_selected_tokens_abs AND NOT EXISTS "${_selected_tokens_abs}")
        message(WARNING
            "pulp_use_kit_ui(${target} ${kit_target}): selected design token file is missing: "
            "${_selected_tokens_abs}")
    endif()

    set(PULP_${target}_UI_SCRIPT "${_selected_script_abs}" CACHE INTERNAL "")
    if(_selected_tokens_abs)
        set(PULP_${target}_UI_THEME "${_selected_tokens_abs}" CACHE INTERNAL "")
    endif()
    if(_selected_assets_joined)
        set(PULP_${target}_UI_ASSET_ROOTS "${_selected_assets_joined}" CACHE INTERNAL "")
    endif()
    set_property(TARGET "${target}_Core" PROPERTY PULP_KIT_UI_TARGET "${kit_target}")
    set_property(TARGET "${target}_Core" PROPERTY PULP_KIT_UI_SCRIPT "${_selected_script}")
    if(_selected_tokens)
        set_property(TARGET "${target}_Core" PROPERTY PULP_KIT_UI_TOKENS "${_selected_tokens}")
    endif()
    if(_selected_assets)
        set_property(TARGET "${target}_Core" PROPERTY PULP_KIT_UI_ASSETS "${_selected_assets}")
    endif()

    set(_applied_targets)
    foreach(_fmt VST3 CLAP AU LV2 AAX AUv3 Standalone)
        if(TARGET "${target}_${_fmt}")
            _pulp_apply_ui_script_definition("${target}_${_fmt}" "${_selected_script_abs}")
            _pulp_apply_ui_theme_definition("${target}_${_fmt}" "${_selected_tokens_abs}")
            _pulp_apply_ui_asset_roots_definition("${target}_${_fmt}" "${_selected_assets_joined}")
            list(APPEND _applied_targets "${target}_${_fmt}")
        endif()
    endforeach()

    if(NOT _applied_targets)
        message(WARNING
            "pulp_use_kit_ui(${target} ${kit_target}): no existing format targets were found; "
            "call this after pulp_add_plugin has created at least one format target")
    endif()
endfunction()

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
    set(_pulp_valid_content_kinds presets themes samples wavetables)

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
                "wavetables.")
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

# ── pulp_add_plugin ─────────────────────────────────────────────────────────
# Creates plugin targets for each requested format from a single declaration.
#
# Usage:
#   pulp_add_plugin(PulpGain
#       FORMATS         VST3 AU CLAP Standalone
#       PLUGIN_NAME     "PulpGain"
#       BUNDLE_ID       "com.pulp.gain"
#       MANUFACTURER    "Pulp"
#       VERSION         "1.0.0"
#       CATEGORY        Effect           # Effect | Instrument | MidiEffect
#       PLUGIN_CODE     "PGan"           # 4-char code for AU
#       MANUFACTURER_CODE "Pulp"         # 4-char code for AU
#       ACCEPTS_MIDI                     # set if descriptor.accepts_midi is true;
#                                        # flips AU component type from aufx to aumf
#                                        # so hosts route inbound MIDI to the plug-in
#       SOURCES         pulp_gain.hpp main.cpp
#       PROCESSOR_FACTORY create_pulp_gain  # Function that returns unique_ptr<Processor>
#   )
#
# This creates:
#   ${target}_Core       — Object library with shared processor code
#   ${target}_VST3       — VST3 bundle (.vst3)
#   ${target}_AU         — AU v2 component (.component)
#   ${target}_CLAP       — CLAP bundle (.clap)
#   ${target}_Standalone — Standalone executable
#
function(pulp_add_plugin target)
    cmake_parse_arguments(PLUGIN
        "ACCEPTS_MIDI"
        "PLUGIN_NAME;BUNDLE_ID;VERSION;MANUFACTURER;CATEGORY;PLUGIN_CODE;MANUFACTURER_CODE;AAX_PRODUCT_CODE;AAX_NATIVE_CODE;PROCESSOR_FACTORY;UI_SCRIPT;DESIGN_WIDTH;DESIGN_HEIGHT;DESIGN_MIN_WIDTH;DESIGN_MIN_HEIGHT;DESIGN_MAX_WIDTH;DESIGN_MAX_HEIGHT"
        "FORMATS;SOURCES;CONTENT_CAPABILITIES;CONTENT_KINDS;CONTENT_HOT_RELOAD_KINDS;CONTENT_MANUAL_RESCAN_KINDS"
        ${ARGN}
    )
    if("PRODUCES_MIDI" IN_LIST PLUGIN_UNPARSED_ARGUMENTS)
        message(WARNING
            "pulp_add_plugin(${target}): PRODUCES_MIDI is ignored. "
            "Set PluginDescriptor::produces_midi = true in the processor; "
            "MIDI output is format/runtime metadata, not a CMake packaging flag.")
    endif()

    # Defaults
    if(NOT PLUGIN_PLUGIN_NAME)
        set(PLUGIN_PLUGIN_NAME "${target}")
    endif()
    if(NOT PLUGIN_VERSION)
        set(PLUGIN_VERSION "1.0.0")
    endif()
    if(NOT PLUGIN_MANUFACTURER)
        set(PLUGIN_MANUFACTURER "Unknown")
    endif()
    if(NOT PLUGIN_CATEGORY)
        set(PLUGIN_CATEGORY "Effect")
    endif()

    # ACCEPTS_MIDI mirrors ``PluginDescriptor::accepts_midi`` to CMake so
    # we can pick the correct AU component type (``aumf`` vs ``aufx``).
    # Hosts only route MIDI to AU v2 effects packaged as
    # ``kAudioUnitType_MusicEffect`` (``aumf``); ``aufx``-typed plug-ins
    # never see ``HandleMIDIEvent`` regardless of what the adapter wires.
    # Plug-ins that set ``accepts_midi = true`` in their descriptor MUST
    # also pass ACCEPTS_MIDI to ``pulp_add_plugin`` so the emitted
    # ``.component`` bundle's ``type`` matches.
    if(PLUGIN_ACCEPTS_MIDI)
        set(_plugin_accepts_midi "1")
    else()
        set(_plugin_accepts_midi "0")
    endif()

    _pulp_normalize_ui_script_path(_PULP_UI_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}" "${PLUGIN_UI_SCRIPT}")
    set(PULP_${target}_UI_SCRIPT "${_PULP_UI_SCRIPT}" CACHE INTERNAL "")
    set(PULP_${target}_CONTENT_CAPABILITIES "${PLUGIN_CONTENT_CAPABILITIES}" CACHE INTERNAL "")
    set(PULP_${target}_CONTENT_KINDS "${PLUGIN_CONTENT_KINDS}" CACHE INTERNAL "")
    set(PULP_${target}_CONTENT_HOT_RELOAD_KINDS "${PLUGIN_CONTENT_HOT_RELOAD_KINDS}" CACHE INTERNAL "")
    set(PULP_${target}_CONTENT_MANUAL_RESCAN_KINDS "${PLUGIN_CONTENT_MANUAL_RESCAN_KINDS}" CACHE INTERNAL "")

    if(_PULP_UI_SCRIPT AND NOT EXISTS "${_PULP_UI_SCRIPT}")
        message(WARNING
            "pulp_add_plugin(${target}): UI_SCRIPT points to a missing file: ${_PULP_UI_SCRIPT}. "
            "The editor will fall back to AutoUi until the script exists.")
    endif()
    _pulp_configure_plugin_runtime_manifest(${target} "${PLUGIN_BUNDLE_ID}")

    # ── Core library ────────────────────────────────────────────────────
    # For header-only processors (no SOURCES), create INTERFACE library.
    # For compiled processors, create OBJECT library.
    if(PLUGIN_SOURCES)
        add_library(${target}_Core OBJECT ${PLUGIN_SOURCES})
        target_link_libraries(${target}_Core PUBLIC ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        set(_PULP_CORE_OBJECTS "${PULP_${target}_CORE_OBJECTS}")
    else()
        add_library(${target}_Core INTERFACE)
        target_link_libraries(${target}_Core INTERFACE ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
        set(_PULP_CORE_OBJECTS "")
    endif()
    # Store for format target functions (CMake doesn't propagate local vars to functions)
    set(_PULP_CORE_OBJECTS "${_PULP_CORE_OBJECTS}" PARENT_SCOPE)
    set(PULP_${target}_CORE_OBJECTS "${_PULP_CORE_OBJECTS}" CACHE INTERNAL "")
    if(PLUGIN_SOURCES)
        target_compile_definitions(${target}_Core PRIVATE
            PULP_PLUGIN_NAME="${PLUGIN_PLUGIN_NAME}"
            PULP_BUNDLE_ID="${PLUGIN_BUNDLE_ID}"
            PULP_PLUGIN_VERSION="${PLUGIN_VERSION}"
        )
    endif()

    # ── Design dimensions (auto-sizing for imported-design plugins) ────
    # When DESIGN_WIDTH/HEIGHT are supplied, inject them as compile-defs so
    # `format::Processor::view_size()`'s default returns sensible bounds
    # (derived min = 2/3 preferred, max = 2x preferred, aspect = W/H).
    # Plugins still pull this in via target_compile_definitions on Core,
    # so all linked format adapters see the same defs. Explicit MIN/MAX
    # args override the derived values. See processor.hpp:view_size() and
    # the import-design skill.
    if(PLUGIN_DESIGN_WIDTH AND PLUGIN_DESIGN_HEIGHT)
        if(NOT PLUGIN_DESIGN_MIN_WIDTH)
            set(PLUGIN_DESIGN_MIN_WIDTH 0)
        endif()
        if(NOT PLUGIN_DESIGN_MIN_HEIGHT)
            set(PLUGIN_DESIGN_MIN_HEIGHT 0)
        endif()
        if(NOT PLUGIN_DESIGN_MAX_WIDTH)
            set(PLUGIN_DESIGN_MAX_WIDTH 0)
        endif()
        if(NOT PLUGIN_DESIGN_MAX_HEIGHT)
            set(PLUGIN_DESIGN_MAX_HEIGHT 0)
        endif()
        set(_design_defs
            PULP_PLUGIN_DESIGN_W=${PLUGIN_DESIGN_WIDTH}
            PULP_PLUGIN_DESIGN_H=${PLUGIN_DESIGN_HEIGHT}
            PULP_PLUGIN_DESIGN_MIN_W=${PLUGIN_DESIGN_MIN_WIDTH}
            PULP_PLUGIN_DESIGN_MIN_H=${PLUGIN_DESIGN_MIN_HEIGHT}
            PULP_PLUGIN_DESIGN_MAX_W=${PLUGIN_DESIGN_MAX_WIDTH}
            PULP_PLUGIN_DESIGN_MAX_H=${PLUGIN_DESIGN_MAX_HEIGHT}
        )
        if(PLUGIN_SOURCES)
            target_compile_definitions(${target}_Core PUBLIC ${_design_defs})
        else()
            target_compile_definitions(${target}_Core INTERFACE ${_design_defs})
        endif()
    endif()

    # ── VST3 ─────────────────────────────────────────────────────────────
    if("VST3" IN_LIST PLUGIN_FORMATS AND PULP_HAS_VST3)
        _pulp_add_vst3(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                        "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}" "${PLUGIN_CATEGORY}")
    endif()

    # ── CLAP ─────────────────────────────────────────────────────────────
    if("CLAP" IN_LIST PLUGIN_FORMATS AND PULP_HAS_CLAP)
        _pulp_add_clap(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                        "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}" "${PLUGIN_CATEGORY}")
    endif()

    # ── AU v2 ────────────────────────────────────────────────────────────
    if("AU" IN_LIST PLUGIN_FORMATS AND APPLE AND PULP_HAS_AUSDK)
        if(NOT PLUGIN_PLUGIN_CODE OR NOT PLUGIN_MANUFACTURER_CODE)
            message(WARNING "pulp_add_plugin(${target}): AU format requires PLUGIN_CODE and MANUFACTURER_CODE")
        else()
            _pulp_add_au(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                          "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}"
                          "${PLUGIN_CATEGORY}" "${PLUGIN_PLUGIN_CODE}" "${PLUGIN_MANUFACTURER_CODE}"
                          "${_plugin_accepts_midi}")
        endif()
    endif()

    # ── LV2 ───────────────────────────────────────────────────────────────
    if("LV2" IN_LIST PLUGIN_FORMATS AND PULP_HAS_LV2)
        _pulp_add_lv2(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                       "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}" "${PLUGIN_CATEGORY}")
    endif()

    # ── AAX ───────────────────────────────────────────────────────────────
    if("AAX" IN_LIST PLUGIN_FORMATS)
        if(NOT APPLE AND NOT WIN32)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): AAX is only supported on macOS and Windows. "
                "Remove AAX from FORMATS when configuring on Linux or Ubuntu.")
        elseif(PULP_HAS_AAX)
            if(NOT PLUGIN_MANUFACTURER_CODE OR NOT PLUGIN_AAX_PRODUCT_CODE OR NOT PLUGIN_AAX_NATIVE_CODE)
                message(FATAL_ERROR
                    "pulp_add_plugin(${target}): AAX format requires "
                    "MANUFACTURER_CODE, AAX_PRODUCT_CODE, and AAX_NATIVE_CODE")
            endif()

            _pulp_add_aax(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                          "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}"
                          "${PLUGIN_CATEGORY}" "${PLUGIN_MANUFACTURER_CODE}"
                          "${PLUGIN_AAX_PRODUCT_CODE}" "${PLUGIN_AAX_NATIVE_CODE}")
        endif()
    endif()

    # ── AUv3 ──────────────────────────────────────────────────────────────
    if("AUv3" IN_LIST PLUGIN_FORMATS AND APPLE)
        if(NOT PLUGIN_PLUGIN_CODE OR NOT PLUGIN_MANUFACTURER_CODE)
            message(WARNING "pulp_add_plugin(${target}): AUv3 format requires PLUGIN_CODE and MANUFACTURER_CODE")
        else()
            _pulp_add_auv3(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                           "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}"
                           "${PLUGIN_CATEGORY}" "${PLUGIN_PLUGIN_CODE}" "${PLUGIN_MANUFACTURER_CODE}"
                           "${_plugin_accepts_midi}")
        endif()
    endif()

    # ── Standalone ───────────────────────────────────────────────────────
    if("Standalone" IN_LIST PLUGIN_FORMATS)
        _pulp_add_standalone(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}" "${PLUGIN_VERSION}")
    endif()

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
    if(TARGET ${target}_VST3 AND DEFINED _vst3_dir)
        list(APPEND _install_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/VST3/${PLUGIN_PLUGIN_NAME}.vst3"
                "${_vst3_dir}/${PLUGIN_PLUGIN_NAME}.vst3")
    endif()
    if(TARGET ${target}_CLAP AND DEFINED _clap_dir)
        list(APPEND _install_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/CLAP/${PLUGIN_PLUGIN_NAME}.clap"
                "${_clap_dir}/${PLUGIN_PLUGIN_NAME}.clap")
    endif()
    if(TARGET ${target}_AU AND DEFINED _au_dir)
        list(APPEND _install_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/AU/${PLUGIN_PLUGIN_NAME}.component"
                "${_au_dir}/${PLUGIN_PLUGIN_NAME}.component")
    endif()
    if(TARGET ${target}_AAX AND DEFINED _aax_dir)
        list(APPEND _install_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/AAX/${PLUGIN_PLUGIN_NAME}.aaxplugin"
                "${_aax_dir}/${PLUGIN_PLUGIN_NAME}.aaxplugin")
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
        set(_auv3_install_dir "$ENV{HOME}/Applications")
        list(APPEND _install_commands
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_auv3_install_dir}"
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                "${_auv3_install_dir}/${PLUGIN_PLUGIN_NAME}.app"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:${target}_AUv3Host>"
                "${_auv3_install_dir}/${PLUGIN_PLUGIN_NAME}.app"
            COMMAND /usr/bin/pluginkit -a
                "${_auv3_install_dir}/${PLUGIN_PLUGIN_NAME}.app/Contents/PlugIns/${PLUGIN_PLUGIN_NAME}.appex"
            COMMAND /usr/bin/killall -9 AudioComponentRegistrar
                || ${CMAKE_COMMAND} -E echo "AudioComponentRegistrar not running; AU host launch will refresh the cache"
        )
    endif()

    if(_install_commands)
        add_custom_target(pulp-install-${target}
            ${_install_commands}
            COMMENT "Installing ${PLUGIN_PLUGIN_NAME} to system plugin folders"
        )
    endif()

    set(_built_formats)
    foreach(_fmt VST3 CLAP AU AUv3 LV2 AAX Standalone)
        if(TARGET ${target}_${_fmt})
            list(APPEND _built_formats ${_fmt})
        endif()
    endforeach()
    if(_built_formats)
        list(JOIN _built_formats ";" _built_formats_display)
    else()
        set(_built_formats_display "none")
    endif()
    message(STATUS "Pulp plugin: ${target} (formats: ${_built_formats_display})")
endfunction()

# ── pulp_add_plugin_bundle — ONE binary per format hosting MANY plugins ────
#
# The cross-format counterpart to the AU/VST3/CLAP bundle macros: a single
# .vst3 / .clap that exposes N distinct plugins (Expert Sleepers Silent Way
# style). Each format's entry TU (`<fmt>_bundle_entry.cpp` in the source dir)
# uses the bundle macros to register N plugins into one binary; per-plugin
# metadata (category, UI script, and — for AU — component codes) lives in the
# macro calls, not in CMake args. The single-plugin path (`pulp_add_plugin`)
# is unchanged and remains the default (see PULP_PLUGIN_PACKAGING).
#
# v1 supports the internally-enumerating formats CLAP and VST3 (each emits one
# entry symbol whose factory lists N plugins). AU/AUv3/AAX bundles additionally
# need a multi-component Info.plist and multi-symbol export, so they land as a
# separate slice; requesting one here errors with that note rather than emitting
# a silently-single-plugin binary.
#
#   pulp_add_plugin_bundle(MyBundle
#       BUNDLE_NAME  "MyBundle"
#       BUNDLE_ID    "com.example.mybundle"
#       VERSION      "1.0.0"
#       MANUFACTURER "Example"
#       FORMATS      CLAP VST3
#       SOURCES      plugin_a.cpp plugin_b.cpp   # union of the bundled plugins
#   )
function(pulp_add_plugin_bundle target)
    cmake_parse_arguments(BUNDLE
        ""
        "BUNDLE_NAME;BUNDLE_ID;VERSION;MANUFACTURER"
        "FORMATS;SOURCES"
        ${ARGN}
    )
    if(NOT BUNDLE_BUNDLE_NAME)
        set(BUNDLE_BUNDLE_NAME "${target}")
    endif()
    if(NOT BUNDLE_VERSION)
        set(BUNDLE_VERSION "1.0.0")
    endif()
    if(NOT BUNDLE_MANUFACTURER)
        set(BUNDLE_MANUFACTURER "Unknown")
    endif()

    foreach(_fmt ${BUNDLE_FORMATS})
        if(_fmt STREQUAL "AU" OR _fmt STREQUAL "AUv3" OR _fmt STREQUAL "AAX")
            message(FATAL_ERROR
                "pulp_add_plugin_bundle(${target}): ${_fmt} bundles are not yet "
                "supported. An ${_fmt} bundle needs a multi-component Info.plist and "
                "multi-symbol export (a separate slice). Use CLAP and/or VST3 here.")
        endif()
    endforeach()

    _pulp_configure_plugin_runtime_manifest(${target} "${BUNDLE_BUNDLE_ID}")

    # All bundled plugins' code flows into each format binary. With SOURCES, an
    # OBJECT library carries them; header-only plugins (inline factories pulled in
    # by the per-format entry TU) use an INTERFACE library. Either way the objects
    # reach the binary through the link (target_link_libraries(<fmt> PRIVATE
    # ${target}_Core) in the format functions), so CORE_OBJECTS stays empty —
    # mirroring pulp_add_plugin.
    if(BUNDLE_SOURCES)
        add_library(${target}_Core OBJECT ${BUNDLE_SOURCES})
        target_link_libraries(${target}_Core PUBLIC ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(${target}_Core PRIVATE
            PULP_PLUGIN_NAME="${BUNDLE_BUNDLE_NAME}"
            PULP_BUNDLE_ID="${BUNDLE_BUNDLE_ID}"
            PULP_PLUGIN_VERSION="${BUNDLE_VERSION}")
    else()
        add_library(${target}_Core INTERFACE)
        target_link_libraries(${target}_Core INTERFACE ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    set(PULP_${target}_CORE_OBJECTS "" CACHE INTERNAL "")
    # No bundle-wide compile-time UI script: a bundle's plugins each resolve
    # their own editor assets from the keyed registry at runtime, rather than
    # from a single compile-time PULP_UI_SCRIPT_PATH shared by the whole binary.
    set(PULP_${target}_UI_SCRIPT "" CACHE INTERNAL "")

    if("VST3" IN_LIST BUNDLE_FORMATS AND PULP_HAS_VST3)
        _pulp_add_vst3(${target} "${BUNDLE_BUNDLE_NAME}" "${BUNDLE_BUNDLE_ID}"
                        "${BUNDLE_VERSION}" "${BUNDLE_MANUFACTURER}" "Effect")
    endif()
    if("CLAP" IN_LIST BUNDLE_FORMATS AND PULP_HAS_CLAP)
        _pulp_add_clap(${target} "${BUNDLE_BUNDLE_NAME}" "${BUNDLE_BUNDLE_ID}"
                        "${BUNDLE_VERSION}" "${BUNDLE_MANUFACTURER}" "Effect")
    endif()

    set(_built_formats)
    foreach(_fmt VST3 CLAP)
        if(TARGET ${target}_${_fmt})
            list(APPEND _built_formats ${_fmt})
        endif()
    endforeach()
    if(_built_formats)
        list(JOIN _built_formats ";" _built_formats_display)
    else()
        set(_built_formats_display "none")
    endif()
    message(STATUS "Pulp plugin BUNDLE: ${target} (formats: ${_built_formats_display})")
endfunction()

# ── pulp_add_reload_logic — a hot-reloadable DSP "logic" library ──────────
#
# Builds the DSP half of a hot-reloadable plugin: a MODULE shared library that
# exports the reload ABI (PULP_RELOAD_LOGIC) and is dlopen'd + hot-swapped by a
# pulp::format::reload::ReloadableShell while the host keeps the plugin alive.
# Keep this target's sources separate from the shell plugin so the DSP can be
# recompiled without rebuilding what the host has loaded.
#
#   pulp_add_reload_logic(<target>
#       SOURCES <src>...                # the DSP translation unit(s)
#       [OUTPUT_NAME <name>]            # built file stem (default: target name)
#       [PUBLISH_DIR <dir>]             # if set, POST_BUILD copies the built
#                                       # module here (dir created if needed) —
#                                       # the path a ReloadableShell watches.
#       [RESOLVE_FROM_HOST])            # thin mode: don't static-link the SDK;
#                                       # resolve pulp::* symbols from the HOST at
#                                       # dlopen (see below).
#
# The module gets no "lib" prefix and a platform-stable suffix (.dylib/.so/.dll)
# so the watched path is predictable.
#
# Linking model — two choices:
#  • DEFAULT (static): links pulp::format into the module. Simple; fine for
#    DSP-only logic. The SDK ends up duplicated in the process (host copy + logic
#    copy); harmless for format (POD/RT code, identical builds gated by the
#    fingerprint), but a logic that pulls ObjC-heavy libs (e.g. pulp::view for a
#    custom create_view()) would duplicate those classes — noisy and unsafe.
#  • RESOLVE_FROM_HOST (thin): links NO SDK archives; adds the SDK headers and,
#    on macOS, `-undefined dynamic_lookup` so pulp::* references resolve from the
#    HOST at dlopen — ONE copy of the SDK in the process. Use this whenever the
#    logic builds UI (links pulp::view). The host that loads it MUST export its
#    symbols: call pulp_reload_host(<host_target>). (Linux resolves undefined
#    shared-object symbols at load by default; Windows needs the static model.)
function(pulp_add_reload_logic target)
    cmake_parse_arguments(RL "RESOLVE_FROM_HOST" "OUTPUT_NAME;PUBLISH_DIR" "SOURCES" ${ARGN})
    if(NOT RL_SOURCES)
        message(FATAL_ERROR "pulp_add_reload_logic(${target}): SOURCES is required")
    endif()
    if(NOT RL_OUTPUT_NAME)
        set(RL_OUTPUT_NAME "${target}")
    endif()
    if(APPLE)
        set(_rl_suffix ".dylib")
    elseif(WIN32)
        set(_rl_suffix ".dll")
    else()
        set(_rl_suffix ".so")
    endif()

    add_library(${target} MODULE ${RL_SOURCES})
    _pulp_pick_target(_RL_FORMAT_TARGET Pulp::format pulp::format)
    if(RL_RESOLVE_FROM_HOST AND NOT WIN32)
        # Thin: SDK headers only; resolve pulp::* from the host at dlopen so there
        # is a single copy of the SDK in the process (no duplicate ObjC classes /
        # statics). Enumerate the SDK targets' interface include dirs without
        # linking their archives.
        #
        # Intentionally an explicit dual-prefix list, NOT the in-tree
        # PULP_SDK_TARGETS (PulpInstallRules.cmake): this helper must also work for
        # downstream find_package(Pulp) consumers, who get imported `Pulp::*`
        # targets and no PULP_SDK_TARGETS. Each entry is if(TARGET)-guarded, so a
        # missing module is skipped silently — when a NEW SDK module's headers are
        # needed by thin logic, add it here.
        foreach(_rl_dep Pulp::format pulp::format Pulp::view pulp::view Pulp::canvas pulp::canvas
                        Pulp::state pulp::state Pulp::audio pulp::audio Pulp::midi pulp::midi
                        Pulp::runtime pulp::runtime Pulp::platform pulp::platform
                        Pulp::events pulp::events Pulp::signal pulp::signal)
            if(TARGET ${_rl_dep})
                target_include_directories(${target} PRIVATE
                    $<TARGET_PROPERTY:${_rl_dep},INTERFACE_INCLUDE_DIRECTORIES>)
            endif()
        endforeach()
        if(APPLE)
            target_link_options(${target} PRIVATE -undefined dynamic_lookup)
        endif()
        # Linux: a MODULE may keep undefined symbols, resolved from the host at
        # dlopen — no extra flag needed.
        #
        # The thin logic MUST compile at the SAME C++ standard as the SDK/host or
        # the build-fingerprint's cpp_standard field mismatches and the reload is
        # rejected. A static logic links pulp::format (PUBLIC cxx_std_23) and so
        # inherits C++23; a THIN logic links no archives, so it would fall back to
        # the root CMAKE_CXX_STANDARD (20) → host=202302 vs logic=202002 mismatch.
        # Match pulp-format, which sets CXX_STANDARD 23 authoritatively
        # (core/format/CMakeLists.txt). Keep in lockstep if the SDK bumps standard.
        set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
    else()
        if(RL_RESOLVE_FROM_HOST AND WIN32)
            message(WARNING "pulp_add_reload_logic(${target}): RESOLVE_FROM_HOST is "
                            "unsupported on Windows; using the static link model.")
        endif()
        target_link_libraries(${target} PRIVATE ${_RL_FORMAT_TARGET})
    endif()
    set_target_properties(${target} PROPERTIES
        PREFIX "" OUTPUT_NAME "${RL_OUTPUT_NAME}" SUFFIX "${_rl_suffix}"
        POSITION_INDEPENDENT_CODE ON)

    if(RL_PUBLISH_DIR)
        # Publish the freshly-built module to the watched path so the shell finds
        # its DSP the moment the host loads it. NB: a literal $ENV{HOME} in
        # PUBLISH_DIR is the CONFIGURING user's home (captured now); a plugin
        # loaded by a different user should set PULP_RELOAD_LOGIC_PATH instead.
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${RL_PUBLISH_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${target}>"
                    "${RL_PUBLISH_DIR}/${RL_OUTPUT_NAME}${_rl_suffix}"
            COMMENT "Publishing reload logic to ${RL_PUBLISH_DIR}/${RL_OUTPUT_NAME}${_rl_suffix}")
    endif()
endfunction()

# ── pulp_reload_host — export a host's SDK symbols for thin reload logic ───
#
# A logic library built with pulp_add_reload_logic(... RESOLVE_FROM_HOST) leaves
# its pulp::* references undefined, to be resolved from whatever HOST dlopen's it
# (the standalone app, the plugin bundle, a capture tool). For that to work the
# host must publish its (statically-linked) SDK symbols in its dynamic symbol
# table. Call this on each host target that loads thin logic. No-op on Windows.
function(pulp_reload_host target)
    if(WIN32)
        return()
    elseif(APPLE)
        target_link_options(${target} PRIVATE -Wl,-export_dynamic)
    else()  # Linux / other ELF
        target_link_options(${target} PRIVATE -rdynamic)
    endif()
endfunction()

# Like pulp_reload_host, but ALSO force-retains + exports the editor ABI surface
# (View/Label/widgets) so a UI-carrying hot-reload logic (its create_view()
# resolves pulp::view from the host at dlopen) can render its editor inside this
# host. Use this instead of pulp_reload_host for a host that loads a logic which
# builds its own UI. (Live-swap M2b — see planning/2026-07-03-m2b-...) The plain
# host would dead-strip the editor symbols (nothing in the host references them);
# force-loading pulp-view-reload-exports keeps them, and export_dynamic below
# makes them resolvable by the dlopened logic. Force-loads ONLY the tiny
# curated-surface lib, NOT all of view-core (which drags in the mac-incompatible
# SDL-host TU).
function(pulp_reload_host_ui target)
    pulp_reload_host(${target})
    if(TARGET pulp-view-reload-exports)
        target_link_libraries(${target} PRIVATE pulp-view-reload-exports)
        if(APPLE)
            target_link_options(${target} PRIVATE
                "-Wl,-force_load,$<TARGET_FILE:pulp-view-reload-exports>"
                # The plugin's -exported_symbols_list exports only the entry point
                # and localizes everything else, defeating -export_dynamic for the
                # SDK symbols a THIN (RESOLVE_FROM_HOST) logic resolves at dlopen.
                # -exported_symbol ADDS patterns to the export set: the whole pulp::
                # surface (mangled Itanium prefix _ZN4pulp..., leading '_' on Mach-O)
                # so the dlopened logic resolves BOTH its editor (pulp::view
                # View/Label/widgets) AND its Processor base (pulp::format virtuals
                # like create_ara_document_controller) + any state/canvas/midi it uses.
                "-Wl,-exported_symbol,__ZN4pulp*"
                "-Wl,-exported_symbol,__ZNK4pulp*"
                "-Wl,-exported_symbol,__ZTVN4pulp*"    # vtables
                "-Wl,-exported_symbol,__ZTIN4pulp*"    # typeinfo
                "-Wl,-exported_symbol,__ZTSN4pulp*")   # typeinfo name
        elseif(NOT WIN32)
            target_link_options(${target} PRIVATE
                "-Wl,--whole-archive" "$<TARGET_FILE:pulp-view-reload-exports>"
                "-Wl,--no-whole-archive")
        endif()
    endif()
endfunction()

# ── Internal: VST3 target ────────────────────────────────────────────────

# Per-format / AUv3 / app target helpers
# live in focused modules now. PulpUtils.cmake stays the
# public include shim so external `find_package(Pulp)` users
# get the full surface via a single include.
include("${CMAKE_CURRENT_LIST_DIR}/PulpPluginFormats.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpAuv3.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpIosHostApp.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpAppTargets.cmake")
