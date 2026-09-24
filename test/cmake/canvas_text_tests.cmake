# Canvas text, font, shaping, and font-rendering tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# One executable for this manifest's pulp::canvas suites (pulp_add_test_group
# in tools/cmake/PulpTestSuite.cmake): each member keeps its own registration
# and properties; only the binary behind them is shared. With Skia the group
# links skia::skia and states PULP_HAS_SKIA=1 plus the Skia include path once,
# which is what the members that used to add them themselves compiled with
# (pulp::canvas exports the same define PUBLIC). The live-GPU suites below get
# their own groups, split by compile line (with and without pulp::view), and
# the embedded-host smoke stays standalone because it compiles the CLAP
# adapter sources into the test.
pulp_add_test_group(pulp-test-group-canvas-text LIBRARIES pulp::canvas)
if(PULP_HAS_SKIA)
    target_link_libraries(pulp-test-group-canvas-text PRIVATE skia::skia)
    target_compile_definitions(pulp-test-group-canvas-text PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-group-canvas-text PRIVATE ${SKIA_INCLUDE_DIRS})
endif()

# TextShaper (PreText-style measure-once-reflow-forever)
pulp_add_test_suite(pulp-test-text-shaper GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)

# SheenBidi-backed BidiAnalyzer + TextRunPlanner non-ICU fallback.
# Exercises pure-Arabic,
# pure-Hebrew, and mixed Latin+Arabic/Hebrew strings. Each TEST_CASE
# gates RTL-specific assertions on BidiAnalyzer::has_sheenbidi() so the
# suite still passes on reduced-deps configs that compile the
# pass-through stub.
pulp_add_test_suite(pulp-test-bidi-text GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)

# FontScope memory-budget eviction.
pulp_add_test_suite(pulp-test-font-scope-budget GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)

# TTF/OTF structural sanitizer.
# Gated on PULP_HAS_SKIA: the non-Skia stub in font_registry_stubs.cpp
# accepts any non-empty buffer, so the rejection cases false-negative.
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-font-security GROUP pulp-test-group-canvas-text
        LIBRARIES pulp::canvas)
endif()

# Variable axis wiring.
# Gated on PULP_HAS_SKIA: FontResolver::resolve_family_list returns
# NotFound when Skia is absent, so the has_typeface() assertions
# false-negative.
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-font-variable-axes GROUP pulp-test-group-canvas-text
        LIBRARIES pulp::canvas)
endif()

# UAX #29-lite cluster_step.
pulp_add_test_suite(pulp-test-cluster-step GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)

# Locale-aware word + line breaking.
# The surface always links: when ICU's <unicode/brkiter.h> is on the
# include path the assertions exercise the real UAX #14 path against
# the SkUnicode_icu symbols bundled in libskia.a; otherwise the
# degraded ASCII-space fallback keeps the API functional and the
# English-only expectations still hold.
pulp_add_test_suite(pulp-test-font-locale-shaping GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)

# AA / hinting / subpixel policy centralization.
pulp_add_test_suite(pulp-test-font-aa-hinting GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)

# FontFlightRecorder + JSON drain.
pulp_add_test_suite(pulp-test-font-flight-recorder GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)

# Variable-font axis-animation LRU cache.
pulp_add_test_suite(pulp-test-font-axis-animation GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)

# Parallel shaping via TextRunPlanner::shape_batch.
pulp_add_test_suite(pulp-test-text-run-planner-parallel GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)

# Real ICU bidi + script run iterators. Verifies multi-run shaping on
# bidi / script boundaries plus UAX #29 cluster grouping (ZWJ,
# regional-indicator-pair, virama,
# combining marks). Cluster-grouping cases run on every build; the
# bidi / script cases auto-skip under non-Skia builds via SUCCEED.
# The group states PULP_HAS_SKIA explicitly so the ICU-path assertions
# register.
pulp_add_test_suite(pulp-test-text-run-planner-icu GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)

# Async font lifecycle (register_font_url).
pulp_add_test_suite(pulp-test-font-async-lifecycle GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)

