# SuperConvolver — convolution reverb / impulse processor.
# Live audio path: RT-safe CPU convolution (signal::PartitionedConvolver) with
# lock-free runtime IR swap. Native GPU UI: live IR waveform + frequency
# display + controls. The GPU convolution engine (core/gpu_audio) is the
# accelerator for very long IRs / many instances.

# Headless audio-validation test (golden: impulse -> IR, live Size swap,
# dry/bypass null).
if(PULP_BUILD_TESTS)
    # Links super_convolver_view.cpp so the processor's vtable (first non-inline
    # virtual is create_view()) resolves. create_view() is never CALLED in the
    # headless audio test — no GPU host is needed at runtime, only the symbol at
    # link time — so the view/canvas libs are a link-only dependency here.
    add_executable(super-convolver-test test_super_convolver.cpp super_convolver_view.cpp)
    target_link_libraries(super-convolver-test
        PRIVATE pulp::format pulp::signal pulp::gpu-audio pulp::view pulp::canvas
                Catch2::Catch2WithMain)
    target_include_directories(super-convolver-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(super-convolver-test)
endif()

# Headless editor screenshot. macOS-only dev tool (Skia raster capture); would
# otherwise pull the Linux windowing stack into the advisory Linux lane.
if(APPLE AND (TARGET pulp::view OR TARGET Pulp::view))
    add_executable(super-convolver-shot screenshot.cpp super_convolver_view.cpp)
    target_link_libraries(super-convolver-shot PRIVATE
        pulp::format pulp::audio pulp::signal pulp::gpu-audio pulp::view pulp::canvas)
    target_include_directories(super-convolver-shot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Honest CPU-vs-GPU convolution benchmark. macOS + GPU only (it instantiates the
# real GpuCompute device); gated like the screenshot target so the advisory Linux
# lane never tries to pull in the windowing/GPU stack. Not registered as a CTest —
# it is a measurement tool, run by hand: ./build/.../super-convolver-bench.
if(APPLE AND TARGET pulp::gpu-audio AND PULP_ENABLE_GPU)
    add_executable(super-convolver-bench bench.cpp)
    target_link_libraries(super-convolver-bench
        PRIVATE pulp::format pulp::signal pulp::gpu-audio)
    target_include_directories(super-convolver-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # GPU-only load generator: proves at the hardware level that the convolution
    # math runs on the GPU (back-to-back submits → measurable GPU utilization).
    add_executable(super-convolver-gpu-hammer gpu_hammer.cpp)
    target_link_libraries(super-convolver-gpu-hammer
        PRIVATE pulp::render pulp::signal)
    target_include_directories(super-convolver-gpu-hammer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Plugin (VST3, AU v2, CLAP, Standalone)
pulp_add_plugin(SuperConvolver
    FORMATS           VST3 AU CLAP Standalone
    SOURCES           super_convolver_view.cpp
    PLUGIN_NAME       "SuperConvolver"
    BUNDLE_ID         "com.pulp.superconvolver"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.6"
    CATEGORY          Effect
    PLUGIN_CODE       "SpCv"
    MANUFACTURER_CODE "Pulp"
)

# The processor's create_view() pulls in the view + canvas stack.
if(TARGET SuperConvolver_Core)
    target_include_directories(SuperConvolver_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(SuperConvolver_Core PUBLIC pulp::gpu-audio pulp::view pulp::canvas)
endif()

# Make the GPU bundles self-contained (the @loader_path fix) AND fail the build
# if any bundle isn't relocatable — the dylib-rpath footgun that ships a build
# that only works on the build machine. See PulpBundleRelocatable.cmake.
include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpBundleRelocatable.cmake)
foreach(_sc_target SuperConvolver_CLAP SuperConvolver_VST3 SuperConvolver_AU SuperConvolver_Standalone)
    if(TARGET ${_sc_target})
        pulp_make_bundle_relocatable(${_sc_target})
        pulp_validate_bundle_relocatable(${_sc_target})
    endif()
endforeach()

# CLAP load smoke test — the plugin's shared object must dlopen cleanly.
if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-SuperConvolver
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/SuperConvolver.clap/Contents/MacOS/SuperConvolver'); print('OK')\"")
    set_tests_properties(clap-dlopen-SuperConvolver PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()

# AU validation via auval. killall AudioComponentRegistrar forces a rescan of
# the freshly-copied bundle (stale-cache trap codified in the auv2 skill).
if(APPLE AND PULP_HAS_AUSDK)
    add_test(NAME auval-SuperConvolver
        COMMAND bash -c "d=\"$HOME/Library/Audio/Plug-Ins/Components/SuperConvolver.auvaltest.component\"; rm -rf \"$d\"; cp -R \"${CMAKE_BINARY_DIR}/AU/SuperConvolver.component\" \"$d\" && { killall -KILL AudioComponentRegistrar 2>/dev/null || true; sleep 1; auval -v aufx SpCv Pulp 2>&1 | tee /dev/stderr | grep -q 'PASS'; }; rc=$?; rm -rf \"$d\"; exit $rc")
    set_tests_properties(auval-SuperConvolver PROPERTIES
        LABELS "validation;au"
        TIMEOUT 30
        ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
endif()

if(PULP_HAS_VST3)
    find_program(PLUGINVAL pluginval)
    if(PLUGINVAL)
        add_test(NAME pluginval-SuperConvolver-VST3
            COMMAND ${PLUGINVAL} --validate ${CMAKE_BINARY_DIR}/VST3/SuperConvolver.vst3 --strictness-level 5 --timeout-ms 10000)
        set_tests_properties(pluginval-SuperConvolver-VST3 PROPERTIES
            LABELS "validation;vst3"
            TIMEOUT 30
            ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
    endif()
endif()
