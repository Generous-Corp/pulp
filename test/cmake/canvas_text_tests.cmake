# Canvas text, font, shaping, and font-rendering tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# TextShaper (PreText-style measure-once-reflow-forever)
pulp_add_test_suite(pulp-test-text-shaper LIBRARIES pulp::canvas)

# SheenBidi-backed BidiAnalyzer + TextRunPlanner non-ICU fallback.
# Exercises pure-Arabic,
# pure-Hebrew, and mixed Latin+Arabic/Hebrew strings. Each TEST_CASE
# gates RTL-specific assertions on BidiAnalyzer::has_sheenbidi() so the
# suite still passes on reduced-deps configs that compile the
# pass-through stub.
pulp_add_test_suite(pulp-test-bidi-text LIBRARIES pulp::canvas)

# FontScope memory-budget eviction.
pulp_add_test_suite(pulp-test-font-scope-budget LIBRARIES pulp::canvas)

# TTF/OTF structural sanitizer.
# Gated on PULP_HAS_SKIA: the non-Skia stub in font_registry_stubs.cpp
# accepts any non-empty buffer, so the rejection cases false-negative.
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-font-security LIBRARIES pulp::canvas)
endif()

# Variable axis wiring.
# Gated on PULP_HAS_SKIA: FontResolver::resolve_family_list returns
# NotFound when Skia is absent, so the has_typeface() assertions
# false-negative.
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-font-variable-axes LIBRARIES pulp::canvas)
endif()

# UAX #29-lite cluster_step.
pulp_add_test_suite(pulp-test-cluster-step LIBRARIES pulp::canvas)

# Locale-aware word + line breaking.
# The surface always links: when ICU's <unicode/brkiter.h> is on the
# include path the assertions exercise the real UAX #14 path against
# the SkUnicode_icu symbols bundled in libskia.a; otherwise the
# degraded ASCII-space fallback keeps the API functional and the
# English-only expectations still hold.
pulp_add_test_suite(pulp-test-font-locale-shaping LIBRARIES pulp::canvas)

