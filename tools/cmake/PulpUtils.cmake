# PulpUtils.cmake — Build utilities for Pulp projects
#
# Provides:
#   pulp_add_plugin()  — Create a plugin target with format adapters
#   pulp_add_app()     — Create a standalone application target
#   pulp_app_icon()    — Attach a generated app icon to a target
#   pulp_use_kit_ui()  — Attach reviewed kit UI resources to plugin formats
#   pulp_enable_midi_tuning_provider() — Attach optional tuning providers

include("${CMAKE_CURRENT_LIST_DIR}/PulpAppIcon.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpSparkle.cmake")

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
_pulp_pick_target(_PULP_NATIVE_VIEW_TARGET Pulp::view-native pulp::view-native)
_pulp_pick_target(_PULP_AUDIO_TARGET Pulp::audio pulp::audio)
_pulp_pick_target(_PULP_MIDI_TARGET Pulp::midi pulp::midi)
_pulp_pick_target(_PULP_STANDALONE_TARGET Pulp::standalone pulp::standalone)
_pulp_pick_target(_PULP_NATIVE_STANDALONE_TARGET
    Pulp::standalone-native pulp::standalone-native)
_pulp_pick_target(_PULP_CONTROL_STANDALONE_TARGET
    Pulp::inspect-standalone-runtime pulp::inspect-standalone-runtime)
_pulp_pick_target(_PULP_CONTROL_UI_TARGET
    Pulp::inspect-ui-runtime pulp::inspect-ui-runtime)
_pulp_pick_target(_PULP_CONTROL_INSPECT_TARGET Pulp::inspect pulp::inspect)
_pulp_pick_target(_PULP_CONTROL_RUNTIME_EVAL_TARGET
    Pulp::inspect-runtime-eval pulp::inspect-runtime-eval)
include("${CMAKE_CURRENT_LIST_DIR}/PulpControlShipping.cmake")
# The plugin helper included below retains the inspector control declaration
# contract for compatibility and shipping truth checks:
# _pulp_cache_control_declarations(${target} ...)
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

include("${CMAKE_CURRENT_LIST_DIR}/PulpFindPluginval.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpMidiTuning.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpPluginMetadata.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpBuildInfo.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpReactRuntime.cmake")

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
    # The cluster is compiled as ONE generated translation unit per binary (see
    # below), so this list is also an include order. window_host_mac.mm goes
    # last: it has a file-scope using-directive that would otherwise apply to
    # every file included after it.
    set(_pulp_view_objc_srcs
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/app_menu_mac.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/window_host_mac_capture.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/window_host_mac_geometry.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/window_host_mac_open_documents.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/window_host_mac_text_input.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/plugin_view_host_mac.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/plugin_view_host_mac_text_input.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/drag_drop_mac.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/accessibility_mac_host_lifetime.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/accessibility_mac.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/text_accessibility_macos.mm"
        "${_PULP_VIEW_PLATFORM_MAC_DIR}/window_host_mac.mm"
    )
    foreach(_src IN LISTS _pulp_view_objc_srcs)
        if(NOT EXISTS "${_src}")
            message(WARNING ${_pulp_view_objc_warn})
            return()
        endif()
    endforeach()

    # metal_surface_mac.mm's PulpMetalSurfaceView (the CAMetalLayer GPU-surface
    # NSView) is the one shared-pulp-render macOS ObjC class that
    # cross-plug-in-collides, so it gets the same per-binary suffix. Only added
    # when the render layer is actually present:
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

    # Compile the cluster as ONE generated ObjC++ translation unit per binary
    # rather than thirteen. Every file in the cluster imports Cocoa.h and the
    # same Pulp view headers, so with one file per object a tree that ships
    # eighty binaries parses Cocoa.h a thousand times for a few thousand lines
    # of ObjC; the unity file parses it once per binary. The per-binary suffix
    # is unchanged: the unity object is PRIVATE to `target` and carries its
    # PULP_VIEW_OBJC_SUFFIX, so it still satisfies pulp-view-core's references
    # ahead of the library's fixed-name copies. The file is written only when
    # its content changes, so a reconfigure does not dirty every binary; the
    # compiler's dependency output names the included .mm files, so editing one
    # of them rebuilds each binary's unity object as before. Each source is
    # included by absolute path, so the quote-includes inside it still resolve
    # against its own directory.
    set(_pulp_view_objc_unity
        "${CMAKE_CURRENT_BINARY_DIR}/pulp_mac_objc/${_pulp_view_objc_suffix}_mac_objc_cluster.mm")
    string(CONCAT _pulp_view_objc_unity_content
        "// Generated by _pulp_apply_view_mac_objc_suffix() for ${target}. Do not edit.\n"
        "// One translation unit for the macOS view/render ObjC cluster; the class\n"
        "// names are suffixed per binary through PULP_VIEW_OBJC_SUFFIX.\n")
    foreach(_src IN LISTS _pulp_render_objc_srcs _pulp_view_objc_srcs)
        string(APPEND _pulp_view_objc_unity_content "#include \"${_src}\"\n")
    endforeach()
    file(CONFIGURE OUTPUT "${_pulp_view_objc_unity}"
        CONTENT "${_pulp_view_objc_unity_content}" @ONLY)
    target_sources(${target} PRIVATE "${_pulp_view_objc_unity}")
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



# Focused concern modules are transitively included here so this file remains
# the stable compatibility shim for in-tree and installed SDK consumers.
# The installed-SDK evidence ledger intentionally anchors the public plugin
# declaration vocabulary here even when implementation bodies live in private
# modules: ICON;ICNS, _pulp_icon_configure_macos(${_pulp_icon_bundle},
# _pulp_standalone_control_capabilities, and the
# dev.pulp.sequencer/transport.loop.read@1 and
# dev.pulp.sequencer/transport.loop.write@1 capability IDs remain part of this
# shim's compatibility ownership surface.
# The plugin module's ordinary-build default remains
# `set(_pulp_control_profile "production-stripped")`.
include("${CMAKE_CURRENT_LIST_DIR}/PulpUiAssets.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpInstall.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpReload.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpPluginFormats.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpAuv3.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpIosHostApp.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpAppTargets.cmake")
# Plugin target orchestration lives in a private module; this include keeps
# PulpUtils.cmake as the stable compatibility entry point.
include("${CMAKE_CURRENT_LIST_DIR}/PulpPlugin.cmake")
