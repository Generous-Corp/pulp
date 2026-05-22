add_library(pulp-view-core)
add_library(pulp::view-core ALIAS pulp-view-core)

add_library(pulp-view-script)
add_library(pulp::view-script ALIAS pulp-view-script)

add_library(pulp-view src/placeholder.cpp)
add_library(pulp::view ALIAS pulp-view)
target_link_libraries(pulp-view PUBLIC pulp::view-script)

find_package(Python3 COMPONENTS Interpreter QUIET)

# ── Embed JS preludes as C++ string constants ────────────────────────────────
set(PULP_JS_PRELUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/js/css-colors.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/css-parser.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-element.js
    # P5-7 follow-up — Events + Pointer-capture (P2b) extracted from
    # web-compat-element.js. Loaded AFTER the parent so the Element
    # constructor + prototype are already defined when the extracted
    # prototype overrides install.
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-element-events.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-canvas.js
    # P5-6 first cut — native GPU helpers extracted from
    # web-compat-canvas.js. Loaded AFTER the parent so the
    # CanvasRenderingContext2D constructor + window.pulp.gpu surface
    # are in scope when GPU consumers call getContext("webgpu").
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-canvas-gpu.js
    # P5-6 follow-up — _PulpCanvasMatrix DOMMatrix-compat helper.
    # CanvasRenderingContext2D.getTransform() instantiates this lazily
    # at call time so embed order relative to canvas.js is flexible.
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-canvas-matrix.js
    # P5-6 follow-up — Canvas2D API gap-closure methods (issue-916):
    # measureText / drawImage / setLineDash / getLineDash /
    # getImageData / putImageData. Loaded AFTER the parent so the
    # CanvasRenderingContext2D constructor is in scope.
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-canvas-image.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl.js
    # P5-5 — per-domain `_applyProperty` handler modules. The former
    # monolithic per-CSS-property switch is split into layout / paint /
    # typography / transform / misc handlers; web-compat-style-decl.js's
    # `_applyProperty` is now a thin dispatcher that calls each in turn.
    # These are plain function declarations referenced by the dispatcher
    # at call time, so they MUST embed AFTER web-compat-style-decl.js
    # (and before any consumer that triggers a style apply).
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl-layout.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl-paint.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl-typography.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl-transform.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl-misc.js
    # P5-5 first cut — _cssToFlex + __cssProperties__ IIFE +
    # setProperty/getPropertyValue/removeProperty. Loaded AFTER
    # web-compat-style-decl.js so the CSSStyleDeclaration constructor
    # + _applyProperty prototype method are in scope when the IIFE
    # walks the property list and installs reflection.
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-style-decl-helpers.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-document.js
    # P5-7 first cut — CSS selector engine extracted from
    # web-compat-document.js. Loaded AFTER the parent so document +
    # Element are in scope when underscore-prefixed selector helpers
    # are dispatched from document.querySelector / .querySelectorAll.
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-document-selectors.js
    # P5-7 follow-up — WebGPU mock factories extracted from
    # web-compat-document.js. Loaded AFTER the parent so GPU* usage
    # constants installed by document.js (GPUTextureUsage etc.) are in
    # scope when the factory bodies resolve them at call time.
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-document-gpu-mock.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-dom-ops.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-gpu-buffered.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-observers.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/web-compat-scheduler.js
    ${CMAKE_CURRENT_SOURCE_DIR}/js/import-runtime.js
)
set(PULP_JS_EMBEDDED_HEADER ${CMAKE_CURRENT_BINARY_DIR}/web_compat_preludes_gen.hpp)
set(PULP_JS_EMBEDDED_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/web_compat_preludes_gen.cpp)

