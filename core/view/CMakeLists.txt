# PULP_ENABLE_JS — when OFF, pulp::view is NATIVE-ONLY: the JS scripting +
# web-compat layer (pulp::view-script — QuickJS/JSC/V8 engine + the widget
# bridge + the embedded JS preludes) is neither built nor linked, so a standalone
# built on pulp::view contains zero JS-engine symbols and ships a smaller binary.
# The faithful-vector design-import lane (native Skia draw_svg + native overlays)
# and every native widget still work — view-core is self-contained and carries no
# JS dependency (view-script depends on view-core, not the reverse). Default ON to
# preserve the JS-scripted GPU UI path.
option(PULP_ENABLE_JS "Build the JS scripting + web-compat layer (pulp::view-script)" ON)

add_library(pulp-view-core)
add_library(pulp::view-core ALIAS pulp-view-core)
# Consumers (and view-core's own headers) gate any optional JS use on this.
target_compile_definitions(pulp-view-core PUBLIC PULP_ENABLE_JS=$<BOOL:${PULP_ENABLE_JS}>)

add_library(pulp-view-script)
add_library(pulp::view-script ALIAS pulp-view-script)

add_library(pulp-view src/placeholder.cpp)
add_library(pulp::view ALIAS pulp-view)
if(PULP_ENABLE_JS)
    target_link_libraries(pulp-view PUBLIC pulp::view-script)
else()
    # Native-only: link the JS-free core directly and keep view-script defined
    # (so its still-evaluated config blocks below never dangle) but out of the
    # default build and every binary, so no JS engine is compiled or shipped.
    target_link_libraries(pulp-view PUBLIC pulp::view-core)
    set_target_properties(pulp-view-script PROPERTIES EXCLUDE_FROM_ALL TRUE)

    # Authoritative native-only guard. The binary nm-check (AssertNoJsSymbols.cmake)
    # cannot catch a view-script leak: view-script is a static archive, so unless
    # a symbol is referenced the linker drops it from the final binary even when
    # the archive IS linked — the symbol-grep passes on a misconfigured build.
    # The link INTERFACE is the reliable signal, available here at configure time.
    get_target_property(_pv_links pulp-view LINK_LIBRARIES)
    if(_pv_links MATCHES "view-script")
        message(FATAL_ERROR
            "PULP_ENABLE_JS=OFF but pulp-view links pulp-view-script "
            "(LINK_LIBRARIES=${_pv_links}). The native-only build must not pull "
            "the JS scripting layer. Remove the view-script link from the "
            "PULP_ENABLE_JS=OFF path in core/view/CMakeLists.txt.")
    endif()
endif()

# ── pulp-view-reload-exports (live-swap M2b) ─────────────────────────────────
# A tiny static lib whose single keep-alive TU references the editor ABI surface
# (View/Label/widgets) a hot-reload logic's create_view() resolves FROM THE HOST
# at dlopen. A host force-loads THIS (via pulp_reload_host_ui) so those symbols
# survive dead-strip + are exported — without dragging in the window/SDL-host TUs
# a whole-archive force-load of view-core would. Self-contained: links view-core.
add_library(pulp-view-reload-exports STATIC src/reload_exports.cpp)
add_library(pulp::view-reload-exports ALIAS pulp-view-reload-exports)
target_link_libraries(pulp-view-reload-exports PUBLIC pulp-view-core)

find_package(Python3 COMPONENTS Interpreter QUIET)

# Keep this as a directory-scope include: the generated source and variables
# belong to the core/view targets declared above.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PulpViewJsPreludes.cmake")

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

# Keep this as a directory-scope include: its source list feeds the source
# manifest below and its cache exports are consumed by sibling directories.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PulpViewJsEngines.cmake")

# NSBundle-based loader for the embedded Three.js
# IIFE bundle. Only compiled on iOS / iOS Simulator; desktop/macOS
# lanes use src/threejs_resources_stub.cpp (always returns nullopt).
if(APPLE AND PULP_IOS)
    target_sources(pulp-view-core PRIVATE src/threejs_resources_apple.mm)
endif()

# Per-platform TextAccessibilityNode backend selection.
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

# Source ownership is declarative and intentionally separate from platform/link
# policy below. Keep this include at directory scope so engine and accessibility
# source selections flow into the target manifests.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PulpViewSources.cmake")

