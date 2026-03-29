add_library(pulp-view)
add_library(pulp::view ALIAS pulp-view)

# ── Embed JS preludes as C++ string constants ────────────────────────────────
set(PULP_JS_PRELUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/js/css-colors.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/css-parser.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-element.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-document.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-dom-ops.js
)
set(PULP_JS_EMBEDDED_HEADER ${CMAKE_CURRENT_BINARY_DIR}/web_compat_preludes_gen.hpp)

add_custom_command(
    OUTPUT ${PULP_JS_EMBEDDED_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DFILE1=${CMAKE_CURRENT_SOURCE_DIR}/js/css-colors.js
        -DFILE2=${CMAKE_CURRENT_SOURCE_DIR}/js/css-parser.js
        -DFILE3=${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-element.js
        -DFILE4=${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl.js
        -DFILE5=${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-document.js
        -DFILE6=${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-dom-ops.js
        -DOUTPUT_FILE=${PULP_JS_EMBEDDED_HEADER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/js/embed_js.cmake
    DEPENDS ${PULP_JS_PRELUDES} ${CMAKE_CURRENT_SOURCE_DIR}/js/embed_js.cmake
    COMMENT "Embedding JS preludes into C++ header"
)
add_custom_target(pulp-view-js-preludes DEPENDS ${PULP_JS_EMBEDDED_HEADER})

target_include_directories(pulp-view
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}  # For generated web_compat_preludes_gen.hpp
)

target_sources(pulp-view PRIVATE
    src/script_engine.cpp
    src/theme.cpp
    src/view.cpp
    src/yoga_layout.cpp
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
    src/canvas_widget.cpp
    src/frame_clock.cpp
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

add_dependencies(pulp-view pulp-view-js-preludes)

target_link_libraries(pulp-view PUBLIC pulp::canvas pulp::events pulp::state)

# Yoga layout engine (MIT)
if(PULP_HAS_YOGA)
    target_link_libraries(pulp-view PRIVATE yogacore)
    target_compile_definitions(pulp-view PRIVATE PULP_HAS_YOGA=1)
endif()

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
            platform/mac/accessibility_mac.mm
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