# pulp #1382 — embed_js.cmake's `string(SUBSTRING)` chunker operates on
# BYTES, so a 12000-byte chunk boundary that lands inside a UTF-8
# multi-byte codepoint splits the codepoint mid-sequence. The result
# is a generated file with invalid UTF-8 at chunk boundaries; MSVC
# then mis-parses the trailing `)__JS__"` delimiter, walks past it
# into the next R"__JS__()__JS__" header, and reports C2026 ("string
# too big") on the runaway literal. Used to be a non-issue when the
# JS preludes were ASCII-only; the css-colors / css-parser / canvas
# files now contain UTF-8 (em-dashes, smart quotes) that can land on
# any byte boundary.
#
# The Python implementation (embed_js.py) slices on Python str, which
# is char-aware, so chunk boundaries always fall between codepoints.
# Switch to that.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# P8-5 (#2641) — split the embedded preludes into an extern-linked TU so a
# JS-prelude edit no longer recompiles the 9.7k-line widget_bridge.cpp.
#
# * The DECLARATION header is generated here at *configure* time from the
#   prelude file list, written only-if-changed. Its content depends only on
#   the file *names* (not their contents), so editing a .js prelude never
#   bumps its mtime — and thus never recompiles widget_bridge.cpp, which
#   includes only this header. (Mirrors tools/cmake/PulpEmbedData.cmake.)
# * The DEFINITION .cpp is generated at *build* time by embed_js.py and
#   compiled as its own translation unit; a .js edit rebuilds only this .cpp
#   + relink.
#
# The var-name derivation (stem with '-' -> '_') MUST match embed_js.py.
set(_pulp_prelude_decls
    "#pragma once\n// Auto-generated from the JS prelude file list — do not edit\n\nnamespace pulp::view::preludes {\n\n")
foreach(_prelude ${PULP_JS_PRELUDES})
    get_filename_component(_prelude_stem "${_prelude}" NAME_WE)
    string(REPLACE "-" "_" _prelude_var "${_prelude_stem}")
    string(APPEND _pulp_prelude_decls "extern const char* const ${_prelude_var};\n")
endforeach()
string(APPEND _pulp_prelude_decls "\n}  // namespace pulp::view::preludes\n")

set(_pulp_existing_decls "")
if(EXISTS "${PULP_JS_EMBEDDED_HEADER}")
    file(READ "${PULP_JS_EMBEDDED_HEADER}" _pulp_existing_decls)
endif()
if(NOT "${_pulp_existing_decls}" STREQUAL "${_pulp_prelude_decls}")
    file(WRITE "${PULP_JS_EMBEDDED_HEADER}" "${_pulp_prelude_decls}")
endif()

