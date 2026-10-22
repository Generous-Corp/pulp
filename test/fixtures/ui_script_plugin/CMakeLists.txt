# A plug-in declared with pulp_add_plugin(UI_SCRIPT) opens its scripted editor
# in every format adapter, not AutoUi. The script path is a runtime value the
# plug-in target registers, so this builds a real plug-in with the public CMake
# surface and opens each built bundle through Pulp's host stack.

if(NOT APPLE OR PULP_IOS OR NOT TARGET pulp-host OR NOT TARGET pulp-format-view)
    return()
endif()

set(_ui_script_formats "")
if(PULP_HAS_VST3)
    list(APPEND _ui_script_formats VST3)
endif()
if(PULP_HAS_CLAP)
    list(APPEND _ui_script_formats CLAP)
endif()
if(PULP_HAS_AUSDK)
    list(APPEND _ui_script_formats AU)
endif()
if(NOT _ui_script_formats)
    return()
endif()
list(APPEND _ui_script_formats Standalone)

pulp_add_plugin(PulpUiScriptFixture
    FORMATS           ${_ui_script_formats}
    PLUGIN_NAME       "Pulp UI Script Fixture"
    BUNDLE_ID         "dev.pulp.test.ui-script-fixture"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Effect
    PLUGIN_CODE       "PUiS"
    MANUFACTURER_CODE "PlUs"
    SOURCES           ui_script_gain.hpp
    PROCESSOR_FACTORY pulp::test_fixtures::create_ui_script_gain
    UI_SCRIPT         ui/main.js
)

add_executable(pulp-ui-script-editor-probe editor_probe.mm)
target_link_libraries(pulp-ui-script-editor-probe PRIVATE
    pulp::host "-framework AppKit" "-framework AudioToolbox")
foreach(_fmt IN LISTS _ui_script_formats)
    if(TARGET PulpUiScriptFixture_${_fmt} AND NOT _fmt STREQUAL "Standalone")
        add_dependencies(pulp-ui-script-editor-probe PulpUiScriptFixture_${_fmt})
    endif()
endforeach()

foreach(_fmt IN LISTS _ui_script_formats)
    if(NOT TARGET PulpUiScriptFixture_${_fmt} OR _fmt STREQUAL "Standalone")
        continue()
    endif()
    string(TOLOWER "${_fmt}" _probe_format)
    add_test(NAME ui-script-plugin-editor-${_probe_format}
        COMMAND ${CMAKE_COMMAND}
            "-DPROBE=$<TARGET_FILE:pulp-ui-script-editor-probe>"
            "-DFORMAT=${_probe_format}"
            "-DBUNDLE=$<TARGET_BUNDLE_DIR:PulpUiScriptFixture_${_fmt}>"
            "-DEXPECT_GPU=${PULP_HAS_SKIA}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_editor_mode.cmake")
    set_tests_properties(ui-script-plugin-editor-${_probe_format} PROPERTIES
        LABELS "pr;format;view;scripted-ui" TIMEOUT 120)
endforeach()

# The Standalone opens an audio device, so it is not launched here; its image
# must still carry the registered script path, which an editor build without
# the per-target registration does not.
if(TARGET PulpUiScriptFixture_Standalone)
    add_test(NAME ui-script-plugin-standalone-registers-script
        COMMAND ${CMAKE_COMMAND}
            "-DIMAGE=$<TARGET_FILE:PulpUiScriptFixture_Standalone>"
            "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/ui/main.js"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_image_registers_script.cmake")
    set_tests_properties(ui-script-plugin-standalone-registers-script PROPERTIES
        LABELS "pr;format;scripted-ui" TIMEOUT 60)
endif()

# The same plug-in built outside the tree against the installed SDK.
if(PULP_HAS_CLAP)
    add_test(NAME ui-script-plugin-editor-installed-sdk
        COMMAND ${CMAKE_COMMAND}
            -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
            "-DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DPROBE=$<TARGET_FILE:pulp-ui-script-editor-probe>"
            "-DEXPECT_GPU=${PULP_HAS_SKIA}"
            "-DCONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/sdk_consumer.cmake")
    set_tests_properties(ui-script-plugin-editor-installed-sdk PROPERTIES
        LABELS "cmake;sdk;format;view;scripted-ui;slow" TIMEOUT 900)
endif()
