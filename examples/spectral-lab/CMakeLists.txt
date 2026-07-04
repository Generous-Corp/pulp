# Spectral Lab — N-layer GPU spectral "cloud" (freeze + morph) example.
# Live audio path: RT-bounded CPU spectral stack + freeze framer (default), with
# an opt-in GPU engine (core/gpu_audio) that carries big "clouds" — many frozen
# layers — for far less per-block cost than the serial CPU. Native GPU UI: layer
# cloud + output spectrum + controls.

# Spectral-core bench + test (spectral_stack-level): the GPU stack matches the
# CPU reference and wins at high layer counts. Build only when the GPU compute
# lib is present.
if(TARGET pulp::gpu-audio AND TARGET pulp::render)
    add_executable(spectral-lab-bench bench.cpp)
    target_link_libraries(spectral-lab-bench
        PRIVATE pulp::gpu-audio pulp::render pulp::signal)
    target_include_directories(spectral-lab-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(TARGET Catch2::Catch2WithMain)
        add_executable(spectral-lab-test test_spectral_lab.cpp)
        target_link_libraries(spectral-lab-test
            PRIVATE pulp::gpu-audio pulp::render pulp::signal Catch2::Catch2WithMain)
        target_include_directories(spectral-lab-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME spectral-lab-test COMMAND spectral-lab-test)
    endif()
endif()

# Processor-level headless test (CPU freeze/sustain, GPU engine carries the
# audio, live engine switch stays finite at fixed latency). Needs format + the
# spectral runtime; links spectral_lab_view.cpp so the processor's vtable (first
# non-inline virtual is create_view()) resolves at link time.
# NB: gate on pulp::render (the GPU compute lib this test LINKS), not merely
# pulp::gpu-audio — the transport target is always built (GPU-agnostic), so a
# render-absent config (GPU-off build, or a find_package consumer where only the
# capital Pulp::view exists) would otherwise satisfy the guard and then fail to
# generate on the missing pulp::render link. Mirrors the pulp::render gate on the
# spectral-lab-bench/-test block above.
if(PULP_BUILD_TESTS AND TARGET pulp::gpu-audio AND TARGET pulp::render AND
   (TARGET pulp::view OR TARGET Pulp::view) AND TARGET Catch2::Catch2WithMain)
    add_executable(spectral-lab-processor-test
        test_spectral_lab_processor.cpp spectral_lab_view.cpp)
    target_link_libraries(spectral-lab-processor-test
        PRIVATE pulp::format pulp::signal pulp::gpu-audio pulp::render
                pulp::view pulp::canvas Catch2::Catch2WithMain)
    target_include_directories(spectral-lab-processor-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(spectral-lab-processor-test)
endif()

# Headless editor screenshot. macOS-only dev tool (Skia raster capture); would
# otherwise pull the Linux windowing stack into the advisory Linux lane.
# Gate on pulp::render (linked below), not just the always-built pulp::gpu-audio.
if(APPLE AND TARGET pulp::gpu-audio AND TARGET pulp::render AND
   (TARGET pulp::view OR TARGET Pulp::view))
    add_executable(spectral-lab-shot screenshot.cpp spectral_lab_view.cpp)
    target_link_libraries(spectral-lab-shot PRIVATE
        pulp::format pulp::audio pulp::signal pulp::gpu-audio pulp::render
        pulp::view pulp::canvas)
    target_include_directories(spectral-lab-shot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# The plugin (VST3, AU v2, CLAP, Standalone). Only when the GPU spectral runtime
# is available — the processor is built on core/gpu_audio and SpectralLab_Core
# links pulp::render, so gate on pulp::render (not just the always-built
# pulp::gpu-audio transport) or a render-absent config fails to generate.
if(TARGET pulp::gpu-audio AND TARGET pulp::render)
    pulp_add_plugin(SpectralLab
        FORMATS           VST3 AU CLAP Standalone
        SOURCES           spectral_lab_view.cpp
        PLUGIN_NAME       "SpectralLab"
        BUNDLE_ID         "com.pulp.spectrallab"
        MANUFACTURER      "Pulp"
        VERSION           "1.0.2"
        CATEGORY          Effect
        PLUGIN_CODE       "SpLb"
        MANUFACTURER_CODE "Pulp"
    )

    # The processor's create_view() pulls in the view + canvas stack, and the
    # spectral runtime.
    if(TARGET SpectralLab_Core)
        target_include_directories(SpectralLab_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(SpectralLab_Core PUBLIC
            pulp::gpu-audio pulp::render pulp::view pulp::canvas)
    endif()

    # Make the GPU bundles self-contained (the @loader_path fix) AND fail the
    # build if any bundle isn't relocatable — the dylib-rpath footgun that ships a
    # build that only works on the build machine. See PulpBundleRelocatable.cmake.
    include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpBundleRelocatable.cmake)
    foreach(_sl_target SpectralLab_CLAP SpectralLab_VST3 SpectralLab_AU SpectralLab_Standalone)
        if(TARGET ${_sl_target})
            pulp_make_bundle_relocatable(${_sl_target})
            pulp_validate_bundle_relocatable(${_sl_target})
        endif()
    endforeach()

    # CLAP load smoke test — the plugin's shared object must dlopen cleanly.
    if(PULP_HAS_CLAP AND APPLE)
        add_test(NAME clap-dlopen-SpectralLab
            COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/SpectralLab.clap/Contents/MacOS/SpectralLab'); print('OK')\"")
        set_tests_properties(clap-dlopen-SpectralLab PROPERTIES LABELS "validation;clap" TIMEOUT 10)
    endif()

    # AU validation via auval. killall AudioComponentRegistrar forces a rescan of
    # the freshly-copied bundle (stale-cache trap codified in the auv2 skill).
    if(APPLE AND PULP_HAS_AUSDK)
        add_test(NAME auval-SpectralLab
            COMMAND bash -c "d=\"$HOME/Library/Audio/Plug-Ins/Components/SpectralLab.auvaltest.component\"; rm -rf \"$d\"; cp -R \"${CMAKE_BINARY_DIR}/AU/SpectralLab.component\" \"$d\" && { killall -KILL AudioComponentRegistrar 2>/dev/null || true; sleep 1; auval -v aufx SpLb Pulp 2>&1 | tee /dev/stderr | grep -q 'PASS'; }; rc=$?; rm -rf \"$d\"; exit $rc")
        set_tests_properties(auval-SpectralLab PROPERTIES
            LABELS "validation;au"
            TIMEOUT 30
            ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
    endif()

    if(PULP_HAS_VST3)
        find_program(PLUGINVAL pluginval)
        if(PLUGINVAL)
            add_test(NAME pluginval-SpectralLab-VST3
                COMMAND ${PLUGINVAL} --validate ${CMAKE_BINARY_DIR}/VST3/SpectralLab.vst3 --strictness-level 5 --timeout-ms 10000)
            set_tests_properties(pluginval-SpectralLab-VST3 PROPERTIES
                LABELS "validation;vst3"
                TIMEOUT 30
                ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
        endif()
    endif()
endif()
