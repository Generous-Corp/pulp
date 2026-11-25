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

# A dev build (PULP_DEV_BUILD, on whenever tests are) compiles the on-disk
# ui/main.js path into the Standalone image's generated source, which is a
# runtime read of the checkout; declare it so selectors account for it.
if(TARGET PulpUiScriptFixture_Standalone AND PULP_DEV_BUILD)
    pulp_test_data(PulpUiScriptFixture_Standalone
        PATHS test/fixtures/ui_script_plugin/ui/main.js
        NO_DEFINE)
endif()

# The same plug-in declared with UI_ENGINE vellum: the script and its embedded
# assets open through the Vellum editor factory, so each format module and the
# standalone app link no Pulp view layer. Built only where the scripted Vellum
# bridge is (PULP_ENABLE_PULP_VELLUM with an SDK exporting
# Vellum::MirrorView_script); nothing here is launched.
if(TARGET pulp-vellum-scripted AND CMAKE_NM)
    pulp_add_plugin(PulpUiScriptVellumFixture
        FORMATS           ${_ui_script_formats}
        PLUGIN_NAME       "Pulp UI Script Vellum Fixture"
        BUNDLE_ID         "dev.pulp.test.ui-script-vellum-fixture"
        MANUFACTURER      "Pulp"
        VERSION           "1.0.0"
        CATEGORY          Effect
        PLUGIN_CODE       "PUiV"
        MANUFACTURER_CODE "PlUs"
        SOURCES           ui_script_gain.hpp
        PROCESSOR_FACTORY pulp::test_fixtures::create_ui_script_gain
        UI_SCRIPT         ui/main.js
        UI_ENGINE         vellum
    )
    if(TARGET PulpUiScriptVellumFixture_AU)
        target_compile_definitions(PulpUiScriptVellumFixture_AU PRIVATE
            PULP_UI_SCRIPT_FIXTURE_AU_CLASS=PulpUiScriptVellumFixtureAU)
    endif()
    set(_vellum_route_targets "")
    foreach(_fmt IN LISTS _ui_script_formats)
        if(NOT TARGET PulpUiScriptVellumFixture_${_fmt})
            continue()
        endif()
        list(APPEND _vellum_route_targets PulpUiScriptVellumFixture_${_fmt})
        string(TOLOWER "${_fmt}" _route_format)
        add_test(NAME ui-script-vellum-route-${_route_format}
            COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM}
                "-DBINARY=$<TARGET_FILE:PulpUiScriptVellumFixture_${_fmt}>" -DROUTE=vellum
                -P "${CMAKE_CURRENT_SOURCE_DIR}/check_vellum_route.cmake")
        set_tests_properties(ui-script-vellum-route-${_route_format} PROPERTIES
            LABELS "pulp-vellum;format;scripted-ui" TIMEOUT 60)
    endforeach()
    # The census on fixed listings (cat stands in for nm), with symbols taken
    # from real images. A listing with Pulp's editor stack reads as legacy. An
    # image that defines the Vellum factory beside that stack (what a
    # UI_ENGINE vellum plug-in linked to the view layer looks like) fails the
    # vellum verdict. Undefined references and std instantiations over a
    # pulp::view type do not count against a Vellum image.
    find_program(_ui_script_census_cat cat)
    if(_ui_script_census_cat)
        foreach(_control legacy-passes mixed-not-vellum vellum-ignores-references)
            if(_control STREQUAL "legacy-passes")
                set(_listing legacy_route.nm)
                set(_route legacy)
            elseif(_control STREQUAL "mixed-not-vellum")
                set(_listing mixed_route.nm)
                set(_route vellum)
            else()
                set(_listing vellum_route.nm)
                set(_route vellum)
            endif()
            add_test(NAME ui-script-vellum-route-census-${_control}
                COMMAND ${CMAKE_COMMAND} -DNM=${_ui_script_census_cat}
                    "-DBINARY=${CMAKE_CURRENT_SOURCE_DIR}/${_listing}" -DROUTE=${_route}
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_vellum_route.cmake")
            set_tests_properties(ui-script-vellum-route-census-${_control} PROPERTIES
                LABELS "pulp-vellum;format;scripted-ui" TIMEOUT 60)
        endforeach()
        set_tests_properties(ui-script-vellum-route-census-mixed-not-vellum PROPERTIES
            WILL_FAIL TRUE)
    endif()
    add_custom_target(pulp-ui-script-vellum-fixture DEPENDS ${_vellum_route_targets})
    # The census reads built plug-in images, which no labelled test executable
    # links, so a lane that builds only its test executables would hand the
    # census a missing file. Build them first, on a governed share of the host.
    add_test(NAME ui-script-vellum-route-build
        COMMAND "${PROJECT_SOURCE_DIR}/tools/ci/governed-build.sh"
            ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target pulp-ui-script-vellum-fixture)
    set_tests_properties(ui-script-vellum-route-build PROPERTIES
        LABELS "pulp-vellum;format;scripted-ui" TIMEOUT 1800
        FIXTURES_SETUP ui-script-vellum-route)
    get_property(_vellum_route_tests DIRECTORY PROPERTY TESTS)
    list(FILTER _vellum_route_tests INCLUDE REGEX "^ui-script-vellum-route-(vst3|clap|au|standalone)$")
    set_tests_properties(${_vellum_route_tests} PROPERTIES FIXTURES_REQUIRED ui-script-vellum-route)
endif()

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
    # An instrumented producer (sanitizer or coverage) installs static
    # libraries that reference its runtime, so the consumer must compile and
    # link with the same instrumentation, as the other installed-SDK consumer
    # fixtures do.
    string(JOIN " " _ui_script_consumer_instrumentation_compile_flags
        ${PULP_SANITIZER_COMPILE_FLAGS} ${PULP_COVERAGE_COMPILE_FLAGS})
    string(JOIN " " _ui_script_consumer_instrumentation_link_flags
        ${PULP_SANITIZER_LINK_FLAGS} ${PULP_COVERAGE_LINK_FLAGS})
    add_test(NAME ui-script-plugin-editor-installed-sdk
        COMMAND ${CMAKE_COMMAND}
            -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
            "-DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DPROBE=$<TARGET_FILE:pulp-ui-script-editor-probe>"
            "-DEXPECT_GPU=${PULP_HAS_SKIA}"
            "-DCONFIG=$<CONFIG>"
            "-DPULP_PARENT_SANITIZER=${PULP_SANITIZER}"
            "-DPULP_PARENT_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
            "-DPULP_PARENT_INSTRUMENTATION_CXX_FLAGS=${_ui_script_consumer_instrumentation_compile_flags}"
            "-DPULP_PARENT_INSTRUMENTATION_LINKER_FLAGS=${_ui_script_consumer_instrumentation_link_flags}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/sdk_consumer.cmake")
    # The required macOS gate runs it: one install plus one small CLAP consumer
    # costs about as much as cmake-view-audio-sdk-consumer beside it. Elsewhere
    # it rides the slow lane with the other installed-SDK builds until a
    # consumer build there is measured.
    set(_ui_script_sdk_consumer_labels "cmake;sdk;format;view;scripted-ui")
    if(NOT APPLE)
        list(APPEND _ui_script_sdk_consumer_labels slow)
    endif()
    set_tests_properties(ui-script-plugin-editor-installed-sdk PROPERTIES
        LABELS "${_ui_script_sdk_consumer_labels}"
        COST 30
        TIMEOUT 300
        ENVIRONMENT_MODIFICATION "${_ui_script_editor_env}")
endif()
