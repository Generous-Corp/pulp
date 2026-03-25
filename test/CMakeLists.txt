# Pulp test suite — Catch2

# Build check (no Catch2 dependency)
add_executable(pulp-test-build-check test_build_check.cpp)
target_link_libraries(pulp-test-build-check PRIVATE pulp::platform pulp::runtime)
add_test(NAME build-check COMMAND pulp-test-build-check)

# Signal/DSP tests
add_executable(pulp-test-signal test_signal.cpp)
target_link_libraries(pulp-test-signal PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-signal)

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

# CLAP entry point macro test
if(PULP_HAS_CLAP)
    add_executable(pulp-test-clap-entry test_clap_entry.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
    )
    target_link_libraries(pulp-test-clap-entry PRIVATE pulp::format clap Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-clap-entry)
endif()

# GPU surface tests
if(PULP_ENABLE_GPU)
    add_executable(pulp-test-gpu-surface test_gpu_surface.cpp)
    target_link_libraries(pulp-test-gpu-surface PRIVATE pulp::render Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-gpu-surface)
endif()

# Script engine tests
add_executable(pulp-test-script test_script_engine.cpp)
target_link_libraries(pulp-test-script PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-script)

# Theme/design token tests
add_executable(pulp-test-theme test_theme.cpp)
target_link_libraries(pulp-test-theme PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-theme)

# Screenshot tests
add_executable(pulp-test-screenshot test_screenshot.cpp)
target_link_libraries(pulp-test-screenshot PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-screenshot)

# Canvas tests
add_executable(pulp-test-canvas test_canvas.cpp)
target_link_libraries(pulp-test-canvas PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-canvas)

# View and layout tests
add_executable(pulp-test-view test_view.cpp)
target_link_libraries(pulp-test-view PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-view)

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
target_link_libraries(pulp-test-inspector PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-inspector)

# Widget bridge tests
add_executable(pulp-test-widget-bridge test_widget_bridge.cpp)
target_link_libraries(pulp-test-widget-bridge PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-bridge)

# Golden-file audio tests (uses HeadlessHost + PulpGain)
add_executable(pulp-test-golden test_golden_audio.cpp)
target_link_libraries(pulp-test-golden PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-golden PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain)
catch_discover_tests(pulp-test-golden)