add_custom_command(
    OUTPUT ${PULP_JS_EMBEDDED_SOURCE}
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/js/embed_js.py
        ${PULP_JS_EMBEDDED_SOURCE}
        ${PULP_JS_PRELUDES}
    DEPENDS
        ${PULP_JS_PRELUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/js/embed_js.py
    COMMENT "Embedding JS preludes into C++ source (UTF-8-safe Python chunker)"
    VERBATIM
)

# The generated .cpp is a real pulp-view-script source: CMake wires the
# custom-command dependency automatically, so no separate aggregator target
# is needed. widget_bridge.cpp includes only the (stable) declaration header.
target_sources(pulp-view-script PRIVATE ${PULP_JS_EMBEDDED_SOURCE})

foreach(_pulp_view_target pulp-view-core pulp-view-script)
    target_include_directories(${_pulp_view_target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
endforeach()
unset(_pulp_view_target)

target_include_directories(pulp-view-script
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}  # For generated web_compat_preludes_gen.{hpp,cpp}
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
        target_include_directories(pulp-view-script PRIVATE ${V8_INCLUDE_DIR})
        target_link_directories(pulp-view-script PRIVATE ${V8_LIB_DIR})
        if(V8_LIBRARY_PATH)
            target_link_libraries(pulp-view-script PRIVATE ${V8_LIBRARY_PATH})
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
                target_link_libraries(pulp-view-script PRIVATE ${PULP_V8_PROVIDER_LIBRARY})
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
    target_compile_definitions(pulp-view-script PRIVATE PULP_HAS_V8=1)
endif()

# Only set non-QuickJS default when explicitly requested (not auto)
if(PULP_JS_ENGINE STREQUAL "jsc")
    if(APPLE)
        target_compile_definitions(pulp-view-script PRIVATE PULP_DEFAULT_ENGINE_JSC=1)
    endif()
elseif(PULP_JS_ENGINE STREQUAL "v8" AND PULP_HAS_V8_ACTUAL)
    target_compile_definitions(pulp-view-script PRIVATE PULP_DEFAULT_ENGINE_V8=1)
endif()
# auto → QuickJS default (backward compatible with all existing code)

# Slice 2.6: per-platform TextAccessibilityNode backend selection.
# Each platform leg overrides the cross-platform "none" default at link
# time by compiling a platform-specific TU INSTEAD of the default
# src/text_accessibility.cpp. The platform TUs export the same symbols
# (register_text_accessibility_node etc.) so callers don't need to
# branch on the build target.
#
# macOS: NSAccessibility-backed (.mm, "macos-ax").
# Windows: UIA IRawElementProviderSimple ("windows-uia").
# Linux: AccessKit stub ("linux-accesskit-stub").
# Other platforms: default C++ "none" backend.
if(APPLE)
    set(PULP_TEXT_ACCESSIBILITY_SOURCE platform/mac/text_accessibility_macos.mm)
elseif(WIN32)
    set(PULP_TEXT_ACCESSIBILITY_SOURCE platform/win/text_accessibility_windows.cpp)
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    set(PULP_TEXT_ACCESSIBILITY_SOURCE platform/linux/text_accessibility_linux.cpp)
else()
    set(PULP_TEXT_ACCESSIBILITY_SOURCE src/text_accessibility.cpp)
endif()

target_sources(pulp-view-core PRIVATE
    src/accessibility.cpp
    ${PULP_TEXT_ACCESSIBILITY_SOURCE}
    src/table_model.cpp
    src/modulation_matrix.cpp
    src/modulation_matrix_widget.cpp
    src/accessibility_tree.cpp
    src/image_cache.cpp
    src/resizable_shell.cpp
    src/graph_editor_view.cpp
    src/theme.cpp
    src/theme_contrast.cpp
    src/theme_presets.cpp
    src/appearance_tracker.cpp
    src/motion_preferences.cpp
    src/asset_manager.cpp
    src/script_event_dispatch.cpp
    src/view.cpp
    src/yoga_layout.cpp
    src/layout_snapshot.cpp
    src/widgets.cpp
    src/widgets/label.cpp
    src/widgets/visualizers.cpp
    src/editor_bridge.cpp
    src/inspector.cpp
    src/sdl_window_host.cpp
    src/auto_ui.cpp
    src/text_editor.cpp
    src/ui_components.cpp
    src/anchor_strategy.cpp
    src/design_export.cpp
    src/design_import.cpp
    src/design_import_native_common.cpp
    src/design_cpp_codegen.cpp
    src/design_tokens.cpp
    src/design_codegen.cpp
    src/lock_to_source.cpp
    src/jsx_lock.cpp
    src/design_import_shortcuts.cpp
    src/design_import_designmd.cpp
    src/token_lock.cpp
    src/screenshot_compare.cpp
    src/app_framework.cpp
    src/canvas_widget.cpp
    src/svg_path_widget.cpp
    src/widgets/svg_rect.cpp
    src/widgets/svg_line.cpp
    src/visualization_bridge.cpp
    src/frame_clock.cpp
    src/motion.cpp
    src/motion_geometry.cpp
    src/motion_cost.cpp
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

target_sources(pulp-view-script PRIVATE
    ${PULP_JS_ENGINE_SOURCES}
    src/script_engine.cpp
    src/js_engine_recommend.cpp
    src/widget_bridge.cpp
    src/widget_bridge_input.cpp
    src/hot_reload.cpp
    src/scripted_ui.cpp
    src/claude_bundle.cpp
    src/claude_bundle_sources.cpp
)

# WebView support — compiled separately to avoid platform WebView runtime
# auto-loading in plugin hosts. Only link into standalone apps, not
# AU/VST3/CLAP plugins.
# The ObjC runtime loads WebKit on dylib load if any WKWebView references exist,
# which crashes Logic Pro's sandboxed AUHostingServiceXPC process.
# ── Linux ATK accessibility (#18 phase 1) ───────────────────────────────────
#
# Pulp publishes accessibility info to Linux screen readers (Orca) via
# ATK + the at-spi2-atk bridge. This slice only wires the ATK headers
# into the build so accessibility_linux.cpp can use the real AtkRole
# enum instead of hard-coded integer constants. The full AtkObject
# vtable + at-spi2 bridge runtime is a follow-up PR that needs
# screen-reader validation on a real Linux desktop.
#
# Without libatk-dev, accessibility_linux.cpp keeps the pre-existing
# fallback constants so contributors on minimal images still build.
#
# Install on Debian/Ubuntu: sudo apt install -y libatk1.0-dev
if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PULP_ATK QUIET IMPORTED_TARGET atk)
        if(PULP_ATK_FOUND)
            target_compile_definitions(pulp-view-core PRIVATE PULP_HAS_ATK=1)
            target_link_libraries(pulp-view-core PRIVATE PkgConfig::PULP_ATK)
            message(STATUS
                "Pulp: Linux ATK ${PULP_ATK_VERSION} found — "
                "accessibility_linux.cpp will use real AtkRole enum")
        else()
            message(STATUS
                "Pulp: Linux ATK dev headers not found "
                "(install: sudo apt install libatk1.0-dev) — "
                "accessibility_linux.cpp will use stub constants")
        endif()
    endif()
endif()

option(PULP_BUILD_WEBVIEW "Include native WebView support (WebKit / WebView2 / WebKitGTK)" OFF)
if(PULP_BUILD_WEBVIEW)
    target_sources(pulp-view-core PRIVATE src/web_view.cpp)
    if(APPLE AND NOT PULP_IOS)
        target_link_libraries(pulp-view-core PRIVATE "-framework WebKit")
    elseif(UNIX AND NOT APPLE AND NOT ANDROID)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
        pkg_check_modules(WEBKIT2GTK REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
        target_link_libraries(pulp-view-core PRIVATE PkgConfig::GTK3 PkgConfig::WEBKIT2GTK)
    # Android: WebView is handled by the Kotlin layer, not native GTK
    endif()
endif()

# CHOC headers (JS engine, utilities)
foreach(_pulp_view_target pulp-view-core pulp-view-script)
    target_include_directories(${_pulp_view_target} PUBLIC
        $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
    )
endforeach()
unset(_pulp_view_target)

target_link_libraries(pulp-view-core PUBLIC pulp::canvas pulp::events pulp::state pulp::signal pulp::platform pulp::host)
target_link_libraries(pulp-view-script PUBLIC pulp::view-core)
# pulp #468 — design_import.cpp uses pulp::runtime::base64_decode +
# gzip_decompress to unpack Claude Design's base64-gzip envelope.
target_link_libraries(pulp-view-core PRIVATE pulp::runtime)
target_link_libraries(pulp-view-script PRIVATE pulp::runtime)

# yaml-cpp (MIT) — DESIGN.md frontmatter parsing.
# Wrapped in $<BUILD_INTERFACE:...> because yaml-cpp is a build-only
# dependency: Pulp's public headers never expose YAML types, so
# downstream consumers of installed pulp-view (via PulpTargets export
# set) must not see yaml-cpp as a transitive requirement.
target_link_libraries(pulp-view-core PRIVATE $<BUILD_INTERFACE:yaml-cpp::yaml-cpp>)

# JavaScriptCore framework for JSC backend (Apple only)
if(APPLE)
    target_link_libraries(pulp-view-script PRIVATE "-framework JavaScriptCore")
endif()

if(WIN32)
    # Note: popen/pclose defines removed — using pulp::platform::ChildProcess instead
endif()

# Yoga layout engine (MIT)
if(PULP_HAS_YOGA)
    target_link_libraries(pulp-view-core PRIVATE
        $<BUILD_INTERFACE:yogacore>
        $<INSTALL_INTERFACE:Pulp::yogacore>
    )
    target_compile_definitions(pulp-view-core PRIVATE PULP_HAS_YOGA=1)
endif()

# SDL3 for cross-platform windowing
if(PULP_HAS_SDL3)
    target_link_libraries(pulp-view-core PRIVATE
        $<BUILD_INTERFACE:SDL3::SDL3-static>
        $<INSTALL_INTERFACE:Pulp::sdl3-static>
    )
    target_compile_definitions(pulp-view-core PRIVATE PULP_HAS_SDL3=1)
endif()

# Platform-specific window host and frameworks
if(APPLE)
    if(PULP_IOS)
        target_sources(pulp-view-core PRIVATE
            platform/ios/window_host_ios.mm
            platform/ios/plugin_view_host_ios.mm
            platform/ios/accessibility_ios.mm
            # #299 + #313 Codex P1 follow-up: bridge stubs compile
            # on iOS too. They provide set_factory /
            # set_screenshot_provider / has_* registration APIs
            # unconditionally; create/render impls stay gated by
            # their own #if checks so the iOS-native window +
            # plugin-view impls aren't shadowed. screenshot has no
            # iOS-native impl yet, so the stub's provider-backed
            # path is the one iOS actually uses.
            src/plugin_view_host_stub.cpp
            src/screenshot_stub.cpp
            src/window_host_stub.cpp
        )
        target_link_libraries(pulp-view-core PRIVATE
            "-framework UIKit"
            "-framework CoreGraphics"
            "-framework QuartzCore"
        )
    else()
        target_sources(pulp-view-core PRIVATE
            platform/mac/window_host_mac.mm
            platform/mac/window_host_mac_capture.mm
            platform/mac/window_host_mac_geometry.mm
            platform/mac/window_manager_mac.mm
            platform/mac/plugin_view_host_mac.mm
            platform/mac/screenshot_mac.mm
            platform/mac/drag_drop_mac.mm
            platform/mac/accessibility_mac.mm
            platform/mac/appearance_mac.mm
            platform/mac/motion_preferences_mac.mm
            # #299: host-backend registration APIs. The stub files
            # compile set_factory/set_screenshot_provider etc.
            # unconditionally; only the create/render impls are
            # gated by `#if !defined(__APPLE__)`. Including them
            # here surfaces the registration API on mac without
            # conflicting with the native impls.
            src/plugin_view_host_stub.cpp
            src/screenshot_stub.cpp
            src/window_host_stub.cpp
        )
        target_link_libraries(pulp-view-core PRIVATE
            "-framework CoreServices"
            "-framework Cocoa"
            "-framework ImageIO"
            "-framework UniformTypeIdentifiers"
        )
        # GPU rendering: link render module and Metal for CAMetalLayer-backed window host
        if(PULP_HAS_SKIA)
            target_link_libraries(pulp-view-core PRIVATE pulp::render
                "-framework Metal"
                "-framework QuartzCore"
            )
        endif()
    endif()
elseif(WIN32)
    target_sources(pulp-view-core PRIVATE
        src/plugin_view_host_stub.cpp
        src/screenshot_stub.cpp
        src/window_host_stub.cpp
        platform/win/accessibility_win.cpp
        platform/win/appearance_win.cpp
        platform/win/motion_preferences_win.cpp
    )
    # Workstream 04 #247 phase 1: UIAutomationCore for
    # UiaClientsAreListening and (in phase 2) UiaReturnRawElementProvider +
    # UiaRaiseStructureChangedEvent.
    target_link_libraries(pulp-view-core PRIVATE uiautomationcore)
elseif(ANDROID)
    target_sources(pulp-view-core PRIVATE
        src/plugin_view_host_stub.cpp
        src/screenshot_stub.cpp
        src/window_host_stub.cpp
    )
    # Android accessibility wired via Kotlin TalkBack integration
elseif(UNIX)
    target_sources(pulp-view-core PRIVATE
        src/plugin_view_host_stub.cpp
        src/screenshot_stub.cpp
        src/window_host_stub.cpp
        platform/linux/accessibility_linux.cpp
    )
    # Linux AT-SPI bridge (workstream 04 #248). Optional: linking atk-
    # bridge-2.0 when pkg-config finds it enables screen-reader
    # (Orca) registration via atk_bridge_adaptor_init. Skipped on
    # minimal/headless distros so the library still loads.
    if(PkgConfig_FOUND OR PKG_CONFIG_FOUND)
        pkg_check_modules(ATK_BRIDGE IMPORTED_TARGET atk-bridge-2.0)
        if(ATK_BRIDGE_FOUND)
            target_compile_definitions(pulp-view-core PRIVATE PULP_HAS_ATSPI=1)
            target_link_libraries(pulp-view-core PRIVATE PkgConfig::ATK_BRIDGE)
            message(STATUS "Pulp: AT-SPI bridge enabled (atk-bridge-2.0)")
        endif()
    endif()
endif()

# Link against render for native GPU bridge (Three.js, WebGPU canvas).
# Codex P1 on PR #526 / #500: previously visualization_bridge.hpp
# directly included pulp/render/bench/perf_counters.hpp under
# PULP_BENCHMARK, which forced this link to go PUBLIC and leaked the
# render include tree through every view consumer. The header now
# forward-declares PerfCounters and the dereferencing code lives in
# visualization_bridge.cpp — so the link stays PRIVATE regardless of
# PULP_BENCHMARK, and downstream targets that only use view APIs do
# NOT need to link pulp::render when PULP_BENCHMARK is enabled.
if(TARGET pulp-render)
    target_link_libraries(pulp-view-core PRIVATE pulp::render)
    target_link_libraries(pulp-view-script PRIVATE pulp::render)
    # Slice 16 — when pulp-render is linked, WindowHost::mark_dirty() can
    # route dirty marks through a vblank-paced RenderLoop. GPU-off builds
    # (sanitizer matrix) have no pulp-render target, so mark_dirty() there
    # degrades to a direct repaint(); the define gates that cleanly.
    target_compile_definitions(pulp-view-core PRIVATE PULP_VIEW_HAS_RENDER_LOOP=1)
endif()
