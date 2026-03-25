add_library(pulp-view)
add_library(pulp::view ALIAS pulp-view)

target_include_directories(pulp-view
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-view PRIVATE
    src/script_engine.cpp
    src/theme.cpp
    src/view.cpp
    src/widgets.cpp
    src/widget_bridge.cpp
    src/hot_reload.cpp
    src/inspector.cpp
    src/sdl_window_host.cpp
    src/auto_ui.cpp
)

# CHOC headers (JS engine, utilities)
target_include_directories(pulp-view PUBLIC
    $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
)

target_link_libraries(pulp-view PUBLIC pulp::canvas pulp::events pulp::state)

# SDL3 for cross-platform windowing
if(PULP_HAS_SDL3)
    target_link_libraries(pulp-view PRIVATE SDL3::SDL3-static)
    target_compile_definitions(pulp-view PRIVATE PULP_HAS_SDL3=1)
endif()

# Platform-specific window host and frameworks
if(APPLE)
    target_sources(pulp-view PRIVATE
        platform/mac/window_host_mac.mm
        platform/mac/plugin_view_host_mac.mm
        platform/mac/screenshot_mac.mm
    )
    target_link_libraries(pulp-view PRIVATE
        "-framework CoreServices"
        "-framework Cocoa"
        "-framework ImageIO"
        "-framework UniformTypeIdentifiers"
    )
endif()
