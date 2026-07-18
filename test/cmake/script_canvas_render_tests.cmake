# Scripted UI, JS, screenshot, Canvas, SDF, View, and render-helper tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Widget animation behavior tests
pulp_add_test_suite(pulp-test-widget-animation LIBRARIES pulp::view)

# Auto UI tests
pulp_add_test_suite(pulp-test-auto-ui LIBRARIES pulp::view)

# ViewBridge lifecycle tests
pulp_add_test_suite(pulp-test-view-bridge LIBRARIES pulp::format)
catch_discover_tests(pulp-test-view-bridge
    TEST_SPEC "[lifecycle]"
    TEST_PREFIX "lifecycle::"
    PROPERTIES LABELS lifecycle)

# Remote View Protocol loopback tests.
pulp_add_test_suite(pulp-test-remote-view LIBRARIES pulp::format pulp::runtime)

# Script engine tests (legacy API)
pulp_add_test_suite(pulp-test-script SOURCES test_script_engine.cpp LIBRARIES pulp::view)

# Scripted UI hot reload/theme reload tests
add_executable(pulp-test-scripted-ui test_scripted_ui.cpp)
target_link_libraries(pulp-test-scripted-ui PRIVATE pulp::view Catch2::Catch2WithMain)
if(TARGET pulp-render)
    target_link_libraries(pulp-test-scripted-ui PRIVATE pulp::render)
endif()
# `slow`: ScriptedUiSession reload tests sleep on file-watcher
# debounce + filesystem mtime; ~0.5-1 sec each. Excluded from fast-CI.
catch_discover_tests(pulp-test-scripted-ui PROPERTIES LABELS slow)

# JS engine abstraction tests (shared across all backends)
pulp_add_test_suite(pulp-test-js-engine LIBRARIES pulp::view)

# Cooperative-interrupt (runaway-abort) tests for the QuickJS backend
pulp_add_test_suite(pulp-test-js-engine-interrupt LIBRARIES pulp::view)

# Scripted-UI runtime inspector bridge (off-thread evaluate marshaling)
pulp_add_test_suite(pulp-test-script-inspector-bridge LIBRARIES pulp::view)

# Theme/design token tests
pulp_add_test_suite(pulp-test-theme LIBRARIES pulp::view)

# Three.js IIFE resources loader contract test. Runs on all platforms;
# iOS-only behavior is gated inside the test.
add_executable(pulp-test-threejs-resources test_threejs_resources.cpp)
target_link_libraries(pulp-test-threejs-resources PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-threejs-resources)

# Node-based smoke test for the IIFE bundler. Only registered when
# Node.js is on PATH — desktop CI hosts have it,
# fresh / offline builds may not (parallels the optional bundle step
# in PulpAuv3.cmake).
find_program(_PULP_NODE_FOR_TESTS NAMES node nodejs)
if(_PULP_NODE_FOR_TESTS)
    add_test(NAME pulp_bundle_threejs_for_jsc_smoke
             COMMAND ${_PULP_NODE_FOR_TESTS}
                     ${CMAKE_SOURCE_DIR}/tools/scripts/test_bundle_threejs_for_jsc.mjs)
    set_tests_properties(pulp_bundle_threejs_for_jsc_smoke PROPERTIES
        TIMEOUT 30
        LABELS "ios-d3b;node;threejs")
endif()

# Screenshot tests. macOS uses the CoreGraphics bitmap context; non-Apple
# builds with Skia use the built-in cross-platform Skia raster backend
# for Windows/Linux parity, so the test runs there too and proves the
# headless render_to_png/rgba path the foreign-host embed depends on. A
# non-Apple build WITHOUT Skia has no backend, so skip there.
if(APPLE OR PULP_HAS_SKIA)
    add_executable(pulp-test-screenshot test_screenshot.cpp)
    target_link_libraries(pulp-test-screenshot PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot)
endif()

