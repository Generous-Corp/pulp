# brew-ui — the suite's shared editor furniture: palette, parameter-bound
# widgets, the bipolar voltage rail, and the indicator lamp.
add_library(brew-ui INTERFACE)
target_include_directories(brew-ui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(brew-ui INTERFACE brew-core pulp::view pulp::canvas)

# Both binaries below construct the editors through the real
# Processor::create_view() path, so both need every plug-in's view source and
# every plug-in's include directory. One list, so adding a plug-in is one edit.
set(_brew_plugins cv_osc dc function lfo quantizer step sync)
set(_brew_view_sources "")
set(_brew_plugin_dirs "")
foreach(plugin ${_brew_plugins})
    list(APPEND _brew_view_sources
        ${CMAKE_CURRENT_SOURCE_DIR}/../${plugin}/${plugin}_view.cpp)
    list(APPEND _brew_plugin_dirs ${CMAKE_CURRENT_SOURCE_DIR}/../${plugin})
endforeach()

if(PULP_BUILD_TESTS)
    # pulp::osc and pulp::events because CV To OSC's editor pulls in its
    # processor header and its mDNS Browse.
    add_executable(brew-ui-test test_brew_ui.cpp ${_brew_view_sources})
    target_link_libraries(brew-ui-test
        PRIVATE pulp::format pulp::osc pulp::events brew-core brew-ui
                Catch2::Catch2WithMain)
    target_include_directories(brew-ui-test PRIVATE ${_brew_plugin_dirs})
    catch_discover_tests(brew-ui-test)

    # Headless screenshot dumper: renders each editor to PNG with no window.
    add_executable(brew-shots brew_shots.cpp ${_brew_view_sources})
    target_link_libraries(brew-shots
        PRIVATE pulp::format pulp::osc pulp::events brew-core brew-ui)
    target_include_directories(brew-shots PRIVATE ${_brew_plugin_dirs})
endif()
