# An editor-less plug-in built with pulp_add_plugin(NO_EDITOR) must carry no
# view, canvas, render, Skia, Dawn or wgpu code. The same processor built
# without NO_EDITOR is the positive control: it does carry them (pulp_add_plugin
# links the view stack by default), so a scanner that sees nothing there is
# blind rather than the floor clean.
if(PULP_IOS OR ANDROID OR EMSCRIPTEN OR WIN32)
    return()
endif()

set(_no_editor_formats "")
if(PULP_HAS_VST3)
    list(APPEND _no_editor_formats VST3)
endif()
if(PULP_HAS_CLAP)
    list(APPEND _no_editor_formats CLAP)
endif()
if(APPLE AND PULP_HAS_AUSDK)
    list(APPEND _no_editor_formats AU)
endif()
if(NOT _no_editor_formats OR NOT PULP_HAS_CLAP)
    return()
endif()

pulp_add_plugin(PulpNoEditorLinkFloor
    NO_EDITOR
    FORMATS           ${_no_editor_formats}
    PLUGIN_NAME       "Pulp No Editor Link Floor"
    BUNDLE_ID         "dev.pulp.test.no-editor-link-floor"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Effect
    PLUGIN_CODE       "PNeL"
    MANUFACTURER_CODE "PlNe"
    SOURCES           no_editor_gain.hpp
    PROCESSOR_FACTORY pulp::test_fixtures::create_no_editor_gain
)

# Positive control: the same processor, default link path.
# FULL_EDITOR: the control must carry the full editor stack the NO_EDITOR
# plug-in leaves out; the macOS default (lite) would carry no Skia or Dawn.
pulp_add_plugin(PulpNoEditorLinkFloorControl
    FULL_EDITOR
    FORMATS           CLAP
    PLUGIN_NAME       "Pulp No Editor Link Floor Control"
    BUNDLE_ID         "dev.pulp.test.no-editor-link-floor-control"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Effect
    SOURCES           no_editor_gain.hpp
    PROCESSOR_FACTORY pulp::test_fixtures::create_no_editor_gain
)

# Target-graph contract, checked at configure time before dead stripping could
# hide a regression in the binary.
foreach(_fmt IN LISTS _no_editor_formats)
    pulp_link_closure(PulpNoEditorLinkFloor_${_fmt} TARGETS_OUT _no_editor_closure)
    # pulp-host and pulp-content are retained subsystems the authoring surface
    # does not need; a pulp-format-core edge to either would drag them in.
    foreach(_forbidden pulp-format pulp-format-view pulp-view pulp-view-core pulp-view-native pulp-view-tooling
                       pulp-view-script pulp-canvas pulp-render pulp-standalone pulp-host pulp-content)
        if(_forbidden IN_LIST _no_editor_closure)
            message(FATAL_ERROR
                "PulpNoEditorLinkFloor_${_fmt} reaches ${_forbidden}: ${_no_editor_closure}")
        endif()
    endforeach()
    # The UI stacks a project chooses with `pulp create --ui`: an audio-only
    # (--ui none) plug-in reaches neither the Vellum bridge nor RmlUi, under
    # any of their Pulp or installed-SDK target names.
    foreach(_forbidden pulp-vellum pulp-vellum-audio pulp-rmlui-native pulp-rmlui-native-metal
                       pulp-view-rmlui-importer-adapter)
        if(_forbidden IN_LIST _no_editor_closure)
            message(FATAL_ERROR
                "PulpNoEditorLinkFloor_${_fmt} reaches ${_forbidden}: ${_no_editor_closure}")
        endif()
    endforeach()
    foreach(_target IN LISTS _no_editor_closure)
        if(_target MATCHES "^(Vellum|VellumRmlUi|RmlUi)::")
            message(FATAL_ERROR
                "PulpNoEditorLinkFloor_${_fmt} reaches ${_target}: ${_no_editor_closure}")
        endif()
    endforeach()
    if(NOT pulp-format-core IN_LIST _no_editor_closure)
        message(FATAL_ERROR
            "PulpNoEditorLinkFloor_${_fmt} lost pulp-format-core: ${_no_editor_closure}")
    endif()
endforeach()

# Control for the Vellum check above: when the pulp-vellum bridge is
# configured, its own closure must match the same pattern, or the check
# could never fire.
if(TARGET pulp-vellum)
    pulp_link_closure(pulp-vellum TARGETS_OUT _vellum_bridge_closure)
    set(_vellum_bridge_seen OFF)
    foreach(_target IN LISTS _vellum_bridge_closure)
        if(_target MATCHES "^(Vellum|VellumRmlUi|RmlUi)::")
            set(_vellum_bridge_seen ON)
        endif()
    endforeach()
    if(NOT _vellum_bridge_seen)
        message(FATAL_ERROR
            "pulp-vellum's closure names no Vellum:: target, so the Vellum check is blind: "
            "${_vellum_bridge_closure}")
    endif()
endif()