# Native PluginViewHost factory + headless-capture proof on non-Apple.
# Built where core/view compiled a platform host: Windows+Skia or non-Android
# UNIX+Skia+X11. Degrades to capture-only with no display server (headless VM);
# capture_back_buffer_png() still yields a frame. On Linux+X11 the same binary
# also runs the XDND synthetic-source handshake under xvfb: it links
# Xlib directly to act as an XDND source against the host's child window, gated
# by PULP_TEST_HAS_X11 so the handshake TEST_CASE is compiled only there.
if(PULP_HAS_SKIA AND NOT APPLE AND NOT ANDROID)
    set(_pulp_has_platform_pvh OFF)
    set(_pulp_pvh_x11 OFF)
    if(WIN32)
        set(_pulp_has_platform_pvh ON)
    elseif(UNIX)
        find_package(X11 QUIET)
        if(X11_FOUND)
            set(_pulp_has_platform_pvh ON)
            set(_pulp_pvh_x11 ON)
        endif()
    endif()
    if(_pulp_has_platform_pvh)
        add_executable(pulp-test-plugin-view-host-factory test_plugin_view_host_factory.cpp)
        target_link_libraries(pulp-test-plugin-view-host-factory PRIVATE pulp::view Catch2::Catch2WithMain)
        if(_pulp_pvh_x11)
            target_link_libraries(pulp-test-plugin-view-host-factory PRIVATE ${X11_LIBRARIES})
            target_include_directories(pulp-test-plugin-view-host-factory PRIVATE ${X11_INCLUDE_DIR})
            target_compile_definitions(pulp-test-plugin-view-host-factory PRIVATE PULP_TEST_HAS_X11=1)
        endif()
        catch_discover_tests(pulp-test-plugin-view-host-factory)
    endif()
endif()

if(APPLE)
    # Smart capture (capture_view) + offscreen-GPU path (render_to_png_gpu).
    add_executable(pulp-test-screenshot-gpu test_screenshot_gpu.cpp)
    target_link_libraries(pulp-test-screenshot-gpu PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot-gpu)

    add_executable(pulp-test-screenshot-cli-contracts test_screenshot_cli_contracts.cpp)
    target_link_libraries(pulp-test-screenshot-cli-contracts PRIVATE
        pulp::view
        pulp::state
        Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot-cli-contracts)
endif()

# Canvas tests
pulp_add_test_suite(pulp-test-canvas-color LIBRARIES pulp::canvas)

# Outset box-shadow coverage cache (Skia path). Test body is gated on
# PULP_HAS_SKIA, which pulp::canvas propagates; without Skia it has zero cases.
pulp_add_test_suite(pulp-test-box-shadow-cache LIBRARIES pulp::canvas)

