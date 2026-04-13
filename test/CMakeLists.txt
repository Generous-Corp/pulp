# Pulp test suite — Catch2

# Build check (no Catch2 dependency)
add_executable(pulp-test-build-check test_build_check.cpp)
target_link_libraries(pulp-test-build-check PRIVATE pulp::platform pulp::runtime)
add_test(NAME build-check COMMAND pulp-test-build-check)

# Validation contract tests (Phase 1 — schema and reality snapshot)
add_executable(pulp-test-validation-contract test_validation_contract.cpp)
target_link_libraries(pulp-test-validation-contract PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-validation-contract)

# CLI design binding tests
add_executable(pulp-test-cli-design-binding test_cli_design_binding.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/design_binding.cpp
)
target_include_directories(pulp-test-cli-design-binding PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-design-binding PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-design-binding)

# CLI create target selection tests
add_executable(pulp-test-cli-create-targets test_cli_create_targets.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/create_targets.cpp
)
target_include_directories(pulp-test-cli-create-targets PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-create-targets PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-create-targets)

# Child process tests
add_executable(pulp-test-child-process test_child_process.cpp)
target_link_libraries(pulp-test-child-process PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-child-process)

# Progress parser tests
add_executable(pulp-test-progress-parser test_progress_parser.cpp)
target_link_libraries(pulp-test-progress-parser PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-progress-parser)

# Stream tests (unified I/O interface)
add_executable(pulp-test-stream test_stream.cpp)
target_link_libraries(pulp-test-stream PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-stream)

# AsyncStream tests (Phase 2)
add_executable(pulp-test-async-stream test_async_stream.cpp)
target_link_libraries(pulp-test-async-stream PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-async-stream)

# Network Stream tests (Phase 3)
add_executable(pulp-test-network-stream test_network_stream.cpp)
target_link_libraries(pulp-test-network-stream PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-network-stream)

# Validation harness tests (Phase 2)
add_executable(pulp-test-validation-harness test_validation_harness.cpp)
target_link_libraries(pulp-test-validation-harness PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-validation-harness)

# AAX metadata/model tests
add_executable(pulp-test-aax-model test_aax_model.cpp)
target_link_libraries(pulp-test-aax-model PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-aax-model)

# Appcast/auto-update tests
add_executable(pulp-test-appcast test_appcast.cpp)
target_link_libraries(pulp-test-appcast PRIVATE pulp::ship Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-appcast)

# V3 gaps tests (Bias, MidiSequence, SubsectionReader, HostType, Primes, Expression)
add_executable(pulp-test-v3-gaps test_v3_gaps.cpp)
target_link_libraries(pulp-test-v3-gaps PRIVATE pulp::signal pulp::midi pulp::audio pulp::format pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-v3-gaps)

# Codesign tests
add_executable(pulp-test-codesign test_codesign.cpp)
target_link_libraries(pulp-test-codesign PRIVATE pulp::ship Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-codesign)

# Sync primitives tests (SeqLock, TripleBuffer)
add_executable(pulp-test-sync test_sync_primitives.cpp)
target_link_libraries(pulp-test-sync PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sync)

# License and BigInteger tests
add_executable(pulp-test-license test_license.cpp)
target_link_libraries(pulp-test-license PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-license)

# MIDI CI tests
add_executable(pulp-test-midi-ci test_midi_ci.cpp)
target_link_libraries(pulp-test-midi-ci PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-midi-ci)

# TextShaper (PreText-style measure-once-reflow-forever)
add_executable(pulp-test-text-shaper test_text_shaper.cpp)
target_link_libraries(pulp-test-text-shaper PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-text-shaper)

# Analytics tests
add_executable(pulp-test-analytics test_analytics.cpp)
target_link_libraries(pulp-test-analytics PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-analytics)

# Crypto tests (SHA-256, MD5, AES, machine ID)
add_executable(pulp-test-crypto test_crypto.cpp)
target_link_libraries(pulp-test-crypto PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-crypto)

# IPC (InterprocessConnection) tests
add_executable(pulp-test-ipc test_ipc.cpp)
target_link_libraries(pulp-test-ipc PRIVATE pulp::events pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ipc)