# AA / hinting / subpixel policy centralization.
add_executable(pulp-test-font-aa-hinting test_font_aa_hinting.cpp)
target_link_libraries(pulp-test-font-aa-hinting PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_link_libraries(pulp-test-font-aa-hinting PRIVATE skia::skia)
endif()
catch_discover_tests(pulp-test-font-aa-hinting)

# FontFlightRecorder + JSON drain.
add_executable(pulp-test-font-flight-recorder test_font_flight_recorder.cpp)
target_link_libraries(pulp-test-font-flight-recorder PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-flight-recorder)

# Variable-font axis-animation LRU cache.
add_executable(pulp-test-font-axis-animation test_font_axis_animation.cpp)
target_link_libraries(pulp-test-font-axis-animation PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-axis-animation)

# Parallel shaping via TextRunPlanner::shape_batch.
add_executable(pulp-test-text-run-planner-parallel test_text_run_planner_parallel.cpp)
target_link_libraries(pulp-test-text-run-planner-parallel PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-text-run-planner-parallel)

# Real ICU bidi + script run iterators. Verifies multi-run shaping on
# bidi / script boundaries plus UAX #29 cluster grouping (ZWJ,
# regional-indicator-pair, virama,
# combining marks). Cluster-grouping cases run on every build; the
# bidi / script cases auto-skip under non-Skia builds via SUCCEED.
#
# Add PULP_HAS_SKIA explicitly so the ICU-path assertions register
# (mirrors test_canvas_fonts.cpp's pattern — pulp::canvas exports the
# define PUBLIC but CMake's PUBLIC propagation doesn't always reach
# sibling test targets, depending on configure ordering).
add_executable(pulp-test-text-run-planner-icu test_text_run_planner_icu.cpp)
target_link_libraries(pulp-test-text-run-planner-icu PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-text-run-planner-icu PRIVATE PULP_HAS_SKIA=1)
endif()
catch_discover_tests(pulp-test-text-run-planner-icu)

# Async font lifecycle (register_font_url).
add_executable(pulp-test-font-async-lifecycle test_font_async_lifecycle.cpp)
target_link_libraries(pulp-test-font-async-lifecycle PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-async-lifecycle)

# WOFF2 runtime decoding (security-gated).
# Structural rejection + decoder-availability probe; no Skia link
# required because the negative paths are universal.
add_executable(pulp-test-font-woff2 test_font_woff2.cpp)
target_link_libraries(pulp-test-font-woff2 PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-woff2)

# Color-font predicate on ResolvedFont.
# Gated on PULP_HAS_SKIA — the predicate returns false without a real
# typeface so the resolver-bound assertions can't fire on non-Skia.
if(PULP_HAS_SKIA)
    add_executable(pulp-test-font-color-mode test_font_color_mode.cpp)
    target_link_libraries(pulp-test-font-color-mode PRIVATE pulp::canvas Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-font-color-mode)
endif()

# Font subsystem typed options and scope generation tests
add_executable(pulp-test-font-options test_font_options.cpp)
target_link_libraries(pulp-test-font-options PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-options)

# Measurement-paint parity harness.
# Asserts TextShaper predictions match Skia's measured advance within
# 0.5px for a hand-picked sample set representative of CHAIN INFO /
# CROSSOVER bugs. The full multilingual corpus (test/text_corpus/
# corpus.json) loader + per-TextAnchor bbox assertions land alongside
# a future JSON parser link.
if(PULP_HAS_SKIA)
    add_executable(pulp-test-parity test_parity.cpp)
    target_link_libraries(pulp-test-parity PRIVATE pulp::canvas Catch2::Catch2WithMain skia::skia)
    catch_discover_tests(pulp-test-parity)
endif()

# Cross-backend font rendering goldens.
# Catches font-cascade / hinting / AA regressions in the Skia
# raster path before they land. Uses a structural digest (width,
# height, opaque-pixel count, darkness sum) with ±5% tolerance
# rather than byte-exact pixel hashes — see the test header for
# the rationale. Skia-only; the bare TU still builds without
# Skia (single soft-skip case) so this stays portable.
if(PULP_HAS_SKIA)
    add_executable(pulp-test-font-rendering-goldens
        test_font_rendering_goldens.cpp)
    target_link_libraries(pulp-test-font-rendering-goldens PRIVATE
        pulp::canvas Catch2::Catch2WithMain skia::skia)
    target_compile_definitions(pulp-test-font-rendering-goldens PRIVATE
        PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-font-rendering-goldens PRIVATE
        ${SKIA_INCLUDE_DIRS})
    catch_discover_tests(pulp-test-font-rendering-goldens)
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
if(PULP_HAS_SKIA AND APPLE AND PULP_ENABLE_GPU)
    add_executable(pulp-test-font-rendering-goldens-gpu
        test_font_rendering_goldens_gpu.cpp)
    target_link_libraries(pulp-test-font-rendering-goldens-gpu PRIVATE
        pulp::canvas pulp::render Catch2::Catch2WithMain skia::skia)
    target_compile_definitions(pulp-test-font-rendering-goldens-gpu PRIVATE
        PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-font-rendering-goldens-gpu PRIVATE
        ${SKIA_INCLUDE_DIRS})
    # APPLE-gated, so the Windows webgpu DL_PATHS dance from the
    # PULP_GPU_TEST_DISCOVERY_ARGS block below isn't needed here.
    catch_discover_tests(pulp-test-font-rendering-goldens-gpu)

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
    catch_discover_tests(pulp-test-plugin-editor-headless-gpu)

    # Subtree scene cache — live-GPU image-in-cache proof (FU-3). Records an
    # image draw inside a cached subtree on the miss frame (GPU-uploads the
    # texture via the persistent Graphite recorder) and asserts it still
    # composites on the replay frame — the cross-frame texture-lifetime gap the
    # raster tests cannot cover. Soft-skips on Dawn-init failure.
    add_executable(pulp-test-subtree-cache-gpu test_subtree_cache_gpu.cpp)
    target_link_libraries(pulp-test-subtree-cache-gpu PRIVATE
        pulp::view pulp::canvas pulp::render
        Catch2::Catch2WithMain skia::skia)
    target_compile_definitions(pulp-test-subtree-cache-gpu PRIVATE
        PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-subtree-cache-gpu PRIVATE
        ${SKIA_INCLUDE_DIRS})
    catch_discover_tests(pulp-test-subtree-cache-gpu)

    # Persistent-scene mode — live-GPU cross-frame retention proof (FU-2).
    # Drives an offscreen Dawn+Skia surface with set_persistent_scene(true) for
    # a full frame then a clipped frame, and asserts the untouched content
    # survives while the clipped region updates. Soft-skips without a real adapter.
    add_executable(pulp-test-partial-repaint-gpu test_partial_repaint_gpu.cpp)
    target_link_libraries(pulp-test-partial-repaint-gpu PRIVATE
        pulp::view pulp::canvas pulp::render
        Catch2::Catch2WithMain skia::skia)
    target_compile_definitions(pulp-test-partial-repaint-gpu PRIVATE
        PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-partial-repaint-gpu PRIVATE
        ${SKIA_INCLUDE_DIRS})
    catch_discover_tests(pulp-test-partial-repaint-gpu)

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
pulp_add_test_suite(pulp-test-canvas-path LIBRARIES pulp::canvas)