# WebView support — compiled separately to avoid platform WebView runtime
# auto-loading in plugin hosts. Only link into standalone apps, not
# AU/VST3/CLAP plugins.
# The ObjC runtime loads WebKit on dylib load if any WKWebView references exist,
# which crashes Logic Pro's sandboxed AUHostingServiceXPC process.
# ── Linux AT-SPI accessibility ─────────────────────────────────────────────
#
# Pulp publishes accessibility info to Linux screen readers (Orca) by
# implementing the AT-SPI2 *wire protocol* directly over D-Bus
# (pulp::platform::DBus, libdbus-1 dlopen'd, honest-fail). There is NO
# build-time or link-time dependency on libatk / libatspi / atk-bridge-2.0 —
# the protocol numbers live in core/view/include/pulp/view/platform/atspi_mapping.hpp
# and accessibility_linux.cpp exports the objects. Roles/states are unit-tested
# offline; the live AT stack is exercised in the Linux VM lane. This also keeps
# the build MIT-clean (the ATK runtime is LGPL-adjacent). PkgConfig is still
# discovered below for X11 / WebView.
find_package(PkgConfig QUIET)

option(PULP_BUILD_WEBVIEW "Include native WebView support (WebKit / WebView2 / WebKitGTK)" OFF)
if(PULP_BUILD_WEBVIEW)
    target_sources(pulp-view-core PRIVATE src/web_view.cpp)
    # PUBLIC so tests and downstream callers see the same value the
    # detector TU was compiled with.
    target_compile_definitions(pulp-view-core PUBLIC PULP_BUILD_WEBVIEW=1)
    if(APPLE AND NOT PULP_IOS)
        # In-process WKWebView snapshot → PNG so native WebView overlays are
        # headlessly capturable (WebViewPanel::snapshot_png()).
        target_sources(pulp-view-core PRIVATE platform/mac/web_view_snapshot_mac.mm)
        target_link_libraries(pulp-view-core PRIVATE "-framework WebKit")
    elseif(UNIX AND NOT APPLE AND NOT ANDROID)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
        pkg_check_modules(WEBKIT2GTK REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
        target_link_libraries(pulp-view-core PRIVATE PkgConfig::GTK3 PkgConfig::WEBKIT2GTK)
    # Android: WebView is handled by the Kotlin layer, not native GTK
    endif()
else()
    # Backend detector honors the documented "none if disabled" contract
    # When WebView is excluded from the build,
    # detect_webview_backend() must return "none" so callers can
    # distinguish "build excluded WebView" from "OS has no WebView".
    target_compile_definitions(pulp-view-core PUBLIC PULP_BUILD_WEBVIEW=0)
endif()

# Backend detection TUs — always compiled into pulp-view-core regardless
# of PULP_BUILD_WEBVIEW so callers can distinguish "this build excluded
# WebView" from "this OS has no usable WebView backend installed".
# `web_view_backend.cpp` owns the symbol on Apple / Android / generic
# OSes; the Windows + Linux per-platform TUs own it on their OS so the
# runtime probe (LoadLibrary / dlopen) can run before the answer is
# returned.
target_sources(pulp-view-core PRIVATE src/web_view_backend.cpp)
if(WIN32)
    target_sources(pulp-view-core PRIVATE platform/win/webview2_backend.cpp)
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    target_sources(pulp-view-core PRIVATE platform/linux/webkitgtk_backend.cpp)
endif()

# CHOC headers (JS engine, utilities)
foreach(_pulp_view_target pulp-view-core pulp-view-script)
    target_include_directories(${_pulp_view_target} PUBLIC
        $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
    )
endforeach()
unset(_pulp_view_target)

target_link_libraries(pulp-view-core PUBLIC
    pulp::canvas pulp::events pulp::state pulp::signal pulp::platform pulp::audio
)
# pulp::host is iOS-restricted: iOS sandbox disallows dlopen of arbitrary
# third-party plugins (no VST3 / CLAP / LV2 hosting), and pulling host into
# the link line on iOS surfaces a `long double to_chars` libc++ availability
# error via `scanner_clap.cpp` → `runtime::log_warn` → `std::format`. The
# header-only `plugin_manager_panel.hpp` lives in pulp-view and includes
# `pulp/host/scanner.hpp`, so consumers that instantiate it on desktop still
# need the symbol — link there. iOS AUv3 .appex bundles never instantiate
# the panel, so dropping the PUBLIC dep there is safe and unblocks the
# iOS AUv3 build smoke.
if(NOT IOS)
    target_link_libraries(pulp-view-core PUBLIC pulp::host)
    # graph_editor_view.cpp uses host::SignalGraph / host::Connection / host::GraphNode
    # directly, so its translation unit only compiles when pulp::host is linked.
    # iOS skips the source entirely; the header is also iOS-guarded so any
    # downstream consumer that includes <pulp/view/widgets/graph_editor_view.hpp>
    # on iOS gets a clean compile-time skip rather than a "file not found" on
    # <pulp/host/signal_graph.hpp>.
    target_sources(pulp-view-core PRIVATE src/graph_editor_view.cpp)
endif()
target_link_libraries(pulp-view-script PUBLIC pulp::view-core)
# design_import.cpp uses pulp::runtime::base64_decode +
# gzip_decompress to unpack Claude Design's base64-gzip envelope.
# PUBLIC because the public header model_manager_view.hpp includes
# <pulp/runtime/model_store.hpp> (exposes runtime::ModelListResult in its API),
# so downstream targets that link only pulp::view must inherit the runtime
# include path + link.
target_link_libraries(pulp-view-core PUBLIC pulp::runtime)
target_link_libraries(pulp-view-script PRIVATE pulp::runtime)

# yaml-cpp (MIT) — DESIGN.md frontmatter parsing. Public Pulp headers expose no
# YAML types, but pulp-view-core is static: its parser object retains YAML
# symbols that the final consumer link must resolve. Keep yaml-cpp private in
# the API while preserving that static link closure in installed SDKs.
if(PULP_ENABLE_DESIGN_IMPORT)
    target_link_libraries(pulp-view-core PRIVATE
        $<BUILD_INTERFACE:yaml-cpp::yaml-cpp>
        $<INSTALL_INTERFACE:Pulp::yaml-cpp>
    )
endif()

# JavaScriptCore framework for JSC backend (Apple only)
if(APPLE)
    target_link_libraries(pulp-view-script PRIVATE "-framework JavaScriptCore")
endif()

if(WIN32)
    # Process spawning routes through pulp::platform::ChildProcess.
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
            # Bridge stubs compile on iOS too. They provide set_factory /
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
        # Wire iOS GPU window host (window_host_ios.mm's
        # IOSGpuWindowHost branch). Mirrors the mac branch below:
        # `pulp::render` exposes Dawn / Skia Graphite + the GpuSurface
        # interface, and the Metal framework backs the CAMetalLayer.
        # `pulp::render` is gated by PULP_HAS_SKIA so a no-Skia iOS
        # configure (CG-only) still links cleanly.
        if(PULP_HAS_SKIA)
            target_link_libraries(pulp-view-core PRIVATE pulp::render
                "-framework Metal"
            )
        endif()
    else()
        target_sources(pulp-view-core PRIVATE
            platform/mac/app_menu_mac.mm
            platform/mac/window_host_mac.mm
            platform/mac/window_host_mac_capture.mm
            platform/mac/window_host_mac_geometry.mm
            platform/mac/window_host_mac_text_input.mm
            platform/mac/window_manager_mac.mm
            platform/mac/plugin_view_host_mac.mm
            platform/mac/plugin_view_host_mac_text_input.mm
            platform/mac/screenshot_mac.mm
            platform/mac/drag_drop_mac.mm
            platform/mac/accessibility_mac.mm
            platform/mac/appearance_mac.mm
            platform/mac/motion_preferences_mac.mm
            # Host-backend registration APIs. The stub files compile
            # set_factory/set_screenshot_provider etc.
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
            # CVDisplayLink drives the continuous-frame / idle pump in BOTH the
            # CPU (CoreGraphics) and GPU plugin hosts, so CoreVideo must link
            # even when Skia is absent (the CG-only fallback build).
            "-framework CoreVideo"
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
        # Standalone outbound file-drag backend (free begin_file_drag via OLE).
        # Unconditional — DoDragDrop has no Skia/GPU dependency, so a non-GPU
        # standalone build still drags files out. ole32 is linked below.
        platform/win/drag_drop_win.cpp
    )
    # Win/Linux parity. When Skia is available, add the built-in Skia raster
    # screenshot backend and the native HWND PluginViewHost.
    # plugin_view_host_stub.cpp stays (it owns the always-compiled
    # set_factory/has_factory registration API + the factory-dispatching
    # create()); the native impl registers ITSELF as the factory via
    # register_platform_plugin_view_host(). screenshot_skia.cpp self-guards
    # on PULP_HAS_SKIA and supersedes the stub's render_* fallback (which is
    # now compiled only when !PULP_HAS_SKIA).
    if(PULP_HAS_SKIA)
        target_sources(pulp-view-core PRIVATE
            src/screenshot_skia.cpp
            platform/win/plugin_view_host_win.cpp
        )
        # The native HWND host supplies register_platform_plugin_view_host();
        # tell the stub not to compile its no-op default (one definition).
        set_source_files_properties(src/plugin_view_host_stub.cpp PROPERTIES
            COMPILE_DEFINITIONS PULP_HAS_PLATFORM_PLUGIN_VIEW_HOST=1)
    endif()
    # UIAutomationCore for UiaClientsAreListening plus
    # UiaReturnRawElementProvider + UiaRaiseStructureChangedEvent.
    target_link_libraries(pulp-view-core PRIVATE uiautomationcore)
    # OLE drag-drop on the plugin-host child HWND (WinPluginViewHost):
    # ole32 = OleInitialize/RegisterDragDrop/ReleaseStgMedium; shell32 =
    # DragQueryFileW for CF_HDROP file lists.
    target_link_libraries(pulp-view-core PRIVATE ole32 shell32)
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
    # Win/Linux parity. Same split as the WIN32 branch: when Skia is available
    # add the built-in Skia raster screenshot backend and the native X11
    # PluginViewHost (which registers itself via
    # register_platform_plugin_view_host()). The X11 host links Xlib; missing
    # X11 dev headers degrade to the headless-render-only path.
    if(PULP_HAS_SKIA)
        target_sources(pulp-view-core PRIVATE src/screenshot_skia.cpp)
        find_package(X11)
        if(X11_FOUND)
            target_sources(pulp-view-core PRIVATE
                platform/linux/plugin_view_host_linux.cpp
                # Standalone outbound file-drag backend (free begin_file_drag via
                # XDND source). Gated with PULP_HAS_X11 so the drag_drop.cpp no-op
                # drops out only when this real backend is compiled.
                platform/linux/drag_drop_linux.cpp)
            target_link_libraries(pulp-view-core PRIVATE ${X11_LIBRARIES})
            target_include_directories(pulp-view-core PRIVATE ${X11_INCLUDE_DIR})
            target_compile_definitions(pulp-view-core PRIVATE PULP_HAS_X11=1)
            # The native X11 host supplies register_platform_plugin_view_host();
            # tell the stub not to compile its no-op default (one definition).
            set_source_files_properties(src/plugin_view_host_stub.cpp PROPERTIES
                COMPILE_DEFINITIONS PULP_HAS_PLATFORM_PLUGIN_VIEW_HOST=1)
        else()
            message(STATUS "Pulp: X11 not found — Linux PluginViewHost limited to headless render")
        endif()
    endif()
    # Linux AT-SPI needs no link deps: accessibility_linux.cpp
    # speaks the AT-SPI2 wire protocol directly over pulp::platform::DBus
    # (libdbus-1 dlopen'd). No atk-bridge-2.0 / libatspi at build time.
endif()

# Link against render for native GPU bridge (Three.js, WebGPU canvas).
# visualization_bridge.hpp forward-declares PerfCounters and the dereferencing
# code lives in visualization_bridge.cpp, so the link stays PRIVATE regardless
# of PULP_BENCHMARK. Downstream targets that only use view APIs do NOT need to
# link pulp::render when PULP_BENCHMARK is enabled.
if(TARGET pulp-render)
    # plugin_frame_renderer.hpp exposes render::GpuSurface when Skia is on,
    # so consumers need the render include and link contract in that build.
    # The no-Skia build keeps render as an implementation detail.
    if(PULP_HAS_SKIA)
        target_link_libraries(pulp-view-core PUBLIC pulp::render)
    else()
        target_link_libraries(pulp-view-core PRIVATE pulp::render)
    endif()
    target_link_libraries(pulp-view-script PRIVATE pulp::render)
    # Enables the offscreen-GPU view-capture path (render_to_png_gpu / capture_view
    # via pulp::render::HeadlessSurface) in src/screenshot_gpu.cpp. Gate on Skia,
    # not merely on the pulp-render target: in a no-Skia build pulp-render still
    # exists but HeadlessSurface::create() is compiled as the stub that always
    # returns null, so the GPU path could never produce bytes. Defining the macro
    # there would make has_gpu_capture() report true and suppress the honest raster
    # fallback, ending in a no-bytes failure. Only claim GPU capture when Skia (and
    # thus a real HeadlessSurface) is present; otherwise capture degrades to raster
    # honestly (has_gpu_capture()==false).
    if(PULP_HAS_SKIA)
        target_compile_definitions(pulp-view-core PRIVATE PULP_VIEW_HAS_GPU_CAPTURE=1)
    endif()
    # When pulp-render is linked, WindowHost::mark_dirty() can
    # route dirty marks through a vblank-paced RenderLoop. GPU-off builds
    # (sanitizer matrix) have no pulp-render target, so mark_dirty() there
    # degrades to a direct repaint(); the define gates that cleanly.
    target_compile_definitions(pulp-view-core PRIVATE PULP_VIEW_HAS_RENDER_LOOP=1)
endif()