# AttributedString and TextLayout tests
add_executable(pulp-test-attributed-string test_attributed_string.cpp)
target_link_libraries(pulp-test-attributed-string PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-attributed-string)

# i18n translation tests
add_executable(pulp-test-i18n test_i18n.cpp)
target_link_libraries(pulp-test-i18n PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-i18n)

# StateTree and ObservableValue tests
add_executable(pulp-test-state-tree test_state_tree.cpp)
target_link_libraries(pulp-test-state-tree PRIVATE pulp::state pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-state-tree)

# GUI component tests (table, toolbar, concertina, buttons, lasso)
add_executable(pulp-test-gui-components test_gui_components.cpp)
target_link_libraries(pulp-test-gui-components PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-gui-components)

# CodeEditor / FileBasedDocument / RecentlyOpenedFilesList tests
add_executable(pulp-test-code-editor test_code_editor.cpp)
target_link_libraries(pulp-test-code-editor PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-code-editor)

# PropertiesFile JSON persistence tests
add_executable(pulp-test-properties test_properties_file.cpp)
target_link_libraries(pulp-test-properties PRIVATE pulp::state pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-properties)

# DSP enhancement tests (dry/wet mixer, processor duplicator, matrix, special functions)
add_executable(pulp-test-dsp-enhancements test_dsp_enhancements.cpp)
target_link_libraries(pulp-test-dsp-enhancements PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-dsp-enhancements)

# Animation & 3D math tests
add_executable(pulp-test-animation-3d test_animation_3d.cpp)
target_link_libraries(pulp-test-animation-3d PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-animation-3d)

# AnimatorSet / 3D types tests
add_executable(pulp-test-animator-set test_animator_set.cpp)
target_link_libraries(pulp-test-animator-set PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-animator-set)

# Runtime utility tests (mmap, temp file, dynlib, base64, range, child process)
add_executable(pulp-test-runtime-utils test_runtime_utils.cpp)
target_link_libraries(pulp-test-runtime-utils PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-runtime-utils)

# XML and ZIP/GZIP compression tests
add_executable(pulp-test-xml-zip test_xml_zip.cpp)
target_link_libraries(pulp-test-xml-zip PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-xml-zip)

# SIMD operations and aligned buffer tests
add_executable(pulp-test-simd test_simd.cpp)
target_link_libraries(pulp-test-simd PRIVATE pulp::runtime pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-simd)

# Drag-and-drop tests
add_executable(pulp-test-dnd test_drag_drop.cpp)
target_link_libraries(pulp-test-dnd PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-dnd)

# OSC tests
add_executable(pulp-test-osc test_osc.cpp)
target_link_libraries(pulp-test-osc PRIVATE pulp::osc Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-osc)

# ImageConvolutionKernel tests
add_executable(pulp-test-image-convolution test_image_convolution.cpp)
target_link_libraries(pulp-test-image-convolution PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-image-convolution)

# RectangleList geometry tests
add_executable(pulp-test-rectangle-list test_rectangle_list.cpp)
target_link_libraries(pulp-test-rectangle-list PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-rectangle-list)

# OSC Bundle serialization tests
add_executable(pulp-test-osc-bundle test_osc_bundle.cpp)
target_link_libraries(pulp-test-osc-bundle PRIVATE pulp::osc Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-osc-bundle)

# SVG tests
add_executable(pulp-test-svg test_svg.cpp)
target_link_libraries(pulp-test-svg PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-svg)

# Effects tests
add_executable(pulp-test-effects test_effects.cpp)
target_link_libraries(pulp-test-effects PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-effects)

# Signal/DSP tests
add_executable(pulp-test-signal test_signal.cpp)
target_link_libraries(pulp-test-signal PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-signal)

# Convolution engine tests
add_executable(pulp-test-convolver test_convolver.cpp)
target_link_libraries(pulp-test-convolver PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-convolver)

# Test signal source (sine tone, file playback)
add_executable(pulp-test-test-signal test_test_signal.cpp)
target_link_libraries(pulp-test-test-signal PRIVATE pulp::standalone Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-test-signal)

# STFT and audio visualization signal tests
add_executable(pulp-test-stft test_stft.cpp)
target_link_libraries(pulp-test-stft PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-stft)

