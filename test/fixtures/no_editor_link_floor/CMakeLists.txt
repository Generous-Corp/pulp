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
pulp_add_plugin(PulpNoEditorLinkFloorControl
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
    foreach(_forbidden pulp-format pulp-format-view pulp-view pulp-view-core pulp-view-native
                       pulp-view-script pulp-canvas pulp-render pulp-standalone)
        if(_forbidden IN_LIST _no_editor_closure)
            message(FATAL_ERROR
                "PulpNoEditorLinkFloor_${_fmt} reaches ${_forbidden}: ${_no_editor_closure}")
        endif()
    endforeach()
    if(NOT pulp-format-core IN_LIST _no_editor_closure)
        message(FATAL_ERROR
            "PulpNoEditorLinkFloor_${_fmt} lost pulp-format-core: ${_no_editor_closure}")
    endif()
endforeach()

# What the control's editor stack contains in this configuration. View and
# canvas always; the GPU layers only when the build has them.
set(_no_editor_control_families view canvas)
set(_no_editor_control_gpu OFF)
if(PULP_ENABLE_GPU AND TARGET pulp-render AND (PULP_HAS_SKIA OR PULP_HAS_WEBGPU))
    list(APPEND _no_editor_control_families render wgpu)
    set(_no_editor_control_gpu ON)
    if(PULP_HAS_SKIA)
        list(APPEND _no_editor_control_families skia)
        # Skia's own Dawn archive (the same test core/render uses).
        if(DAWN_LIBRARY AND NOT DAWN_LIBRARY STREQUAL "DAWN_LIBRARY-NOTFOUND")
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

