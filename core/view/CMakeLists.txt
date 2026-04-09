add_library(pulp-view)
add_library(pulp::view ALIAS pulp-view)

find_package(Python3 COMPONENTS Interpreter QUIET)

# ── Embed JS preludes as C++ string constants ────────────────────────────────
set(PULP_JS_PRELUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/js/css-colors.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/css-parser.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-element.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-canvas.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-document.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-dom-ops.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-gpu-buffered.js
)
set(PULP_JS_EMBEDDED_HEADER ${CMAKE_CURRENT_BINARY_DIR}/web_compat_preludes_gen.hpp)

add_custom_command(
    OUTPUT ${PULP_JS_EMBEDDED_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT_FILE=${PULP_JS_EMBEDDED_HEADER}
        -DFILE1=${CMAKE_CURRENT_SOURCE_DIR}/js/css-colors.js
        -DFILE2=${CMAKE_CURRENT_SOURCE_DIR}/js/css-parser.js
        -DFILE3=${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-element.js
        -DFILE4=${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-canvas.js
        -DFILE5=${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl.js
        -DFILE6=${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-document.js
        -DFILE7=${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-dom-ops.js
        -DFILE8=${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-gpu-buffered.js
        -P ${CMAKE_CURRENT_SOURCE_DIR}/js/embed_js.cmake
    DEPENDS
        ${PULP_JS_PRELUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/js/embed_js.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/js/embed_js.py
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

# ── JS engine backend selection ──────────────────────────────────────────────
# PULP_JS_ENGINE: auto | quickjs | jsc | v8
#   auto    → JSC on Apple, QuickJS elsewhere (V8 if PULP_HAS_V8)
#   quickjs → QuickJS only (always available, portable)
#   jsc     → JavaScriptCore (Apple platforms only)
#   v8      → V8 (requires external V8 build, set V8_INCLUDE_DIR / V8_LIB_DIR,
#                 and optionally V8_LIBRARY_PATH or V8_LIBRARY_NAME)
set(PULP_JS_ENGINE "auto" CACHE STRING "JS engine backend (auto|quickjs|jsc|v8)")
set_property(CACHE PULP_JS_ENGINE PROPERTY STRINGS auto quickjs jsc v8)
set(V8_LIBRARY_PATH "" CACHE FILEPATH "Full path to the V8 provider library (e.g. libv8_monolith or libnode)")
set(V8_LIBRARY_NAME "" CACHE STRING "Library name to link for V8 when V8_LIBRARY_PATH is not set")

# QuickJS backend is always compiled (portable fallback)
set(PULP_JS_ENGINE_SOURCES
    src/js_quickjs_engine.cpp
    src/js_engine_factory.cpp
)

# V8 backend (optional, needs external V8)
if(PULP_JS_ENGINE STREQUAL "v8" OR (PULP_JS_ENGINE STREQUAL "auto" AND DEFINED V8_INCLUDE_DIR))
    if(DEFINED V8_INCLUDE_DIR AND DEFINED V8_LIB_DIR)
        list(APPEND PULP_JS_ENGINE_SOURCES src/js_v8_engine.cpp)
        target_include_directories(pulp-view PRIVATE ${V8_INCLUDE_DIR})
        target_link_directories(pulp-view PRIVATE ${V8_LIB_DIR})
        if(V8_LIBRARY_PATH)
            target_link_libraries(pulp-view PRIVATE ${V8_LIBRARY_PATH})
        else()
            set(PULP_V8_PROVIDER_NAMES)
            if(V8_LIBRARY_NAME)
                list(APPEND PULP_V8_PROVIDER_NAMES ${V8_LIBRARY_NAME})
            endif()
            list(APPEND PULP_V8_PROVIDER_NAMES v8_monolith node.141 node)

            find_library(PULP_V8_PROVIDER_LIBRARY
                NAMES ${PULP_V8_PROVIDER_NAMES}
                PATHS ${V8_LIB_DIR}
                NO_DEFAULT_PATH
            )

            if(PULP_V8_PROVIDER_LIBRARY)
                target_link_libraries(pulp-view PRIVATE ${PULP_V8_PROVIDER_LIBRARY})
            else()
                message(FATAL_ERROR
                    "Could not find a V8 provider library in ${V8_LIB_DIR}. "
                    "Set V8_LIBRARY_PATH to a full library path or V8_LIBRARY_NAME to the library basename."
                )
            endif()
        endif()
        set(PULP_HAS_V8_ACTUAL ON)
    else()
        if(PULP_JS_ENGINE STREQUAL "v8")
            message(FATAL_ERROR "PULP_JS_ENGINE=v8 requires V8_INCLUDE_DIR and V8_LIB_DIR")
        endif()
        list(APPEND PULP_JS_ENGINE_SOURCES src/js_v8_engine.cpp)
    endif()
else()
    list(APPEND PULP_JS_ENGINE_SOURCES src/js_v8_engine.cpp)
endif()

# JSC backend (Apple platforms only)
if(APPLE)
    list(APPEND PULP_JS_ENGINE_SOURCES src/js_jsc_engine.mm)
endif()

# Set compile definitions for engine availability / defaults
if(PULP_HAS_V8_ACTUAL)
    target_compile_definitions(pulp-view PRIVATE PULP_HAS_V8=1)
endif()

# Only set non-QuickJS default when explicitly requested (not auto)
if(PULP_JS_ENGINE STREQUAL "jsc")
    if(APPLE)
        target_compile_definitions(pulp-view PRIVATE PULP_DEFAULT_ENGINE_JSC=1)
    endif()
elseif(PULP_JS_ENGINE STREQUAL "v8" AND PULP_HAS_V8_ACTUAL)
    target_compile_definitions(pulp-view PRIVATE PULP_DEFAULT_ENGINE_V8=1)
endif()
# auto → QuickJS default (backward compatible with all existing code)

target_sources(pulp-view PRIVATE
    ${PULP_JS_ENGINE_SOURCES}
    src/script_engine.cpp
    src/js_engine_recommend.cpp
    src/theme.cpp
    src/theme_contrast.cpp
    src/theme_presets.cpp
    src/appearance_tracker.cpp
    src/asset_manager.cpp
    src/view.cpp
    src/yoga_layout.cpp
    src/widgets.cpp
    src/widget_bridge.cpp
    src/hot_reload.cpp
    src/scripted_ui.cpp
    src/inspector.cpp
    src/sdl_window_host.cpp
    src/auto_ui.cpp
    src/text_editor.cpp
    src/ui_components.cpp
    src/design_export.cpp
    src/design_import.cpp
    src/screenshot_compare.cpp
    src/app_framework.cpp
    src/canvas_widget.cpp
    src/frame_clock.cpp
    src/window_manager.cpp
    src/eq_curve_view.cpp
    src/midi_keyboard.cpp
    src/color_picker.cpp
    src/theme_editor.cpp
    src/file_drop_zone.cpp
    src/split_view.cpp
    src/property_list.cpp
    src/breadcrumb.cpp
    src/buttons.cpp
    src/file_browser.cpp
    src/code_editor.cpp
    src/table.cpp
    src/toolbar.cpp
    src/concertina_panel.cpp
    src/splash_screen.cpp
    src/live_constant_editor.cpp
    src/inspector_window.cpp
)

# WebView support — compiled separately to avoid platform WebView runtime
# auto-loading in plugin hosts. Only link into standalone apps, not
# AU/VST3/CLAP plugins.
# The ObjC runtime loads WebKit on dylib load if any WKWebView references exist,
# which crashes Logic Pro's sandboxed AUHostingServiceXPC process.
option(PULP_BUILD_WEBVIEW "Include native WebView support (WebKit / WebView2 / WebKitGTK)" OFF)
if(PULP_BUILD_WEBVIEW)
    target_sources(pulp-view PRIVATE src/web_view.cpp)
    if(APPLE AND NOT PULP_IOS)
        target_link_libraries(pulp-view PRIVATE "-framework WebKit")
    elseif(UNIX AND NOT APPLE AND NOT ANDROID)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
        pkg_check_modules(WEBKIT2GTK REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
        target_link_libraries(pulp-view PRIVATE PkgConfig::GTK3 PkgConfig::WEBKIT2GTK)
    # Android: WebView is handled by the Kotlin layer, not native GTK
    endif()
endif()

# CHOC headers (JS engine, utilities)
target_include_directories(pulp-view PUBLIC
    $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

add_dependencies(pulp-view pulp-view-js-preludes)

target_link_libraries(pulp-view PUBLIC pulp::canvas pulp::events pulp::state pulp::signal pulp::platform)

# JavaScriptCore framework for JSC backend (Apple only)
if(APPLE)
    target_link_libraries(pulp-view PRIVATE "-framework JavaScriptCore")
endif()

if(WIN32)
    # Note: popen/pclose defines removed — using pulp::platform::ChildProcess instead
endif()

# Yoga layout engine (MIT)
if(PULP_HAS_YOGA)
    target_link_libraries(pulp-view PRIVATE
        $<BUILD_INTERFACE:yogacore>
        $<INSTALL_INTERFACE:Pulp::yogacore>
    )
    target_compile_definitions(pulp-view PRIVATE PULP_HAS_YOGA=1)
endif()

# SDL3 for cross-platform windowing
if(PULP_HAS_SDL3)
    target_link_libraries(pulp-view PRIVATE
        $<BUILD_INTERFACE:SDL3::SDL3-static>
        $<INSTALL_INTERFACE:Pulp::sdl3-static>
    )
    target_compile_definitions(pulp-view PRIVATE PULP_HAS_SDL3=1)
endif()

# Platform-specific window host and frameworks
if(APPLE)
    if(PULP_IOS)
        target_sources(pulp-view PRIVATE
            platform/ios/window_host_ios.mm
            platform/ios/plugin_view_host_ios.mm
            platform/ios/accessibility_ios.mm
        )
        target_link_libraries(pulp-view PRIVATE
            "-framework UIKit"
            "-framework CoreGraphics"
            "-framework QuartzCore"
        )
    else()
        target_sources(pulp-view PRIVATE
            platform/mac/window_host_mac.mm
            platform/mac/window_manager_mac.mm
            platform/mac/plugin_view_host_mac.mm
            platform/mac/screenshot_mac.mm
            platform/mac/drag_drop_mac.mm
            platform/mac/accessibility_mac.mm
            platform/mac/appearance_mac.mm
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
        src/plugin_view_host_stub.cpp
        src/screenshot_stub.cpp
        src/window_host_stub.cpp
        platform/win/accessibility_win.cpp
        platform/win/appearance_win.cpp
    )
elseif(ANDROID)
    target_sources(pulp-view PRIVATE
        src/plugin_view_host_stub.cpp
        src/screenshot_stub.cpp
        src/window_host_stub.cpp
    )
    # Android accessibility wired via Kotlin TalkBack integration
elseif(UNIX)
    target_sources(pulp-view PRIVATE
        src/plugin_view_host_stub.cpp
        src/screenshot_stub.cpp
        src/window_host_stub.cpp
        platform/linux/accessibility_linux.cpp
    )
endif()

# Link against render for native GPU bridge (Three.js, WebGPU canvas)
if(TARGET pulp-render)
    target_link_libraries(pulp-view PRIVATE pulp::render)
endif()