# Visualization bridge and widget tests
add_executable(pulp-test-visualization test_visualization.cpp)
target_link_libraries(pulp-test-visualization PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-visualization)

# Clipboard tests
add_executable(pulp-test-clipboard test_clipboard.cpp)
target_link_libraries(pulp-test-clipboard PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-clipboard)

# Platform tests
add_executable(pulp-test-platform test_platform.cpp)
target_link_libraries(pulp-test-platform PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-platform)

# Runtime tests
add_executable(pulp-test-runtime test_runtime.cpp)
target_link_libraries(pulp-test-runtime PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-runtime)

# Events tests
add_executable(pulp-test-events test_events.cpp)
target_link_libraries(pulp-test-events PRIVATE pulp::events Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-events)

# Audio file I/O tests
add_executable(pulp-test-audio-file test_audio_file.cpp)
target_link_libraries(pulp-test-audio-file PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio-file)

# Audio tests
add_executable(pulp-test-audio test_audio.cpp)
target_link_libraries(pulp-test-audio PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio)

# Excerpt window enumeration tests
add_executable(pulp-test-audio-excerpt test_audio_excerpt.cpp)
target_link_libraries(pulp-test-audio-excerpt PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio-excerpt)

# Repo-level audio tooling tests
add_executable(pulp-test-audio-tools test_audio_tools.cpp)
target_link_libraries(pulp-test-audio-tools PRIVATE pulp::tool-audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio-tools)

# MIDI tests
add_executable(pulp-test-midi test_midi.cpp)
target_link_libraries(pulp-test-midi PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-midi)

# State tests
add_executable(pulp-test-state test_state.cpp)
target_link_libraries(pulp-test-state PRIVATE pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-state)

# Binding tests
add_executable(pulp-test-binding test_binding.cpp)
target_link_libraries(pulp-test-binding PRIVATE pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-binding)

# Headless adapter tests
add_executable(pulp-test-headless test_headless.cpp)
target_link_libraries(pulp-test-headless PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-headless)

# Diagnostic reporter tests
add_executable(pulp-test-diagnostic test_diagnostic_reporter.cpp)
target_link_libraries(pulp-test-diagnostic PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-diagnostic)

# CLAP entry point macro test
if(PULP_HAS_CLAP)
    add_executable(pulp-test-clap-entry test_clap_entry.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
    )
    target_link_libraries(pulp-test-clap-entry PRIVATE pulp::format clap Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-clap-entry)
endif()

# LV2 adapter tests
if(PULP_HAS_LV2)
    add_executable(pulp-test-lv2-adapter test_lv2_adapter.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/lv2_adapter.cpp
    )
    target_link_libraries(pulp-test-lv2-adapter PRIVATE pulp::format lv2-headers Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-lv2-adapter)
endif()

# NSIS installer script generation tests (cross-platform — pure string output)
add_executable(pulp-test-nsis-installer test_nsis_installer.cpp
    ${CMAKE_SOURCE_DIR}/ship/platform/win/installer_win.cpp
)
target_link_libraries(pulp-test-nsis-installer PRIVATE pulp::ship Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-nsis-installer)

# Image decode / glTF texture pipeline tests
add_executable(pulp-test-image-decode test_image_decode.cpp)
target_link_libraries(pulp-test-image-decode PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-image-decode)