# What the control's editor stack contains in this configuration. View and
# canvas always; the GPU layers only when the build has them.
set(_no_editor_control_families view canvas)
set(_no_editor_control_gpu OFF)
if(PULP_ENABLE_GPU AND TARGET pulp-render AND (PULP_HAS_SKIA OR PULP_HAS_WEBGPU))
    list(APPEND _no_editor_control_families render)
    set(_no_editor_control_gpu ON)
    # The control must describe the provider actually linked by pulp-render.
    # When Skia supplies Dawn, the optional wgpu-native target is deliberately
    # absent from the renderer link graph, so expecting `_wgpu*` symbols makes
    # the positive control report a false blind spot.
    set(_no_editor_webgpu_target FALSE)
    if(TARGET webgpu)
        set(_no_editor_webgpu_target TRUE)
    endif()
    pulp_gpu_links_wgpu_native(_no_editor_links_wgpu
        WEBGPU_TARGET ${_no_editor_webgpu_target})
    if(_no_editor_links_wgpu)
        list(APPEND _no_editor_control_families wgpu)
    endif()
    if(PULP_HAS_SKIA)
        list(APPEND _no_editor_control_families skia)
        # Skia's own Dawn archive (the decision core/render reads).
        if(PULP_SKIA_PROVIDES_DAWN)
            list(APPEND _no_editor_control_families dawn)
        endif()
    endif()
endif()
string(REPLACE ";" "|" _no_editor_control_families "${_no_editor_control_families}")
set(_no_editor_scan_args
    "-DCONTROL=$<TARGET_FILE:PulpNoEditorLinkFloorControl_CLAP>"
    "-DCONTROL_FAMILIES=${_no_editor_control_families}"
    "-DCONTROL_GPU_RUNTIME=${_no_editor_control_gpu}")
foreach(_fmt IN LISTS _no_editor_formats)
    list(APPEND _no_editor_scan_args "-D${_fmt}=$<TARGET_FILE:PulpNoEditorLinkFloor_${_fmt}>")
endforeach()
add_test(NAME no-editor-plugin-link-floor-artifacts
    COMMAND "${CMAKE_COMMAND}" ${_no_editor_scan_args}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/scan_no_editor_link_floor.cmake")
set_tests_properties(no-editor-plugin-link-floor-artifacts PROPERTIES
    LABELS "format;link-contract;artifact" TIMEOUT 120)

# The GPU-stack dependency pattern above is platform-specific, and a host only
# builds its own platform's artifacts. This self-test checks the pattern
# against recorded listings from every platform on any host.
add_test(NAME no-editor-link-floor-runtime-pattern
    COMMAND "${CMAKE_COMMAND}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/test_no_editor_runtime_deps.cmake")
set_tests_properties(no-editor-link-floor-runtime-pattern PROPERTIES
    LABELS "pr;link-contract" TIMEOUT 60)


# The ThirdPartyNotices file in each bundle comes from that bundle's own link
# closure: the no-editor bundles list no UI stack, the control lists its own.
#
# The control also links two stand-in SDKs after pulp_add_plugin() returns, so
# the closure is shown to be read at the end of the configure: one registers a
# manifest (with one entry behind a feature that is off), the other has none
# and must be reported under sdks_without_manifest.
add_library(NoticesProbe::probe INTERFACE IMPORTED)
add_library(NoticesUnclaimed::probe INTERFACE IMPORTED)
# A plain-named library no manifest claims: it must land in the build-directory
# report as an unattributed target and never in the bundle's file.
add_library(notices_unattributed_probe INTERFACE)
target_link_libraries(PulpNoEditorLinkFloorControl_CLAP PRIVATE
    NoticesProbe::probe NoticesUnclaimed::probe notices_unattributed_probe)
pulp_add_notice_manifest("${CMAKE_CURRENT_SOURCE_DIR}/notices_probe/notices.json")
set(PULP_NOTICES_PROBE_DISABLED_FEATURE OFF CACHE INTERNAL "")
set(_no_editor_notices "")
set(_no_editor_format_sdk_CLAP CLAP)
set(_no_editor_format_sdk_VST3 "VST3 SDK")
set(_no_editor_format_sdk_AU AudioUnitSDK)
foreach(_fmt IN LISTS _no_editor_formats)
    _pulp_notices_output(_notices PulpNoEditorLinkFloor_${_fmt} ${_fmt}
        "Pulp No Editor Link Floor" ThirdPartyNotices.json)
    list(APPEND _no_editor_notices "${_no_editor_format_sdk_${_fmt}}=${_notices}")
endforeach()
string(REPLACE ";" "|" _no_editor_notices "${_no_editor_notices}")
_pulp_notices_output(_control_notices PulpNoEditorLinkFloorControl_CLAP CLAP
    "Pulp No Editor Link Floor Control" ThirdPartyNotices.json)
set(_control_notice_requires CLAP Yoga "Notices Probe Component")
if(_no_editor_control_gpu AND PULP_HAS_SKIA)
    list(APPEND _control_notice_requires Skia)
    if(PULP_SKIA_PROVIDES_DAWN)
        list(APPEND _control_notice_requires Dawn)
    endif()
endif()
string(REPLACE ";" "|" _control_notice_requires "${_control_notice_requires}")
add_test(NAME plugin-third-party-notices
    COMMAND "${CMAKE_COMMAND}"
        "-DNO_EDITOR=${_no_editor_notices}"
        "-DCONTROL=${_control_notices}"
        "-DCONTROL_REQUIRES=${_control_notice_requires}"
        "-DCONTROL_FORBIDS=Notices Probe Disabled Feature"
        "-DCONTROL_MISSING_SDKS=NoticesUnclaimed"
        "-DCONTROL_REPORT=${CMAKE_BINARY_DIR}/pulp-notices/PulpNoEditorLinkFloorControl_CLAP/ThirdPartyNotices.report.json"
        "-DCONTROL_UNATTRIBUTED=notices_unattributed_probe"
        "-DNO_EDITOR_REPORT_DIR=${CMAKE_BINARY_DIR}/pulp-notices"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_third_party_notices.cmake")
set_tests_properties(plugin-third-party-notices PROPERTIES
    LABELS "format;link-contract;artifact" TIMEOUT 60)
