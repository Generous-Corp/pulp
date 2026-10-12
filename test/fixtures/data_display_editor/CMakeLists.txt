# Audio displays reach a real editor through the paths products take: a plug-in
# built with pulp_add_plugin, opened through format-view, and a hot-reloaded
# editor whose logic resolves the display registry from its host.

if(NOT TARGET pulp-view-audio OR NOT TARGET pulp-format-view OR NOT PULP_ENABLE_DESIGN_IMPORT)
    return()
endif()

if(PULP_HAS_CLAP)
    pulp_add_plugin(PulpDataDisplayFixture
        FORMATS CLAP
        SOURCES data_display_plugin.cpp
        PROCESSOR_FACTORY make_data_display_fixture
        PLUGIN_NAME "Pulp Data Display Fixture"
        BUNDLE_ID "dev.pulp.test.data-display-fixture"
        MANUFACTURER "Pulp"
        VERSION "1.0.0"
        CATEGORY Effect)

    # The plug-in's own processor objects, with its editor opened through
    # ViewBridge.
    add_executable(pulp-test-data-display-plugin-editor test_data_display_plugin_editor.cpp)
    target_link_libraries(pulp-test-data-display-plugin-editor PRIVATE
        PulpDataDisplayFixture_Core pulp::format Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-data-display-plugin-editor
        PROPERTIES LABELS "pr;format;view;data-display")

    # The built plug-in image carries the registration entry point; a binary
    # built without the audio provider is the instrument's negative control.
    if(CMAKE_NM AND NOT WIN32 AND TARGET pulp-test-data-display-link-floor)
        add_test(NAME data-display-plugin-image-registers-audio-widgets
            COMMAND ${CMAKE_COMMAND}
                "-DNM=${CMAKE_NM}"
                "-DIMAGE=$<TARGET_FILE:PulpDataDisplayFixture_CLAP>"
                "-DNEGATIVE_CONTROL=$<TARGET_FILE:pulp-test-data-display-link-floor>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/check_registration_symbol.cmake")
        set_tests_properties(data-display-plugin-image-registers-audio-widgets PROPERTIES
            LABELS "pr;format;view;data-display;link-contract" TIMEOUT 60)
    endif()
endif()

# Hot reload: thin logic resolves pulp symbols from the host at dlopen, which
# is supported on macOS and Linux.
if(NOT WIN32)
    pulp_add_reload_logic(pulp-reload-logic-meter
        SOURCES reload_logic_meter.cpp
        OUTPUT_NAME reload-logic-meter
        RESOLVE_FROM_HOST)
    add_executable(pulp-test-reload-data-display test_reload_data_display.cpp)
    target_link_libraries(pulp-test-reload-data-display PRIVATE
        pulp::format pulp::view Catch2::Catch2WithMain ${CMAKE_DL_LIBS})
    pulp_reload_host_ui(pulp-test-reload-data-display)
    add_dependencies(pulp-test-reload-data-display pulp-reload-logic-meter)
    target_compile_definitions(pulp-test-reload-data-display PRIVATE
        RELOAD_LOGIC_METER="$<TARGET_FILE:pulp-reload-logic-meter>")
    catch_discover_tests(pulp-test-reload-data-display
        PROPERTIES LABELS "pr;format;reload;data-display")
endif()