# WOFF2 runtime decoding (security-gated).
# Structural rejection + decoder-availability probe; no Skia link
# required because the negative paths are universal.
pulp_add_test_suite(pulp-test-font-woff2 GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas
    COMPILE_DEFINITIONS
        PULP_TEST_WOFF2_FIXTURE="${CMAKE_SOURCE_DIR}/packages/pulp-web-player/src/theme/inter.woff2"
        PULP_TEST_INTER_FIXTURE="${CMAKE_SOURCE_DIR}/external/fonts/Inter-Regular.ttf"
        PULP_TEST_JOST_FIXTURE="${CMAKE_SOURCE_DIR}/external/fonts/Jost-Regular.ttf"
        PULP_TEST_JETBRAINS_FIXTURE="${CMAKE_SOURCE_DIR}/external/fonts/JetBrainsMono-Regular.ttf")

# Color-font predicate on ResolvedFont.
# Gated on PULP_HAS_SKIA — the predicate returns false without a real
# typeface so the resolver-bound assertions can't fire on non-Skia.
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-font-color-mode GROUP pulp-test-group-canvas-text
        LIBRARIES pulp::canvas)
endif()

# Font subsystem typed options and scope generation tests
pulp_add_test_suite(pulp-test-font-options GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)

# Measurement-paint parity harness.
# Asserts TextShaper predictions match Skia's measured advance within
# 0.5px for a hand-picked sample set representative of CHAIN INFO /
# CROSSOVER bugs. The full multilingual corpus (test/text_corpus/
# corpus.json) loader + per-TextAnchor bbox assertions land alongside
# a future JSON parser link.
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-parity GROUP pulp-test-group-canvas-text
        LIBRARIES pulp::canvas)
endif()

# Cross-backend font rendering goldens.
# Catches font-cascade / hinting / AA regressions in the Skia
# raster path before they land. Uses a structural digest (width,
# height, opaque-pixel count, darkness sum) with ±5% tolerance
# rather than byte-exact pixel hashes — see the test header for
# the rationale. Skia-only; the bare TU still builds without
# Skia (single soft-skip case) so this stays portable.
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-font-rendering-goldens GROUP pulp-test-group-canvas-text
        LIBRARIES pulp::canvas)
endif()

# Skia GPU font rendering lane (Dawn → Metal on macOS-arm64).
# Sibling of `pulp-test-font-rendering-goldens` (raster). Same
# three strings, same structural digest, ±15% tolerance to absorb
# Graphite/Metal subpixel + AA drift. Gated at compile time on
# PULP_HAS_SKIA && APPLE && PULP_ENABLE_GPU — the test soft-skips
# at runtime when Dawn / Graphite init fails so CI lanes without a
# working adapter don't hard-fail. Vulkan + D3D backends are
# deferred — see the PULP_VULKAN_AVAILABLE / PULP_D3D_AVAILABLE
# scaffold blocks immediately below.
# Every suite below creates a real Dawn/Metal device and does blocking
# offscreen readbacks, so each carries RESOURCE_LOCK pulp_gpu — the same lock
# the render/GPU and gpu-audio manifests use. Without it they run in parallel
# with every other GPU test and contend for a process-global resource, which
# surfaces as an intermittent short readback: the frame composites fewer
# pixels than the assertion expects, on a PR that changed nothing related.
# Renderer-owned retained compositing layers (WAH-12): keyed lookup, typed
# owner identity, pruning of sealed-but-never-drawn non-cacheable layers, and a
# bounded LRU budget. Raster-only — none of this needs a GPU.
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-retained-layer-store GROUP pulp-test-group-canvas-text
        LIBRARIES pulp::canvas)
endif()

