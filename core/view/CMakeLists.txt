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
    src/text_editor.cpp
    src/ui_components.cpp
    src/design_export.cpp
    src/app_framework.cpp
)

# WebView support — compiled separately to avoid WebKit framework auto-loading
# in plugin hosts. Only link into standalone apps, not AU/VST3/CLAP plugins.
# The ObjC runtime loads WebKit on dylib load if any WKWebView references exist,
# which crashes Logic Pro's sandboxed AUHostingServiceXPC process.
option(PULP_BUILD_WEBVIEW "Include WebView support (requires WebKit)" OFF)
if(PULP_BUILD_WEBVIEW)
    target_sources(pulp-view PRIVATE src/web_view.cpp)
    if(APPLE AND NOT PULP_IOS)
        target_link_libraries(pulp-view PRIVATE "-framework WebKit")
    endif()
endif()

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
    if(PULP_IOS)
        target_sources(pulp-view PRIVATE
            platform/ios/window_host_ios.mm
            platform/ios/plugin_view_host_ios.mm
        )
        target_link_libraries(pulp-view PRIVATE
            "-framework UIKit"
            "-framework CoreGraphics"
            "-framework QuartzCore"
        )
    else()
        target_sources(pulp-view PRIVATE
            platform/mac/window_host_mac.mm
            platform/mac/plugin_view_host_mac.mm
            platform/mac/screenshot_mac.mm
            platform/mac/drag_drop_mac.mm
        )
        target_link_libraries(pulp-view PRIVATE
            "-framework CoreServices"
            "-framework Cocoa"
            "-framework ImageIO"
            "-framework UniformTypeIdentifiers"
        )
        # GPU rendering: link render module and Metal for CAMetalLayer-backed window host
        if(PULP_HAS_SKIA)
            target_link_libraries(pulp-view PRIVATE pulp::render
                "-framework Metal"
                "-framework QuartzCore"
            )
        endif()
    endif()
elseif(WIN32)
    target_sources(pulp-view PRIVATE
        platform/win/accessibility_win.cpp
    )
elseif(UNIX)
    target_sources(pulp-view PRIVATE
        platform/linux/accessibility_linux.cpp
    )
endif()
