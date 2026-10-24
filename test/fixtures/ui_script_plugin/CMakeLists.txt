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

# The format adapters refuse to open an editor when CI, PULP_HEADLESS,
# PULP_TEST_MODE or PULP_DISABLE_PLUGIN_EDITOR is set, so a plug-in never pops a
# window during an unattended host scan. CI runners export CI=true, which would
# turn these probes into "editor disabled" failures on every runner. The probes
# exist to open the editor, and the macOS runners have a window server, so they
# run with those switches cleared.
set(_ui_script_editor_env
    "CI=unset:"
    "PULP_HEADLESS=unset:"
    "PULP_TEST_MODE=unset:"
    "PULP_DISABLE_PLUGIN_EDITOR=unset:")

foreach(_fmt IN LISTS _ui_script_formats)
    if(NOT TARGET PulpUiScriptFixture_${_fmt} OR _fmt STREQUAL "Standalone")
        continue()
    endif()
    string(TOLOWER "${_fmt}" _probe_format)
    # The probe disposes the plug-in before AppKit releases the editor view.
    # For AU that is the order in which the view's dealloc runs after the audio
    # unit and its parameter store are gone, so the adapter must say it left
    # that store alone.
    # MallocScribble fills freed memory, so a read of the disposed unit's
    # store crashes the probe even in a Release build instead of passing.
    set(_expect_log "")
    set(_probe_env ${_ui_script_editor_env})
    if(_fmt STREQUAL "AU")
        set(_expect_log "-DEXPECT_LOG=AU v2 editor: view outlived its audio unit")
        list(APPEND _probe_env "MallocScribble=set:1")
    endif()
    add_test(NAME ui-script-plugin-editor-${_probe_format}
        COMMAND ${CMAKE_COMMAND}
            "-DPROBE=$<TARGET_FILE:pulp-ui-script-editor-probe>"
            "-DFORMAT=${_probe_format}"
            "-DBUNDLE=$<TARGET_BUNDLE_DIR:PulpUiScriptFixture_${_fmt}>"
            "-DEXPECT_GPU=${PULP_HAS_SKIA}"
            ${_expect_log}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_editor_mode.cmake")
    set_tests_properties(ui-script-plugin-editor-${_probe_format} PROPERTIES
        LABELS "pr;format;view;scripted-ui" TIMEOUT 120
        ENVIRONMENT_MODIFICATION "${_probe_env}")
endforeach()

# The Standalone opens an audio device, so it is not launched here; in a dev
# build (PULP_DEV_BUILD) its image must still carry the registered script path,
# which an editor build without the per-target registration does not. A
# tests-on configure with the option off records no path, and
# ui-script-plugin-editor-installed-sdk proves that absence on a release build.
if(TARGET PulpUiScriptFixture_Standalone AND PULP_DEV_BUILD)
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
        LABELS "cmake;sdk;format;view;scripted-ui;slow" TIMEOUT 900
        ENVIRONMENT_MODIFICATION "${_ui_script_editor_env}")
endif()