if(PULP_HAS_SKIA AND APPLE AND PULP_ENABLE_GPU)
    # Two live-GPU groups, split by compile line: the pulp::canvas + pulp::render
    # suites, and the ones that also drive a pulp::view tree.
    pulp_add_test_group(pulp-test-group-canvas-text-gpu
        LIBRARIES pulp::canvas pulp::render skia::skia
        INCLUDE_DIRS ${SKIA_INCLUDE_DIRS}
        COMPILE_DEFINITIONS PULP_HAS_SKIA=1)
    pulp_add_test_group(pulp-test-group-canvas-text-view-gpu
        LIBRARIES pulp::view pulp::canvas pulp::render skia::skia
        INCLUDE_DIRS ${SKIA_INCLUDE_DIRS}
        COMPILE_DEFINITIONS PULP_HAS_SKIA=1)

    # APPLE-gated, so the Windows webgpu DL_PATHS dance from the
    # PULP_GPU_TEST_DISCOVERY_ARGS block below isn't needed here.
    pulp_add_test_suite(pulp-test-font-rendering-goldens-gpu GROUP pulp-test-group-canvas-text-gpu
        LIBRARIES pulp::canvas pulp::render
        PROPERTIES RESOURCE_LOCK pulp_gpu)

    # GPU view-host-in-plugins — proves a Processor::create_view() editor
    # that sets requires_gpu_host() auto-selects the GPU host, mounts a real
    # (non-AutoUi) tree, and paints a nonblank offscreen Dawn/Skia frame.
    # Soft-skips on Dawn-init failure.
    add_executable(pulp-test-plugin-editor-headless-gpu
        test_plugin_editor_headless_gpu.cpp)
    target_link_libraries(pulp-test-plugin-editor-headless-gpu PRIVATE
        pulp::format pulp::view pulp::state pulp::canvas pulp::render
        Catch2::Catch2WithMain skia::skia)
    target_compile_definitions(pulp-test-plugin-editor-headless-gpu PRIVATE
        PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-plugin-editor-headless-gpu PRIVATE
        ${SKIA_INCLUDE_DIRS})
    catch_discover_tests(pulp-test-plugin-editor-headless-gpu
        PROPERTIES RESOURCE_LOCK pulp_gpu)

    # Subtree scene cache — live-GPU image-in-cache proof (FU-3). Records an
    # image draw inside a cached subtree on the miss frame (GPU-uploads the
    # texture via the persistent Graphite recorder) and asserts it still
    # composites on the replay frame — the cross-frame texture-lifetime gap the
    # raster tests cannot cover. Soft-skips on Dawn-init failure.
    # This live-GPU replay proof is environment-sensitive on ephemeral
    # macOS CI VMs. Keep it in push/nightly coverage while the required
    # PR and merge-group lanes exclude the existing `slow` label.
    pulp_add_test_suite(pulp-test-subtree-cache-gpu GROUP pulp-test-group-canvas-text-view-gpu
        LIBRARIES pulp::view pulp::canvas pulp::render
        LABELS slow
        PROPERTIES RESOURCE_LOCK pulp_gpu)

    # Persistent-scene mode — live-GPU cross-frame retention proof (FU-2).
    # Drives an offscreen Dawn+Skia surface with set_persistent_scene(true) for
    # a full frame then a clipped frame, and asserts the untouched content
    # survives while the clipped region updates. Soft-skips without a real adapter.
    pulp_add_test_suite(pulp-test-partial-repaint-gpu GROUP pulp-test-group-canvas-text-view-gpu
        LIBRARIES pulp::view pulp::canvas pulp::render
        PROPERTIES RESOURCE_LOCK pulp_gpu)

    # Visible-frame success contract on a LIVE Dawn/Graphite surface (WAH-2).
    # The fake-surface unit tests pin how a host REACTS to each FrameOutcome;
    # this pins which outcome the real backend produces — in particular that a
    # captured frame (whose recording read_current_rgba already flushed) still
    # reports as having reached its output. Soft-skips without a real adapter.
    pulp_add_test_suite(pulp-test-skia-frame-outcome-gpu GROUP pulp-test-group-canvas-text-gpu
        LIBRARIES pulp::canvas pulp::render
        PROPERTIES RESOURCE_LOCK pulp_gpu)

    # Embedded-host smoke (mac GPU lane): attaches the GPU host to a hidden
    # NSWindow and proves a nonblank first-frame capture, plus drives the CLAP
    # gui_create/set_parent adapter path. Needs CLAP for the PULP_CLAP_PLUGIN
    # entry. Soft-skips without a Dawn adapter / window server.
    if(PULP_HAS_CLAP)
        add_executable(pulp-test-plugin-editor-host-smoke-mac
            test_plugin_editor_host_smoke_mac.mm
            ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
            ${CMAKE_SOURCE_DIR}/core/format/src/clap_remote_controls.cpp)
        target_link_libraries(pulp-test-plugin-editor-host-smoke-mac PRIVATE
            pulp::format pulp::view pulp::state pulp::canvas pulp::render
            clap Catch2::Catch2WithMain skia::skia "-framework AppKit")
        target_compile_definitions(pulp-test-plugin-editor-host-smoke-mac PRIVATE
            PULP_HAS_SKIA=1 PULP_CLAP_GUI=1)
        target_include_directories(pulp-test-plugin-editor-host-smoke-mac PRIVATE
            ${SKIA_INCLUDE_DIRS})
        catch_discover_tests(pulp-test-plugin-editor-host-smoke-mac)
    endif()