add_executable(pulp-test-canvas test_canvas.cpp)
target_link_libraries(pulp-test-canvas PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(APPLE)
    # CoreGraphicsCanvas tests build a CGBitmapContext directly to
    # verify CGContextClearRect actually clears destination pixels.
    # ImageIO provides CGImageDestinationCreateWithURL for PNG encoding
    # of a pattern fixture; CoreServices provides the public.png UTI.
    target_link_libraries(pulp-test-canvas PRIVATE "-framework CoreGraphics" "-framework ImageIO" "-framework CoreServices")
endif()
# Public font-registration API tests use a bundled .ttf as a
# fixture. external/fonts/Inter-Regular.ttf is the canonical "always
# available" font in the repo (also baked into pulp-canvas via
# pulp_add_binary_data). Pass the absolute path through a compile def so
# the test binary can find it regardless of CWD when run via ctest.
target_compile_definitions(pulp-test-canvas PRIVATE
    "PULP_TEST_FONT_PATH=\"${CMAKE_SOURCE_DIR}/external/fonts/Inter-Regular.ttf\"")
catch_discover_tests(pulp-test-canvas)

# Canvas CG-paths tests. Apple-only TU covering concat_transform,
# Canvas2D path API fill/curves/stroke/gradients, fill_rect /
# fill_path with active gradient, save/restore tracking, and
# BlendMode every-value round-trip. Companion to test_canvas_cg_gradients.cpp;
# the non-Apple parts stay in test_canvas.cpp.
add_executable(pulp-test-canvas-cg-paths test_canvas_cg_paths.cpp)
target_link_libraries(pulp-test-canvas-cg-paths PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(APPLE)
    target_link_libraries(pulp-test-canvas-cg-paths PRIVATE "-framework CoreGraphics" "-framework ImageIO" "-framework CoreServices")
endif()
catch_discover_tests(pulp-test-canvas-cg-paths)

# Canvas font tests. Covers bundled-font registration via
# match_bundled_typeface plus the public register_font(path) API.
# Shares the PULP_TEST_FONT_PATH compile def so test fixtures find
# the bundled Inter-Regular.ttf regardless of CWD.
add_executable(pulp-test-canvas-fonts test_canvas_fonts.cpp)
target_link_libraries(pulp-test-canvas-fonts PRIVATE pulp::canvas Catch2::Catch2WithMain)
# pulp::canvas exports PULP_HAS_SKIA=1 as a PUBLIC compile def when Skia
# is available, but CMake's optimization skips re-propagating PUBLIC defs
# to sibling targets that don't trigger a rebuild. Add the define + Skia
# include path explicitly so the #ifdef PULP_HAS_SKIA blocks register
# their TEST_CASEs.
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-canvas-fonts PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-canvas-fonts PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
target_compile_definitions(pulp-test-canvas-fonts PRIVATE
    "PULP_TEST_FONT_PATH=\"${CMAKE_SOURCE_DIR}/external/fonts/Inter-Regular.ttf\""
    # Deterministic variable font (wght axis 300-800) for the variable-font
    # weight-instancing regression tests. Test-only; not in bundled_blobs().
    "PULP_TEST_VARIABLE_FONT_PATH=\"${CMAKE_SOURCE_DIR}/external/fonts/FunnelDisplay-VariableFont_wght.ttf\"")
catch_discover_tests(pulp-test-canvas-fonts)

# CG-degraded gradient + pattern cluster. Apple-only TU: every
# TEST_CASE drives CoreGraphics directly to prove the CoreGraphicsCanvas
# fallback honours the canvas2d spec — conic / two-circle
# radial / pattern + gradient-anchored fill/stroke text + line-dash CG
# paths.
add_executable(pulp-test-canvas-cg-gradients test_canvas_cg_gradients.cpp)
target_link_libraries(pulp-test-canvas-cg-gradients PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-canvas-cg-gradients PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-canvas-cg-gradients PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
catch_discover_tests(pulp-test-canvas-cg-gradients)

# CSS filter chain coverage: Skia-only pixel readback (contrast /
# invert / opacity ordering with drop-shadow, set_filter drop-shadow
# parser) plus portable filter-chain matrix math (contrast bias,
# invert maps, identity, opacity alpha scaling).
add_executable(pulp-test-canvas-filter-chain test_canvas_filter_chain.cpp)
target_link_libraries(pulp-test-canvas-filter-chain PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-canvas-filter-chain PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-canvas-filter-chain PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
catch_discover_tests(pulp-test-canvas-filter-chain)

# Canvas arcs / path-primitive Skia rasterization fixtures. Native
# arc / arcTo / ellipse / roundRect cluster: RecordingCanvas
# captures of native primitives + Skia rasterization fixtures (full /
# half circle arc matches reference SkPath::arcTo, arc_to collinear
# lineTos, round_rect 4-corner radii, ellipse rotation contour
# continuity).
add_executable(pulp-test-canvas-arcs test_canvas_arcs.cpp)
target_link_libraries(pulp-test-canvas-arcs PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-canvas-arcs PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-canvas-arcs PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
catch_discover_tests(pulp-test-canvas-arcs)

# Canvas::draw_svg — faithful SVG rendering via SkSVGDOM (gradients/filters).
add_executable(pulp-test-canvas-svg test_canvas_svg.cpp)
target_link_libraries(pulp-test-canvas-svg PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-canvas-svg PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-canvas-svg PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
catch_discover_tests(pulp-test-canvas-svg)

# Emoji-parity tests — cover the segmenter + cluster-aware shaping +
# letter-spacing + cache invalidation paths. Also writes PNGs to
# `$PULP_EMOJI_TEST_PNG_DIR` (default /tmp/pulp-emoji-validation/) for
# visual inspection of real-emoji rendering through Skia.
add_executable(pulp-test-canvas-emoji test_canvas_emoji.cpp)
target_link_libraries(pulp-test-canvas-emoji PRIVATE
    pulp::canvas
    Catch2::Catch2WithMain
)
catch_discover_tests(pulp-test-canvas-emoji)

# SDF glyph atlas exploration
pulp_add_test_suite(pulp-test-sdf-atlas LIBRARIES pulp::canvas)

# MSDF glyph atlas
pulp_add_test_suite(pulp-test-msdf-atlas LIBRARIES pulp::canvas)

# PSDF glyph atlas + vector fallback
pulp_add_test_suite(pulp-test-psdf-atlas LIBRARIES pulp::canvas)

# Shared SDF/MSDF text helpers (pen snap + build_text_quads + fill_text_*)
pulp_add_test_suite(pulp-test-sdf-text LIBRARIES pulp::canvas)

# SDF effects layer — host-side API + presets
pulp_add_test_suite(pulp-test-sdf-effects LIBRARIES pulp::canvas)

# Runtime SDF atlas cache — LRU + frame-based eviction
pulp_add_test_suite(pulp-test-sdf-atlas-cache LIBRARIES pulp::canvas)

# Software SDF renderer (end-to-end headless raster)
pulp_add_test_suite(pulp-test-sdf-software-renderer LIBRARIES pulp::canvas)

# Runtime path → SDF conversion — on-device SDF generation
pulp_add_test_suite(pulp-test-path-to-sdf LIBRARIES pulp::canvas)

# View and layout tests
pulp_add_test_suite(pulp-test-view LIBRARIES pulp::view)

# View corner-radius tests extracted from test_view.cpp.
# Covers Panel + View percent-radius math, per-corner radius
# resolution, reflow-on-bounds-change, view border stroke + outline +
# box-shadow honoring effective_corner_radius.
pulp_add_test_suite(pulp-test-view-corner-radius LIBRARIES pulp::view)

# View z-index + overflow tests extracted from test_view.cpp. Covers
# z-index paint-order + hit-test, overflow:visible default for
# absolute-positioned popovers, and symmetric overflow:visible hit-test
# extension.
pulp_add_test_suite(pulp-test-view-zindex-overflow LIBRARIES pulp::view)

# View CSS mask + overflow:visible hit-test extension. Covers the
# symmetric overflow:visible hit-test extension
# (500px in each direction so absolutely-positioned popovers protrude
# past their parent) plus CSS mask-image + mask-size
# paint routing (save_layer_with_mask routing).
pulp_add_test_suite(pulp-test-view-mask-overflow LIBRARIES pulp::view)

# DirtyTracker, GpuGraphRenderer, and RenderLoop timer-state tests
# are pure helpers; keep them available in GPU-off sanitizer and coverage builds.
add_executable(pulp-test-dirty-tracker test_dirty_tracker.cpp)
target_include_directories(pulp-test-dirty-tracker PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-dirty-tracker PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-dirty-tracker)

# Partial-rendering POC test. Pins the DirtyTracker contract the
# macOS GPU host's tracker wiring relies on.
# The host integration itself needs Skia + a Metal device for end-to-end
# coverage, so that lives in a separate partial-rendering test.
add_executable(pulp-test-partial-rendering-poc test_partial_rendering_poc.cpp)
target_include_directories(pulp-test-partial-rendering-poc PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-partial-rendering-poc PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-partial-rendering-poc)

add_executable(pulp-test-gpu-graph test_gpu_graph.cpp)
target_include_directories(pulp-test-gpu-graph PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-gpu-graph PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-gpu-graph)

# Dawn GPU timestamp queries. The pure tick->ms resolution
# math plus the CPU-only GpuTimestamps stub compile without a GPU link,
# so this builds the gpu_timestamps.cpp translation unit directly (it
# falls back to the no-op stub when PULP_HAS_SKIA is undefined). Live-
# device QuerySet resolution is covered by CI's GPU matrix.
add_executable(pulp-test-gpu-timestamps
    test_gpu_timestamps.cpp
    ${CMAKE_SOURCE_DIR}/core/render/src/gpu_timestamps.cpp)
target_include_directories(pulp-test-gpu-timestamps PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-gpu-timestamps PRIVATE
    pulp::runtime
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-gpu-timestamps
    PROPERTIES
        LABELS coverage)

add_executable(pulp-test-gpu-render-time
    test_gpu_render_time.cpp)
target_include_directories(pulp-test-gpu-render-time PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-gpu-render-time PRIVATE
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-gpu-render-time
    PROPERTIES
        LABELS coverage)

add_executable(pulp-test-render-loop
    test_render_loop.cpp
    ${CMAKE_SOURCE_DIR}/core/render/src/render_loop.cpp)
target_compile_definitions(pulp-test-render-loop PRIVATE
    PULP_RENDER_LOOP_FORCE_TIMER=1)
target_include_directories(pulp-test-render-loop PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include
    ${CMAKE_SOURCE_DIR}/core/render/src)
target_link_libraries(pulp-test-render-loop PRIVATE
    pulp::runtime
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-render-loop
    PROPERTIES
        LABELS coverage)

add_executable(pulp-test-sdl3-surface
    test_sdl3_surface.cpp
    ${CMAKE_SOURCE_DIR}/core/render/src/sdl3_surface.cpp)
target_include_directories(pulp-test-sdl3-surface PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-sdl3-surface PRIVATE
    pulp::runtime
    Catch2::Catch2WithMain)
if(PULP_HAS_SDL3)
    target_link_libraries(pulp-test-sdl3-surface PRIVATE
        $<BUILD_INTERFACE:SDL3::SDL3-static>)
    target_compile_definitions(pulp-test-sdl3-surface PRIVATE PULP_HAS_SDL3=1)
endif()
catch_discover_tests(pulp-test-sdl3-surface
    PROPERTIES
        LABELS coverage)

# GPU-stack tests — pulp::render only exists when PULP_ENABLE_GPU=ON.
# The sanitizer matrix builds with GPU off; without the guard configure
# fails with "Target ... links to: pulp::render but the target was not
# found". The earlier GPU-test block (test_gpu_surface, test_skia_surface,
# test_gpu_compute) already gates on the same condition.
if(PULP_ENABLE_GPU)
    # Rendering integration tests (effects, dimensions, text direction, render passes)
    add_executable(pulp-test-rendering-integration test_rendering_integration.cpp)
    target_link_libraries(pulp-test-rendering-integration PRIVATE pulp::view pulp::render pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-rendering-integration)

    # WindowHost::mark_dirty() VBlank-locked safe-repaint routing.
    # Needs both pulp::view (mark_dirty / attach_render_loop) and
    # pulp::render (RenderLoop factory), so it gates on PULP_ENABLE_GPU.
    add_executable(pulp-test-window-host-repaint test_window_host_repaint.cpp)
    target_link_libraries(pulp-test-window-host-repaint PRIVATE
        pulp::view pulp::render Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-window-host-repaint)

endif()

# Texture atlas helpers compile cleanly without GPU deps; keep this pure
# test available in GPU-off builds so sanitizer and local coverage lanes do
# not fetch GPU assets. The atlas .cpp impl is compiled directly into the
# test target rather than linking pulp::render (which would pull in Skia/Dawn).
add_executable(pulp-test-texture-atlas
    test_texture_atlas.cpp
    ${CMAKE_SOURCE_DIR}/core/render/src/texture_atlas.cpp)
target_include_directories(pulp-test-texture-atlas PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-texture-atlas PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-texture-atlas)

# Sprite strip tests
pulp_add_test_suite(pulp-test-sprite-strip LIBRARIES pulp::view)

# Edit history (global undo) tests
pulp_add_test_suite(pulp-test-edit-history LIBRARIES pulp::state)

# HostQuirks scaffolding + HostVersion detection
pulp_add_test_suite(pulp-test-host-version LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-host-quirks LIBRARIES pulp::format)

# Host-quirks catalog parity: the
# machine-readable core/format/host-quirks.json must list EXACTLY the
# flags + tiers in the C++ HostQuirksMeta. Pure-Python parser, no C++
# build needed, so it runs cheaply in every lane and guards against the
# JSON and the struct drifting apart.
if(Python3_Interpreter_FOUND)
    add_test(NAME host-quirks-catalog-parity
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/scripts/test_host_quirks_catalog_parity.py)
    set_tests_properties(host-quirks-catalog-parity PROPERTIES
        LABELS "format;host-quirks;catalog"
        TIMEOUT 30)
endif()
