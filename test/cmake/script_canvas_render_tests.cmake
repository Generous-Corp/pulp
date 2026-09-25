# Scripted UI, JS, screenshot, Canvas, SDF, View, and render-helper tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Grouped executables for this manifest (pulp_add_test_group in
# tools/cmake/PulpTestSuite.cmake): each member keeps its own registration,
# labels and properties; only the binary behind them is shared. Members are
# grouped by the compile line they already had, so four groups cover the
# pulp::view suites, the pulp::canvas suites, the header-only render helpers
# and the pulp::format suites. A suite stays on its own when it needs its own
# process or compile line: the RT allocation probe (css-gradient-render), a
# library .cpp compiled into the test (render-loop, sdl3-surface,
# texture-atlas), the scripted-ui suite (pulp::view and pulp::format together
# is a compile line no other suite here has), edit-history (pulp::state alone),
# and the non-Apple PluginViewHost factory.
set(_pulp_script_view_group_libs pulp::view pulp::canvas pulp::state)
if(TARGET pulp-render)
    list(APPEND _pulp_script_view_group_libs pulp::render)
endif()
pulp_add_test_group(pulp-test-group-script-view LIBRARIES ${_pulp_script_view_group_libs})

# Widget animation behavior tests
pulp_add_test_suite(pulp-test-widget-animation GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# Auto UI tests
pulp_add_test_suite(pulp-test-auto-ui GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# One executable for the pulp::format suites: HostQuirks, HostVersion and the
# ViewBridge lifecycle.
pulp_add_test_group(pulp-test-group-script-format LIBRARIES pulp::format)

# ViewBridge lifecycle tests. PULP_REPO_ROOT lets the hosted-adapter structural
# guard read the format adapter sources and assert each one builds its editor
# from ViewBridge::Options::hosted_editor().
pulp_add_test_suite(pulp-test-view-bridge GROUP pulp-test-group-script-format
    LIBRARIES pulp::format
    COMPILE_DEFINITIONS PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
pulp_add_test_suite(pulp-test-view-bridge GROUP pulp-test-group-script-format
    LIBRARIES pulp::format
    TEST_SPEC "[lifecycle]"
    TEST_PREFIX "lifecycle::"
    LABELS lifecycle)

# Script engine tests (legacy API)
pulp_add_test_suite(pulp-test-script GROUP pulp-test-group-script-view
    SOURCES test_script_engine.cpp
    LIBRARIES pulp::view)

# Scripted UI hot reload/theme reload tests
add_executable(pulp-test-scripted-ui
    test_scripted_ui.cpp)
target_link_libraries(pulp-test-scripted-ui PRIVATE pulp::view pulp::format Catch2::Catch2WithMain)
if(TARGET pulp-render)
    target_link_libraries(pulp-test-scripted-ui PRIVATE pulp::render)
endif()
# `slow`: ScriptedUiSession reload tests sleep on file-watcher
# debounce + filesystem mtime; ~0.5-1 sec each. Excluded from fast-CI.
catch_discover_tests(pulp-test-scripted-ui PROPERTIES LABELS slow)

# Runtime-evaluation tests do not use the file watcher or its debounce sleeps.
# Keep them in the normal-speed coverage lane so the capability, timeout, realm
# replacement, and teardown paths are exercised by the diff-coverage gate.
pulp_add_test_suite(pulp-test-scripted-ui-runtime-eval GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# JS engine abstraction tests (shared across all backends)
pulp_add_test_suite(pulp-test-js-engine GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# Cooperative-interrupt (runaway-abort) tests for the QuickJS backend
pulp_add_test_suite(pulp-test-js-engine-interrupt GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# Scripted-UI runtime inspector bridge (off-thread evaluate marshaling)
pulp_add_test_suite(pulp-test-script-inspector-bridge GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# Theme/design token tests
pulp_add_test_suite(pulp-test-theme GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# Three.js IIFE resources loader contract test. Runs on all platforms;
# iOS-only behavior is gated inside the test.
pulp_add_test_suite(pulp-test-threejs-resources GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

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
        # The script provisions esbuild in a temporary package before running
        # five bundle cases, so cold or contended CI hosts need more than 30s.
        TIMEOUT 120
        LABELS "ios-d3b;node;threejs")
endif()

# Screenshot tests. macOS uses the CoreGraphics bitmap context; non-Apple
# builds with Skia use the built-in cross-platform Skia raster backend
# for Windows/Linux parity, so the test runs there too and proves the
# headless render_to_png/rgba path the foreign-host embed depends on. A
# non-Apple build WITHOUT Skia has no backend, so skip there.
if(APPLE OR PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-screenshot GROUP pulp-test-group-script-view
        LIBRARIES pulp::view)
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
    pulp_add_test_suite(pulp-test-screenshot-gpu GROUP pulp-test-group-script-view
        LIBRARIES pulp::view)

    pulp_add_test_suite(pulp-test-screenshot-cli-contracts GROUP pulp-test-group-script-view
        LIBRARIES pulp::view pulp::state)
endif()

# One executable for the pulp::canvas suites. pulp::canvas exports
# PULP_HAS_SKIA=1 and the Skia include path when Skia is available; the group
# states them once more so the #ifdef PULP_HAS_SKIA cases of every member
# register whatever order the targets were configured in. The CoreGraphics /
# ImageIO / CoreServices frameworks back the CoreGraphicsCanvas cases on Apple.
pulp_add_test_group(pulp-test-group-canvas LIBRARIES pulp::canvas)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-group-canvas PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-group-canvas PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
if(APPLE)
    target_link_libraries(pulp-test-group-canvas PRIVATE
        "-framework CoreGraphics" "-framework ImageIO" "-framework CoreServices")
endif()

# Canvas tests
pulp_add_test_suite(pulp-test-canvas-color GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# Outset box-shadow coverage cache (Skia path). Test body is gated on
# PULP_HAS_SKIA, which pulp::canvas propagates; without Skia it has zero cases.
pulp_add_test_suite(pulp-test-box-shadow-cache GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# CoreGraphicsCanvas tests build a CGBitmapContext directly to verify
# CGContextClearRect actually clears destination pixels; ImageIO provides
# CGImageDestinationCreateWithURL for PNG encoding of a pattern fixture and
# CoreServices the public.png UTI (linked on the group above).
# Public font-registration API tests use a bundled .ttf as a
# fixture. external/fonts/Inter-Regular.ttf is the canonical "always
# available" font in the repo (also baked into pulp-canvas via
# pulp_add_binary_data). Pass the absolute path through a compile def so
# the test binary can find it regardless of CWD when run via ctest.
pulp_add_test_suite(pulp-test-canvas GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas
    COMPILE_DEFINITIONS
        "PULP_TEST_FONT_PATH=\"${CMAKE_SOURCE_DIR}/external/fonts/Inter-Regular.ttf\"")

# Canvas CG-paths tests. Apple-only TU covering concat_transform,
# Canvas2D path API fill/curves/stroke/gradients, fill_rect /
# fill_path with active gradient, save/restore tracking, and
# BlendMode every-value round-trip. Companion to test_canvas_cg_gradients.cpp;
# the non-Apple parts stay in test_canvas.cpp.
pulp_add_test_suite(pulp-test-canvas-cg-paths GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# Canvas font tests. Covers bundled-font registration via
# match_bundled_typeface plus the public register_font(path) API.
# Shares the PULP_TEST_FONT_PATH compile def so test fixtures find
# the bundled Inter-Regular.ttf regardless of CWD.
pulp_add_test_suite(pulp-test-canvas-fonts GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas
    COMPILE_DEFINITIONS
        "PULP_TEST_FONT_PATH=\"${CMAKE_SOURCE_DIR}/external/fonts/Inter-Regular.ttf\""
        # Deterministic variable font (wght axis 300-800) for the variable-font
        # weight-instancing regression tests. Test-only; not in bundled_blobs().
        "PULP_TEST_VARIABLE_FONT_PATH=\"${CMAKE_SOURCE_DIR}/external/fonts/FunnelDisplay-VariableFont_wght.ttf\"")

# CSS gradient strings judged by the pixels they produce. Every defect this
# covers was a value the parser ACCEPTED and painted wrong, so a string
# assertion cannot see any of them — each case renders the CSS beside a
# hand-written equivalent and requires the two images to agree.
if(PULP_HAS_SKIA)
    add_executable(pulp-test-css-gradient-render
        test_css_gradient_render.cpp
        harness/rt_allocation_probe.cpp)
    target_link_libraries(pulp-test-css-gradient-render
        PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-css-gradient-render)
endif()

# CSS gradient geometry against Chrome's own render of the same string. Split
# from the file above because its oracle is different in kind: those cases
# assert that two CSS spellings agree with EACH OTHER, which cannot see an
# arithmetic error both spellings share. These expectations are pixel
# positions read off Chromium by tools/import-validation/chrome_gradient_oracle.py.
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-css-gradient-geometry GROUP pulp-test-group-script-view
        LIBRARIES pulp::view)
endif()

# CG-degraded gradient + pattern cluster. Apple-only TU, registered only on
# Apple: a grouped member whose file compiles to no test cases fails discovery. every
# TEST_CASE drives CoreGraphics directly to prove the CoreGraphicsCanvas
# fallback honours the canvas2d spec — conic / two-circle
# radial / pattern + gradient-anchored fill/stroke text + line-dash CG
# paths.
if(APPLE)
    pulp_add_test_suite(pulp-test-canvas-cg-gradients GROUP pulp-test-group-canvas
        LIBRARIES pulp::canvas)
endif()

# CoreGraphicsCanvas image-draw verbs. Apple-only TU: encodes a known PNG,
# draws it back through the CG decoder + CGContextDrawImage, reads the
# pixels, and proves the actual image (color + orientation) lands instead of
# a filename-placeholder fallback.
if(APPLE)
    pulp_add_test_suite(pulp-test-canvas-cg-image GROUP pulp-test-group-canvas
        LIBRARIES pulp::canvas)
endif()

# ImageFileCache path-keyed decoded/GPU-uploaded image cache.
# Skia-gated (value type is sk_sp<SkImage>); asserts hit/miss, backend-token
# partitioning, clear() invalidation, disabled passthrough, and LRU eviction.
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-image-file-cache GROUP pulp-test-group-canvas
        LIBRARIES pulp::canvas)
endif()

# CSS filter chain coverage: Skia-only pixel readback (contrast /
# invert / opacity ordering with drop-shadow, set_filter drop-shadow
# parser) plus portable filter-chain matrix math (contrast bias,
# invert maps, identity, opacity alpha scaling).
pulp_add_test_suite(pulp-test-canvas-filter-chain GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# Canvas::supports(CanvasCapability) — the honest per-backend capability map
# (WI-28). Drives Skia (full faithful set), CoreGraphics (images only, macOS),
# and RecordingCanvas (records intent, advertises nothing) plus a real
# masked-View paint that proves the verb still lands when the capability is
# false. Links pulp::view for the paint_all mask-branch case.
pulp_add_test_suite(pulp-test-canvas-capabilities GROUP pulp-test-group-script-view
    LIBRARIES pulp::canvas pulp::view)

# Subtree scene cache — View::set_subtree_cached + Canvas::record_scene /
# draw_scene (FU-3, WI-20). Raster-Skia pixel readback proves cached == uncached
# and that invalidation (child mutation / add_child / set_bounds) re-records,
# while a spied child paint() proves a cache HIT skips the subtree walk. A
# RecordingCanvas case proves the honest direct-paint fallback on a
# non-scene_cache backend, and a ScopedAllocAllowed case pins the record's
# no-alloc exemption. Links pulp::view for paint_all + the cache wiring.
pulp_add_test_suite(pulp-test-subtree-cache GROUP pulp-test-group-script-view
    LIBRARIES pulp::canvas pulp::view)

# Partial-repaint equivalence (FU-2, WI-17). Pure compute_effective_damage unit
# tests (no Skia) PLUS a raster screenshot-equivalence harness that proves a
# clipped repaint of a mutation is byte-identical to a full repaint, including
# hazard cases (blurred / backdrop-filter sibling) with "naive clip WOULD
# differ" negative proofs. Links pulp::view (producer + damage model) + Skia.
pulp_add_test_suite(pulp-test-partial-repaint-equivalence GROUP pulp-test-group-script-view
    LIBRARIES pulp::canvas pulp::view)

# Widgets must request a BOUNDED repaint on their hot value path. The rect-less
# View::request_repaint() dirties the WHOLE surface by design, so a knob drag
# would re-composite a plug-in's static chrome per mouse move and partial
# repaint could never engage regardless of host wiring
# (test/test_widget_bounded_repaint.cpp).
pulp_add_test_suite(pulp-test-widget-bounded-repaint GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# Canvas arcs / path-primitive Skia rasterization fixtures. Native
# arc / arcTo / ellipse / roundRect cluster: RecordingCanvas
# captures of native primitives + Skia rasterization fixtures (full /
# half circle arc matches reference SkPath::arcTo, arc_to collinear
# lineTos, round_rect 4-corner radii, ellipse rotation contour
# continuity).
pulp_add_test_suite(pulp-test-canvas-arcs GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# Canvas::draw_svg — faithful SVG rendering via SkSVGDOM (gradients/filters).
pulp_add_test_suite(pulp-test-canvas-svg GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# Emoji-parity tests — cover the segmenter + cluster-aware shaping +
# letter-spacing + cache invalidation paths. Also writes PNGs to
# `$PULP_EMOJI_TEST_PNG_DIR` (default /tmp/pulp-emoji-validation/) for
# visual inspection of real-emoji rendering through Skia.
pulp_add_test_suite(pulp-test-canvas-emoji GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# SDF glyph atlas exploration
pulp_add_test_suite(pulp-test-sdf-atlas GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# MSDF glyph atlas
pulp_add_test_suite(pulp-test-msdf-atlas GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# PSDF glyph atlas + vector fallback
pulp_add_test_suite(pulp-test-psdf-atlas GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# Shared SDF/MSDF text helpers (pen snap + build_text_quads + fill_text_*)
pulp_add_test_suite(pulp-test-sdf-text GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# SDF effects layer — host-side API + presets
pulp_add_test_suite(pulp-test-sdf-effects GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# Runtime SDF atlas cache — LRU + frame-based eviction
pulp_add_test_suite(pulp-test-sdf-atlas-cache GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# Software SDF renderer (end-to-end headless raster)
pulp_add_test_suite(pulp-test-sdf-software-renderer GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# Runtime path → SDF conversion — on-device SDF generation
pulp_add_test_suite(pulp-test-path-to-sdf GROUP pulp-test-group-canvas
    LIBRARIES pulp::canvas)

# View and layout tests
pulp_add_test_suite(pulp-test-view GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# View corner-radius tests extracted from test_view.cpp.
# Covers Panel + View percent-radius math, per-corner radius
# resolution, reflow-on-bounds-change, view border stroke + outline +
# box-shadow honoring effective_corner_radius.
pulp_add_test_suite(pulp-test-view-corner-radius GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# View z-index + overflow tests extracted from test_view.cpp. Covers
# z-index paint-order + hit-test, overflow:visible default for
# absolute-positioned popovers, and symmetric overflow:visible hit-test
# extension.
pulp_add_test_suite(pulp-test-view-zindex-overflow GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# Scrollable overflow (`overflow: scroll` / CSS `auto`). Covers the scrollable
# range, wheel input, clamping at both ends, the clip, and — the load-bearing
# one — that a row owns its own PAINTED centre at a non-zero scroll offset, so
# paint and hit-testing cannot drift apart.
pulp_add_test_suite(pulp-test-overflow-scroll GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# View CSS mask + overflow:visible hit-test extension. Covers the
# symmetric overflow:visible hit-test extension
# (500px in each direction so absolutely-positioned popovers protrude
# past their parent) plus CSS mask-image + mask-size
# paint routing (save_layer_with_mask routing).
pulp_add_test_suite(pulp-test-view-mask-overflow GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# One executable for the header-only render helpers (DirtyTracker, the
# partial-rendering contract and the GPU render-time budget). They link no
# Pulp library, so they stay available in GPU-off sanitizer and coverage
# builds.
pulp_add_test_group(pulp-test-group-render-helpers
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/core/render/include)

# DirtyTracker and RenderLoop timer-state tests are pure helpers;
# keep them available in GPU-off sanitizer and coverage builds.
pulp_add_test_suite(pulp-test-dirty-tracker GROUP pulp-test-group-render-helpers)

# Partial-rendering POC test. Pins the DirtyTracker contract the
# macOS GPU host's tracker wiring relies on.
# The host integration itself needs Skia + a Metal device for end-to-end
# coverage, so that lives in a separate partial-rendering test.
pulp_add_test_suite(pulp-test-partial-rendering-poc GROUP pulp-test-group-render-helpers)

pulp_add_test_suite(pulp-test-gpu-render-time GROUP pulp-test-group-render-helpers
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
    pulp_add_test_suite(pulp-test-rendering-integration GROUP pulp-test-group-script-view
        LIBRARIES pulp::view pulp::render pulp::state)

    # WindowHost::mark_dirty() VBlank-locked safe-repaint routing.
    # Needs both pulp::view (mark_dirty / attach_render_loop) and
    # pulp::render (RenderLoop factory), so it gates on PULP_ENABLE_GPU.
    pulp_add_test_suite(pulp-test-window-host-repaint GROUP pulp-test-group-script-view
        LIBRARIES pulp::view pulp::render)

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
pulp_add_test_suite(pulp-test-sprite-strip GROUP pulp-test-group-script-view
    LIBRARIES pulp::view)

# Edit history (global undo) tests
pulp_add_test_suite(pulp-test-edit-history LIBRARIES pulp::state)

# HostQuirks scaffolding + HostVersion detection
pulp_add_test_suite(pulp-test-host-version GROUP pulp-test-group-script-format
    LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-host-quirks GROUP pulp-test-group-script-format
    LIBRARIES pulp::format)

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