endif()

# Skia GPU Vulkan font rendering lane (scaffold).
# Sibling of `pulp-test-font-rendering-goldens-gpu` (Metal). The Vulkan
# lane is intentionally OFF by default: there is no Vulkan-capable
# runner in Pulp's required gate set today, so building this target
# would only burn CI roundtrips on "couldn't find <vulkan/vulkan.h>"
# failures. Flip `PULP_VULKAN_AVAILABLE=ON` on a future Linux or
# Windows CI lane (with Skia-Vulkan linked) to light the target up.
# Tolerance for the raster↔Vulkan cross-backend probe is ±20 % —
# see the test header for the rationale.
option(PULP_VULKAN_AVAILABLE
    "Build the Skia GPU Vulkan rendering-goldens scaffold. Requires \
Skia built with skia_use_vulkan=true and a working Vulkan ICD on the \
host. OFF on every Pulp CI lane today; flip ON when a Vulkan-capable \
runner exists."
    OFF)
if(PULP_HAS_SKIA AND (UNIX AND NOT APPLE OR WIN32) AND PULP_VULKAN_AVAILABLE)
    add_executable(pulp-test-font-rendering-goldens-vulkan
        test_font_rendering_goldens_vulkan.cpp)
    target_link_libraries(pulp-test-font-rendering-goldens-vulkan PRIVATE
        pulp::canvas pulp::render Catch2::Catch2WithMain skia::skia)
    target_compile_definitions(pulp-test-font-rendering-goldens-vulkan PRIVATE
        PULP_HAS_SKIA=1
        PULP_VULKAN_AVAILABLE=1)
    target_include_directories(pulp-test-font-rendering-goldens-vulkan PRIVATE
        ${SKIA_INCLUDE_DIRS})
    catch_discover_tests(pulp-test-font-rendering-goldens-vulkan)
endif()

# Skia GPU Direct3D 12 font rendering lane (scaffold).
# Sibling of `pulp-test-font-rendering-goldens-gpu` (Metal). The D3D
# lane is intentionally OFF by default: there is no Windows D3D
# runner in Pulp's required gate set today, so building this target
# would only burn CI roundtrips on "couldn't find <d3d12.h>" failures.
# Flip `PULP_D3D_AVAILABLE=ON` on a future Windows CI lane (with
# Skia-D3D linked) to light the target up. Tolerance for the
# raster↔D3D cross-backend probe is ±20 % — see the test header for
# the rationale.
option(PULP_D3D_AVAILABLE
    "Build the Skia GPU Direct3D 12 rendering-goldens scaffold. \
Requires Skia built with skia_use_direct3d=true on Windows. OFF on \
every Pulp CI lane today; flip ON when a Windows D3D runner exists."
    OFF)
if(PULP_HAS_SKIA AND WIN32 AND PULP_D3D_AVAILABLE)
    add_executable(pulp-test-font-rendering-goldens-d3d
        test_font_rendering_goldens_d3d.cpp)
    target_link_libraries(pulp-test-font-rendering-goldens-d3d PRIVATE
        pulp::canvas pulp::render Catch2::Catch2WithMain skia::skia)
    target_compile_definitions(pulp-test-font-rendering-goldens-d3d PRIVATE
        PULP_HAS_SKIA=1
        PULP_D3D_AVAILABLE=1)
    target_include_directories(pulp-test-font-rendering-goldens-d3d PRIVATE
        ${SKIA_INCLUDE_DIRS})
    catch_discover_tests(pulp-test-font-rendering-goldens-d3d)
endif()

# Path: the retained vector value type. Tight-vs-hull bounds, the two fill rules
# disagreeing about a twice-enclosed region, and scale_to_fit's centring and its
# degenerate guard (a zero-width path scaled to a non-zero width is a division by
# zero, and the "result" is a path of NaNs that renders as nothing, forever).
pulp_add_test_suite(pulp-test-canvas-path GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)

# PathMeasure: arc length over a Path, and the trim built on it. Covers the
# clamping contract a meter depends on, the zero-length cases that must report
# no contour, and the requirement that trimming a cubic yields a cubic rather
# than a polyline.
pulp_add_test_suite(pulp-test-canvas-path-measure GROUP pulp-test-group-canvas-text
    LIBRARIES pulp::canvas)
