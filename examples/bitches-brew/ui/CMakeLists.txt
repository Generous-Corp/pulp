# brew-ui — the suite's shared editor furniture: palette, parameter-bound
# widgets, the bipolar voltage rail, and the indicator lamp.
add_library(brew-ui INTERFACE)
target_include_directories(brew-ui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(brew-ui INTERFACE brew-core pulp::view pulp::canvas)

if(PULP_BUILD_TESTS)
    # The editors are constructed through the real Processor::create_view() path,
    # so the test needs both plug-ins' sources.
    add_executable(brew-ui-test
        test_brew_ui.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../dc/dc_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../lfo/lfo_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../sync/sync_view.cpp)
    target_link_libraries(brew-ui-test
        PRIVATE pulp::format brew-core brew-ui Catch2::Catch2WithMain)
    target_include_directories(brew-ui-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../dc
        ${CMAKE_CURRENT_SOURCE_DIR}/../lfo
        ${CMAKE_CURRENT_SOURCE_DIR}/../sync)
    catch_discover_tests(brew-ui-test)
endif()

# Headless screenshot dumper: renders each editor to PNG with no window.
if(PULP_BUILD_TESTS)
    add_executable(brew-shots
        brew_shots.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../dc/dc_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../lfo/lfo_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../sync/sync_view.cpp)
    target_link_libraries(brew-shots PRIVATE pulp::format brew-core brew-ui)
    target_include_directories(brew-shots PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../dc
        ${CMAKE_CURRENT_SOURCE_DIR}/../lfo
        ${CMAKE_CURRENT_SOURCE_DIR}/../sync)
endif()