# GPU surface tests
if(PULP_ENABLE_GPU)
    set(PULP_GPU_TEST_DISCOVERY_ARGS)
    if(WIN32 AND TARGET webgpu)
        list(APPEND PULP_GPU_TEST_DISCOVERY_ARGS
            DISCOVERY_MODE PRE_TEST
            DL_PATHS "$<TARGET_FILE_DIR:webgpu>"
        )
    endif()

    add_executable(pulp-test-gpu-surface test_gpu_surface.cpp)
    target_link_libraries(pulp-test-gpu-surface PRIVATE pulp::render Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-gpu-surface ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # Skia surface test
    add_executable(pulp-test-skia-surface test_skia_surface.cpp)
    target_link_libraries(pulp-test-skia-surface PRIVATE pulp::render Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-skia-surface ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # GPU compute tests (Phase 11 feasibility)
    add_executable(pulp-test-gpu-compute test_gpu_compute.cpp)
    target_link_libraries(pulp-test-gpu-compute PRIVATE pulp::render pulp::signal Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-gpu-compute ${PULP_GPU_TEST_DISCOVERY_ARGS})
endif()

# Animation tests
add_executable(pulp-test-animation test_animation.cpp)
target_link_libraries(pulp-test-animation PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-animation)

# FrameClock tests
add_executable(pulp-test-frame-clock test_frame_clock.cpp)
target_link_libraries(pulp-test-frame-clock PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-frame-clock)

# Widget animation behavior tests
add_executable(pulp-test-widget-animation test_widget_animation.cpp)
target_link_libraries(pulp-test-widget-animation PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-animation)

# Auto UI tests
add_executable(pulp-test-auto-ui test_auto_ui.cpp)
target_link_libraries(pulp-test-auto-ui PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-auto-ui)

# ViewBridge lifecycle tests
add_executable(pulp-test-view-bridge test_view_bridge.cpp)
target_link_libraries(pulp-test-view-bridge PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-view-bridge)

# Script engine tests (legacy API)
add_executable(pulp-test-script test_script_engine.cpp)
target_link_libraries(pulp-test-script PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-script)

# Scripted UI hot reload/theme reload tests
add_executable(pulp-test-scripted-ui test_scripted_ui.cpp)
target_link_libraries(pulp-test-scripted-ui PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-scripted-ui)

# JS engine abstraction tests (shared across all backends)
add_executable(pulp-test-js-engine test_js_engine.cpp)
target_link_libraries(pulp-test-js-engine PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-js-engine)

# Theme/design token tests
add_executable(pulp-test-theme test_theme.cpp)
target_link_libraries(pulp-test-theme PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-theme)

# Screenshot tests (macOS only — uses CoreGraphics bitmap context)
if(APPLE)
    add_executable(pulp-test-screenshot test_screenshot.cpp)
    target_link_libraries(pulp-test-screenshot PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot)
endif()

# Canvas tests
add_executable(pulp-test-canvas test_canvas.cpp)
target_link_libraries(pulp-test-canvas PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-canvas)

# SDF glyph atlas exploration (#76)
add_executable(pulp-test-sdf-atlas test_sdf_atlas.cpp)
target_link_libraries(pulp-test-sdf-atlas PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sdf-atlas)

# MSDF glyph atlas (Phase 2 scaffold)
add_executable(pulp-test-msdf-atlas test_msdf_atlas.cpp)
target_link_libraries(pulp-test-msdf-atlas PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-msdf-atlas)

# PSDF glyph atlas + vector-fallback (Phase 3 scaffold)
add_executable(pulp-test-psdf-atlas test_psdf_atlas.cpp)
target_link_libraries(pulp-test-psdf-atlas PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-psdf-atlas)

# Shared SDF/MSDF text helpers (pen snap + build_text_quads + fill_text_*)
add_executable(pulp-test-sdf-text test_sdf_text.cpp)
target_link_libraries(pulp-test-sdf-text PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sdf-text)

# SDF effects layer (Phase 4 — host-side API + presets)
add_executable(pulp-test-sdf-effects test_sdf_effects.cpp)
target_link_libraries(pulp-test-sdf-effects PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sdf-effects)

# Runtime SDF atlas cache (Phase 5 — LRU + frame-based eviction)
add_executable(pulp-test-sdf-atlas-cache test_sdf_atlas_cache.cpp)
target_link_libraries(pulp-test-sdf-atlas-cache PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sdf-atlas-cache)

# Software SDF renderer (end-to-end headless raster)
add_executable(pulp-test-sdf-software-renderer test_sdf_software_renderer.cpp)
target_link_libraries(pulp-test-sdf-software-renderer PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sdf-software-renderer)

# Runtime path → SDF conversion (Phase 5 — on-device SDF generation)
add_executable(pulp-test-path-to-sdf test_path_to_sdf.cpp)
target_link_libraries(pulp-test-path-to-sdf PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-path-to-sdf)

# View and layout tests
add_executable(pulp-test-view test_view.cpp)
target_link_libraries(pulp-test-view PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-view)

# Dirty region tracker tests
add_executable(pulp-test-dirty-tracker test_dirty_tracker.cpp)
target_link_libraries(pulp-test-dirty-tracker PRIVATE pulp::render Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-dirty-tracker)

# Rendering integration tests (effects, dimensions, text direction, render passes)
add_executable(pulp-test-rendering-integration test_rendering_integration.cpp)
target_link_libraries(pulp-test-rendering-integration PRIVATE pulp::view pulp::render pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-rendering-integration)

# Draw batcher tests
add_executable(pulp-test-draw-batcher test_draw_batcher.cpp)
target_link_libraries(pulp-test-draw-batcher PRIVATE pulp::render Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-draw-batcher)

# GPU graph renderer tests
add_executable(pulp-test-gpu-graph test_gpu_graph.cpp)
target_link_libraries(pulp-test-gpu-graph PRIVATE pulp::render Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-gpu-graph)

# Texture atlas tests
add_executable(pulp-test-texture-atlas test_texture_atlas.cpp)
target_link_libraries(pulp-test-texture-atlas PRIVATE pulp::render Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-texture-atlas)

# Sprite strip tests
add_executable(pulp-test-sprite-strip test_sprite_strip.cpp)
target_link_libraries(pulp-test-sprite-strip PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sprite-strip)

# Edit history (global undo) tests
add_executable(pulp-test-edit-history test_edit_history.cpp)
target_link_libraries(pulp-test-edit-history PRIVATE pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-edit-history)

# Platform maturity tests (cursor, focus, IME, context menu, accessibility)
add_executable(pulp-test-platform-maturity test_platform_maturity.cpp)
target_link_libraries(pulp-test-platform-maturity PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-platform-maturity)

# Audio bridge tests
add_executable(pulp-test-audio-bridge test_audio_bridge.cpp)
target_link_libraries(pulp-test-audio-bridge PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio-bridge)

# Widget tests
add_executable(pulp-test-widgets test_widgets.cpp)
target_link_libraries(pulp-test-widgets PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widgets)

# Hot-reload tests
add_executable(pulp-test-hot-reload test_hot_reload.cpp)
target_link_libraries(pulp-test-hot-reload PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-hot-reload)

# Inspector tests
add_executable(pulp-test-inspector test_inspector.cpp)
target_link_libraries(pulp-test-inspector PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-inspector)

# Widget bridge tests
add_executable(pulp-test-widget-bridge test_widget_bridge.cpp)
target_link_libraries(pulp-test-widget-bridge PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-bridge)

# Input events tests
add_executable(pulp-test-input-events test_input_events.cpp)
target_link_libraries(pulp-test-input-events PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-input-events)

# Text editor tests
add_executable(pulp-test-text-editor test_text_editor.cpp)
target_link_libraries(pulp-test-text-editor PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-text-editor)

# TextEditor input pipeline tests (headless — validates focus, typing, Enter, backspace)
add_executable(pulp-test-text-input test_text_input.cpp)
target_link_libraries(pulp-test-text-input PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-text-input)

# W3C Layout parity tests (flexbox, box model, visual properties)
add_executable(pulp-test-layout-w3c test_layout_w3c.cpp)
target_link_libraries(pulp-test-layout-w3c PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-layout-w3c)

# CanvasWidget tests (JS-driven custom drawing)
add_executable(pulp-test-canvas-widget test_canvas_widget.cpp)
target_link_libraries(pulp-test-canvas-widget PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-canvas-widget)

# ScrollView tests
add_executable(pulp-test-scroll-view test_scroll_view.cpp)
target_link_libraries(pulp-test-scroll-view PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-scroll-view)

# ComboBox dropdown interaction tests
add_executable(pulp-test-combo-dropdown test_combo_dropdown.cpp)
target_link_libraries(pulp-test-combo-dropdown PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-combo-dropdown)

# Panel widget tests (styled containers)
add_executable(pulp-test-panel test_panel.cpp)
target_link_libraries(pulp-test-panel PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-panel)

# UI components tests (ComboBox, TabPanel, ListBox, ScrollView, Tooltip, ProgressBar, CallOutBox)
add_executable(pulp-test-ui-components test_ui_components.cpp)
target_link_libraries(pulp-test-ui-components PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ui-components)

# Modal overlay tests
add_executable(pulp-test-modal test_modal.cpp)
target_link_libraries(pulp-test-modal PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-modal)

# Design export tests
add_executable(pulp-test-design-export test_design_export.cpp)
target_link_libraries(pulp-test-design-export PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-export)

# Design import tests (Figma/Stitch/v0/Pencil → Pulp, W3C tokens)
add_executable(pulp-test-design-import test_design_import.cpp)
target_link_libraries(pulp-test-design-import PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import)

# Screenshot comparison tests (visual diff for design import validation)
if(APPLE)
    add_executable(pulp-test-screenshot-compare test_screenshot_compare.cpp)
    target_link_libraries(pulp-test-screenshot-compare PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot-compare)
endif()

# Parameter attachment tests
add_executable(pulp-test-param-attachment test_param_attachment.cpp)
target_link_libraries(pulp-test-param-attachment PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-param-attachment)

# App framework tests (KeyMapping, MenuBar, Toolbar, AppSettings)
add_executable(pulp-test-app-framework test_app_framework.cpp)
target_link_libraries(pulp-test-app-framework PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-app-framework)

# Preset manager tests
add_executable(pulp-test-preset-manager test_preset_manager.cpp)
target_link_libraries(pulp-test-preset-manager PRIVATE pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-preset-manager)

# Waveform editor tests
add_executable(pulp-test-waveform-editor test_waveform_editor.cpp)
target_link_libraries(pulp-test-waveform-editor PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-waveform-editor)

# FastMath approximation tests
add_executable(pulp-test-fast-math test_fast_math.cpp)
target_link_libraries(pulp-test-fast-math PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-fast-math)

# Polynomial/Matrix math tests
add_executable(pulp-test-poly-math test_poly_math.cpp)
target_link_libraries(pulp-test-poly-math PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-poly-math)

# Preset browser UI tests
add_executable(pulp-test-preset-browser test_preset_browser.cpp)
target_link_libraries(pulp-test-preset-browser PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-preset-browser)

# DSP expansion tests (FIR, Ballistics, LogRamp, ProcessorChain, LookupTable, TPT)
add_executable(pulp-test-dsp-expansion test_dsp_expansion.cpp)
target_link_libraries(pulp-test-dsp-expansion PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-dsp-expansion)

# Interpolator tests (Hermite, Lagrange, windowed-sinc)
add_executable(pulp-test-interpolator test_interpolator.cpp)
target_link_libraries(pulp-test-interpolator PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-interpolator)

# MIDI expansion tests (RPN/NRPN parser, MidiKeyboardState)
add_executable(pulp-test-midi-expansion test_midi_expansion.cpp)
target_link_libraries(pulp-test-midi-expansion PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-midi-expansion)

# Audio process load measurer tests
add_executable(pulp-test-load-measurer test_load_measurer.cpp)
target_link_libraries(pulp-test-load-measurer PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-load-measurer)

# FilterDesign tests
add_executable(pulp-test-filter-design test_filter_design.cpp)
target_link_libraries(pulp-test-filter-design PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-filter-design)

# TreeView tests
add_executable(pulp-test-tree-view test_tree_view.cpp)
target_link_libraries(pulp-test-tree-view PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-tree-view)

# Undo manager tests
add_executable(pulp-test-undo-manager test_undo_manager.cpp)
target_link_libraries(pulp-test-undo-manager PRIVATE pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-undo-manager)

# BufferingReader tests
add_executable(pulp-test-buffering-reader test_buffering_reader.cpp)
target_link_libraries(pulp-test-buffering-reader PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-buffering-reader)

# AudioWorkgroup tests
add_executable(pulp-test-workgroup test_workgroup.cpp)
target_link_libraries(pulp-test-workgroup PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-workgroup)

# Plugin matrix tests (sample rate/buffer size/edge case sweep)
add_executable(pulp-test-matrix test_plugin_matrix.cpp)
target_link_libraries(pulp-test-matrix PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain)
catch_discover_tests(pulp-test-matrix)

# Per-plugin matrix tests (separate TUs to avoid enum collisions)
add_executable(pulp-test-matrix-effect test_matrix_effect.cpp)
target_link_libraries(pulp-test-matrix-effect PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-effect PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-effect)
catch_discover_tests(pulp-test-matrix-effect)

add_executable(pulp-test-matrix-compressor test_matrix_compressor.cpp)
target_link_libraries(pulp-test-matrix-compressor PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-compressor PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-compressor)
catch_discover_tests(pulp-test-matrix-compressor)

add_executable(pulp-test-matrix-tone test_matrix_tone.cpp)
target_link_libraries(pulp-test-matrix-tone PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-tone PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-tone)
catch_discover_tests(pulp-test-matrix-tone)

add_executable(pulp-test-matrix-drums test_matrix_drums.cpp)
target_link_libraries(pulp-test-matrix-drums PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-drums PRIVATE ${CMAKE_SOURCE_DIR}/examples/PulpDrums)
catch_discover_tests(pulp-test-matrix-drums)

add_executable(pulp-test-matrix-synth test_matrix_synth.cpp)
target_link_libraries(pulp-test-matrix-synth PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-synth PRIVATE ${CMAKE_SOURCE_DIR}/examples/PulpSynth)
catch_discover_tests(pulp-test-matrix-synth)

add_executable(pulp-test-matrix-sampler test_matrix_sampler.cpp)
target_link_libraries(pulp-test-matrix-sampler PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-sampler PRIVATE ${CMAKE_SOURCE_DIR}/examples/PulpSampler)
catch_discover_tests(pulp-test-matrix-sampler)

# Golden-file audio tests (uses HeadlessHost + PulpGain + PulpTone)
add_executable(pulp-test-golden test_golden_audio.cpp)
target_link_libraries(pulp-test-golden PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-golden PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/pulp-gain
    ${CMAKE_SOURCE_DIR}/examples/pulp-tone
)
catch_discover_tests(pulp-test-golden)

# Golden-file tests for PulpEffect (separate to avoid enum collision with PulpGain)
add_executable(pulp-test-golden-effect test_golden_effect.cpp)
target_link_libraries(pulp-test-golden-effect PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-golden-effect PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-effect)
catch_discover_tests(pulp-test-golden-effect)

# Negative-path tests (NaN, Inf, extreme sample rates, edge cases)
add_executable(pulp-test-negative-path test_negative_path.cpp)
target_link_libraries(pulp-test-negative-path PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-negative-path PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/pulp-gain
    ${CMAKE_SOURCE_DIR}/examples/pulp-tone
)
catch_discover_tests(pulp-test-negative-path)

# iOS foundation tests (platform detection, safe area geometry, touch events)
add_executable(pulp-test-ios-foundation test_ios_foundation.cpp)
target_link_libraries(pulp-test-ios-foundation PRIVATE pulp::view pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ios-foundation)

# Identity/UUID tests
add_executable(pulp-test-identity test_identity.cpp)
target_link_libraries(pulp-test-identity PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-identity)

# WebView tests (requires PULP_BUILD_WEBVIEW — WebViewPanel::create is only compiled when ON)
if(PULP_BUILD_WEBVIEW)
    add_executable(pulp-test-webview test_web_view.cpp)
    target_link_libraries(pulp-test-webview PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-webview)
endif()

# MIDI 2.0 UMP/MPE tests
add_executable(pulp-test-ump-mpe test_ump_mpe.cpp)
target_link_libraries(pulp-test-ump-mpe PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ump-mpe)

# MPE voice tracker tests
add_executable(pulp-test-mpe-voice-tracker test_mpe_voice_tracker.cpp)
target_link_libraries(pulp-test-mpe-voice-tracker PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-mpe-voice-tracker)

# UMP buffer + MIDI 1.0 <-> UMP conversion + UMP-aware MpeVoiceTracker
add_executable(pulp-test-ump-buffer-conversion test_ump_buffer_conversion.cpp)
target_link_libraries(pulp-test-ump-buffer-conversion PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ump-buffer-conversion)

# MPE buffer + Processor sidecar tests
add_executable(pulp-test-mpe-buffer test_mpe_buffer.cpp)
target_link_libraries(pulp-test-mpe-buffer PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-mpe-buffer)

# MPE synth voice helpers (voice, allocator, glide detector)
add_executable(pulp-test-mpe-synth-voice test_mpe_synth_voice.cpp)
target_link_libraries(pulp-test-mpe-synth-voice PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-mpe-synth-voice)

# Plugin hosting tests (scanner, signal graph)
add_executable(pulp-test-host test_host.cpp)
target_link_libraries(pulp-test-host PRIVATE pulp::host Catch2::Catch2WithMain)
# Point the integration test at the locally-built PulpGain.clap so the CLAP
# loader runs against a known real plugin.
if(TARGET PulpGain_CLAP)
    target_compile_definitions(pulp-test-host PRIVATE
        PULP_TEST_CLAP_PATH="${CMAKE_BINARY_DIR}/CLAP/PulpGain.clap")
    add_dependencies(pulp-test-host PulpGain_CLAP)
endif()
catch_discover_tests(pulp-test-host)

# WAMv2 + WebCLAP format adapter tests
add_executable(pulp-test-wam-wclap test_wam_wclap.cpp
    ${CMAKE_SOURCE_DIR}/core/format/src/wasm/wam_adapter.cpp
)
target_link_libraries(pulp-test-wam-wclap PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-wam-wclap PRIVATE
    ${CMAKE_SOURCE_DIR}/core/format/include
)
catch_discover_tests(pulp-test-wam-wclap)

# CLAP webview extension tests
add_executable(pulp-test-clap-webview test_clap_webview.cpp
    ${CMAKE_SOURCE_DIR}/core/format/src/wasm/clap_webview.cpp
)
target_link_libraries(pulp-test-clap-webview PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-clap-webview PRIVATE
    ${CMAKE_SOURCE_DIR}/core/format/include
)
catch_discover_tests(pulp-test-clap-webview)

# Web demo plugin tests (PulpPluck, PulpChorus)
add_executable(pulp-test-web-demos test_web_demos.cpp)
target_link_libraries(pulp-test-web-demos PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-web-demos PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/web-demos
)
catch_discover_tests(pulp-test-web-demos)

# AI Designer: token diff tests
add_executable(pulp-test-token-diff test_token_diff.cpp)
target_link_libraries(pulp-test-token-diff PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-token-diff)

# AI Designer: design tool layout/parity tests
add_executable(pulp-test-design-tool-layout test_design_tool_layout.cpp)
target_link_libraries(pulp-test-design-tool-layout PRIVATE pulp::view pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-tool-layout)

# AI Designer: style pack tests
add_executable(pulp-test-style-pack test_style_pack.cpp)
target_link_libraries(pulp-test-style-pack PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-style-pack)

# AI Designer: showcase tests
add_executable(pulp-test-showcase test_showcase.cpp)
target_link_libraries(pulp-test-showcase PRIVATE pulp::view Catch2::Catch2WithMain)
target_include_directories(pulp-test-showcase PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/design
)
catch_discover_tests(pulp-test-showcase)

# WindowManager multi-window framework tests (Phase 6)
add_executable(pulp-test-window-manager test_window_manager.cpp)
target_link_libraries(pulp-test-window-manager PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-window-manager)

# ── Phase 9: Theme management, widgets, asset system ────────────────────────

# Theme contrast utilities (WCAG AA)
add_executable(pulp-test-theme-contrast test_theme_contrast.cpp)
target_link_libraries(pulp-test-theme-contrast PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-theme-contrast)

# Theme preset library (38 tweakcn + derivation)
add_executable(pulp-test-theme-presets test_theme_presets.cpp)
target_link_libraries(pulp-test-theme-presets PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-theme-presets)

# OS appearance tracking
add_executable(pulp-test-appearance test_appearance_tracker.cpp)
target_link_libraries(pulp-test-appearance PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-appearance)

# New widgets (EqCurve, MidiKeyboard, ColorPicker, FileDropZone, SplitView, PropertyList, Breadcrumb)
add_executable(pulp-test-phase9-widgets test_phase9_widgets.cpp)
target_link_libraries(pulp-test-phase9-widgets PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-phase9-widgets)

# Asset manager and resource system
add_executable(pulp-test-asset-manager test_asset_manager.cpp)
target_link_libraries(pulp-test-asset-manager PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-asset-manager)

# Web-compat test suite (Phase 6: CSS parsing, layout, events, visual regression)
add_subdirectory(web-compat)
