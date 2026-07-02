# GPU NAM — fused block-parallel WaveNet neural-amp inference on the GPU.
#
# Per-architecture build toggles for the reusable NAM inference substrate
# (nam_runtime.hpp + the per-arch headers). All default ON; turn one OFF to build
# a smaller binary that supports only the subset you need — e.g.
# `-DGPU_NAM_WITH_LSTM=OFF`. The toggles pass GPU_NAM_WITH_<ARCH>=0 to the
# substrate + its tests; a define is emitted only for a DISABLED arch, so the
# default all-on build is byte-for-byte unchanged. (The full GPU plugin ships all
# architectures; the WaveNet GPU engine is WaveNet-specific, so a subset build
# that drops WaveNet targets the CPU inference layer, not the GPU plugin.)
option(GPU_NAM_WITH_WAVENET "GPU NAM: build WaveNet (A1) architecture" ON)
option(GPU_NAM_WITH_A2      "GPU NAM: build WaveNet A2 architecture" ON)
option(GPU_NAM_WITH_CONVNET "GPU NAM: build ConvNet architecture" ON)
option(GPU_NAM_WITH_LSTM    "GPU NAM: build LSTM architecture" ON)
option(GPU_NAM_WITH_LINEAR  "GPU NAM: build Linear architecture" ON)
option(GPU_NAM_WITH_KERAS   "GPU NAM: build RTNeural/Keras (.json) GRU/LSTM engine" ON)
set(_gpu_nam_arch_defs "")
foreach(_gn_arch WAVENET A2 CONVNET LSTM LINEAR KERAS)
    if(NOT GPU_NAM_WITH_${_gn_arch})
        list(APPEND _gpu_nam_arch_defs "GPU_NAM_WITH_${_gn_arch}=0")
    endif()
endforeach()
# Apply the toggles directory-wide so EVERY gpu-nam target that compiles the
# inference substrate — the plugin core, its format adapters, and the tests —
# sees a consistent GPU_NAM_WITH_<ARCH> set (ODR-safe: they all share one
# NamRuntime layout). A define is emitted only for a DISABLED arch, so the
# default all-on build passes nothing and is unchanged. The full GPU plugin
# additionally requires WaveNet (its GPU engine is WaveNet-specific) and
# #errors at compile time if WaveNet is turned off.
if(_gpu_nam_arch_defs)
    add_compile_definitions(${_gpu_nam_arch_defs})
endif()

# Optional licensing gate (default OFF) — a reference for paid plugins. The shipped
# MIT demo leaves it OFF, so nothing is restricted. Turn it ON and supply a public
# key to gate the opt-in GPU acceleration behind a valid license (the free tier
# falls back to the bit-exact CPU engine).
option(GPU_NAM_WITH_LICENSE "GPU NAM: gate the GPU engine behind a license (reference)" OFF)
set(GPU_NAM_LICENSE_PUBKEY "" CACHE STRING "PEM RSA public key for the GPU NAM license gate")
if(GPU_NAM_WITH_LICENSE)
    add_compile_definitions(GPU_NAM_WITH_LICENSE=1
        "GPU_NAM_LICENSE_PUBKEY=\"${GPU_NAM_LICENSE_PUBKEY}\"")
endif()

if(TARGET pulp::render)
    add_executable(gpu-nam-check wavenet_check.cpp)
    target_link_libraries(gpu-nam-check PRIVATE pulp::render)
    target_include_directories(gpu-nam-check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(TARGET Catch2::Catch2WithMain)
        add_executable(gpu-nam-test test_gpu_nam.cpp)
        target_link_libraries(gpu-nam-test PRIVATE pulp::render Catch2::Catch2WithMain)
        target_include_directories(gpu-nam-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME gpu-nam-test COMMAND gpu-nam-test)

        # Fused GPU NAM forward validated against the exact CPU oracle on the real
        # .nam capture. Needs both pulp::render (GPU primitive) and pulp::runtime
        # (choc::json, used by the .nam loader / oracle).
        if(TARGET pulp::runtime)
            add_executable(gpu-nam-gpu-test test_gpu_nam_real.cpp)
            target_link_libraries(gpu-nam-gpu-test
                PRIVATE pulp::render pulp::runtime Catch2::Catch2WithMain)
            target_include_directories(gpu-nam-gpu-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            add_test(NAME gpu-nam-gpu-test COMMAND gpu-nam-gpu-test)
        endif()
    endif()
endif()

# Exact, reference-faithful NAM WaveNet CPU inference + .nam loader. This is the
# oracle the GPU primitive is validated against; it only needs choc::json (via
# pulp::runtime), not the GPU/render stack, so it builds independently of pulp::render.
if(TARGET pulp::runtime)
    add_executable(gpu-nam-model-check nam_model_check.cpp)
    target_link_libraries(gpu-nam-model-check PRIVATE pulp::runtime)
    target_include_directories(gpu-nam-model-check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(TARGET Catch2::Catch2WithMain)
        # Each architecture's test (loader + NamRuntime dispatch) is built only
        # when that architecture is enabled — a disabled arch's runtime dispatch
        # correctly rejects the model, which would fail the test by design.
        if(GPU_NAM_WITH_WAVENET)
            add_executable(gpu-nam-model-test test_nam_model.cpp)
            target_link_libraries(gpu-nam-model-test PRIVATE pulp::runtime Catch2::Catch2WithMain)
            target_include_directories(gpu-nam-model-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            add_test(NAME gpu-nam-model-test COMMAND gpu-nam-model-test)
        endif()

        # WaveNet A2 architecture (SlimmableContainer + windowed head) + dispatch.
        if(GPU_NAM_WITH_A2)
            add_executable(gpu-nam-a2-test test_nam_a2.cpp)
            target_link_libraries(gpu-nam-a2-test PRIVATE pulp::runtime Catch2::Catch2WithMain)
            target_include_directories(gpu-nam-a2-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            add_test(NAME gpu-nam-a2-test COMMAND gpu-nam-a2-test)
        endif()

        # ConvNet architecture (dilated conv blocks + batchnorm) + dispatch.
        if(GPU_NAM_WITH_CONVNET)
            add_executable(gpu-nam-convnet-test test_nam_convnet.cpp)
            target_link_libraries(gpu-nam-convnet-test PRIVATE pulp::runtime Catch2::Catch2WithMain)
            target_include_directories(gpu-nam-convnet-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            add_test(NAME gpu-nam-convnet-test COMMAND gpu-nam-convnet-test)
        endif()

        # LSTM architecture + NamRuntime dispatch (no GPU needed).
        if(GPU_NAM_WITH_LSTM)
            add_executable(gpu-nam-lstm-test test_nam_lstm.cpp)
            target_link_libraries(gpu-nam-lstm-test PRIVATE pulp::runtime Catch2::Catch2WithMain)
            target_include_directories(gpu-nam-lstm-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            add_test(NAME gpu-nam-lstm-test COMMAND gpu-nam-lstm-test)
        endif()

        # Linear architecture (single causal FIR) + NamRuntime dispatch (no GPU).
        if(GPU_NAM_WITH_LINEAR)
            add_executable(gpu-nam-linear-test test_nam_linear.cpp)
            target_link_libraries(gpu-nam-linear-test PRIVATE pulp::runtime Catch2::Catch2WithMain)
            target_include_directories(gpu-nam-linear-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            add_test(NAME gpu-nam-linear-test COMMAND gpu-nam-linear-test)
        endif()

        # RTNeural/Keras (.json) GRU + LSTM + Dense engine. A separate KerasRuntime
        # from the .nam NamRuntime (different file format + recurrent-only weight
        # layout); recurrent, so CPU-only. Needs only choc::json (pulp::runtime).
        if(GPU_NAM_WITH_KERAS)
            add_executable(gpu-nam-keras-test test_keras_runtime.cpp)
            target_link_libraries(gpu-nam-keras-test PRIVATE pulp::runtime Catch2::Catch2WithMain)
            target_include_directories(gpu-nam-keras-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            add_test(NAME gpu-nam-keras-test COMMAND gpu-nam-keras-test)
        endif()

        # Optional licensing gate (free-tier default, locks on an invalid license).
        add_executable(gpu-nam-license-test test_gpu_nam_license.cpp)
        target_link_libraries(gpu-nam-license-test PRIVATE pulp::runtime Catch2::Catch2WithMain)
        target_include_directories(gpu-nam-license-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME gpu-nam-license-test COMMAND gpu-nam-license-test)

        # Grace-period engine retirement (audio-thread-safe live swaps; no GPU).
        add_executable(gpu-nam-retire-test test_nam_retire_list.cpp)
        target_link_libraries(gpu-nam-retire-test PRIVATE Catch2::Catch2WithMain)
        target_include_directories(gpu-nam-retire-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME gpu-nam-retire-test COMMAND gpu-nam-retire-test)
    endif()
endif()

# ── GPU NAM plugin (VST3, AU v2, CLAP, Standalone) ──────────────────────────
# A Neural Amp Modeler player: loads an open-source .nam WaveNet capture and runs
# it through the inline CPU oracle (default) or the opt-in GPU audio runtime
# (GpuNamCloudNode on a GpuAudioTransport). Native GPU UI: input→output transfer
# curve + output spectrum + gain/mix/engine controls. Requires the GPU compute
# primitive (pulp::render) and the GPU audio runtime (pulp::gpu-audio).
if(TARGET pulp::render AND TARGET pulp::gpu-audio)
    set(GPU_NAM_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/models/example.nam")
    set(GPU_NAM_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/assets/nam")

    pulp_add_plugin(GpuNam
        FORMATS           VST3 AU CLAP Standalone
        SOURCES           gpu_nam_view.cpp
        PLUGIN_NAME       "GpuNam"
        BUNDLE_ID         "com.pulp.gpunam"
        MANUFACTURER      "Pulp"
        VERSION           "1.1.0"
        CATEGORY          Effect
        PLUGIN_CODE       "GNam"
        MANUFACTURER_CODE "Pulp"
    )

    # The processor's create_view() + GPU engine pull in the render / gpu-audio /
    # view / canvas / signal stack. Shipped plugin/standalone binaries find their
    # default model via the executable-relative bundled Resources copy below — they
    # deliberately do NOT bake the source-tree path (so they stay portable). The
    # source-tree fallback define is set only on the dev tools (test + shot), which
    # run from the build tree without a bundle.
    if(TARGET GpuNam_Core)
        target_include_directories(GpuNam_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(GpuNam_Core PUBLIC
            pulp::render pulp::gpu-audio pulp::signal pulp::view pulp::canvas pulp::runtime)
        # Source-tree asset fallback for dev/standalone runs from the build tree;
        # shipped bundles resolve the executable-relative Resources/assets/nam copy.
        target_compile_definitions(GpuNam_Core PRIVATE
            GPU_NAM_ASSET_DIR="${GPU_NAM_ASSETS}")
    endif()

    # Bundle the default model into each format's Resources so the plugin finds it
    # at runtime next to the binary (the build-define is the source-tree fallback).
    foreach(_gn_target GpuNam_CLAP GpuNam_VST3 GpuNam_AU GpuNam_Standalone)
        if(TARGET ${_gn_target})
            if(APPLE)
                add_custom_command(TARGET ${_gn_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_BUNDLE_DIR:${_gn_target}>/Contents/Resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${GPU_NAM_MODEL}"
                        "$<TARGET_BUNDLE_DIR:${_gn_target}>/Contents/Resources/example.nam"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${GPU_NAM_ASSETS}"
                        "$<TARGET_BUNDLE_DIR:${_gn_target}>/Contents/Resources/assets/nam"
                    COMMENT "Bundling example.nam + UI assets into ${_gn_target}"
                    VERBATIM)
            else()
                add_custom_command(TARGET ${_gn_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${GPU_NAM_MODEL}"
                        "$<TARGET_FILE_DIR:${_gn_target}>/example.nam"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${GPU_NAM_ASSETS}"
                        "$<TARGET_FILE_DIR:${_gn_target}>/assets/nam"
                    COMMENT "Copying example.nam + UI assets next to ${_gn_target}"
                    VERBATIM)
            endif()
        endif()
    endforeach()

    # Self-contained, relocatable GPU bundles (the @loader_path fix) — fail the
    # build if any bundle bakes a build-tree dylib path.
    include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpBundleRelocatable.cmake)
    foreach(_gn_target GpuNam_CLAP GpuNam_VST3 GpuNam_AU GpuNam_Standalone)
        if(TARGET ${_gn_target})
            pulp_make_bundle_relocatable(${_gn_target})
            pulp_validate_bundle_relocatable(${_gn_target})
        endif()
    endforeach()

    # Headless editor screenshot. macOS-only dev tool (Skia raster capture).
    if(APPLE AND (TARGET pulp::view OR TARGET Pulp::view))
        add_executable(gpu-nam-shot screenshot.cpp gpu_nam_view.cpp)
        target_link_libraries(gpu-nam-shot PRIVATE
            pulp::format pulp::audio pulp::render pulp::gpu-audio pulp::signal
            pulp::view pulp::canvas pulp::runtime)
        target_include_directories(gpu-nam-shot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(gpu-nam-shot PRIVATE
            GPU_NAM_DEFAULT_MODEL_PATH="${GPU_NAM_MODEL}"
            GPU_NAM_ASSET_DIR="${GPU_NAM_ASSETS}")
    endif()

    # Performance harness (CPU oracle vs GPU engine, realtime ratio across block
    # sizes). A dev tool like gpu-nam-shot; not built into the shipped plugin.
    add_executable(gpu-nam-bench bench.cpp gpu_nam_view.cpp)
    target_link_libraries(gpu-nam-bench PRIVATE
        pulp::format pulp::render pulp::gpu-audio pulp::signal
        pulp::view pulp::canvas pulp::runtime)
    target_include_directories(gpu-nam-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(gpu-nam-bench PRIVATE
        GPU_NAM_DEFAULT_MODEL_PATH="${GPU_NAM_MODEL}"
        GPU_NAM_ASSET_DIR="${GPU_NAM_ASSETS}")

    # Processor tests (CPU + GPU engine, live switch, load_model). Links
    # gpu_nam_view.cpp so the processor's vtable (first non-inline virtual is
    # create_view()) resolves at link time; the view is never CALLED headless.
    if(PULP_BUILD_TESTS AND TARGET Catch2::Catch2WithMain)
        add_executable(gpu-nam-plugin-test test_gpu_nam_plugin.cpp gpu_nam_view.cpp)
        target_link_libraries(gpu-nam-plugin-test PRIVATE
            pulp::format pulp::render pulp::gpu-audio pulp::signal
            pulp::view pulp::canvas pulp::runtime Catch2::Catch2WithMain)
        target_include_directories(gpu-nam-plugin-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(gpu-nam-plugin-test PRIVATE
            GPU_NAM_DEFAULT_MODEL_PATH="${GPU_NAM_MODEL}"
            GPU_NAM_ASSET_DIR="${GPU_NAM_ASSETS}")
        catch_discover_tests(gpu-nam-plugin-test)

        # Editor tests: faithful UI renders non-blank + composited (Skia), and
        # pointer input (toggles, knob drag) drives real parameters.
        if(APPLE AND (TARGET pulp::view OR TARGET Pulp::view))
            add_executable(gpu-nam-ui-test test_gpu_nam_ui.cpp gpu_nam_view.cpp)
            target_link_libraries(gpu-nam-ui-test PRIVATE
                pulp::format pulp::render pulp::gpu-audio pulp::signal
                pulp::view pulp::canvas pulp::runtime Catch2::Catch2WithMain)
            target_include_directories(gpu-nam-ui-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            target_compile_definitions(gpu-nam-ui-test PRIVATE
                GPU_NAM_DEFAULT_MODEL_PATH="${GPU_NAM_MODEL}"
                GPU_NAM_ASSET_DIR="${GPU_NAM_ASSETS}")
            catch_discover_tests(gpu-nam-ui-test)
        endif()
    endif()

    # CLAP load smoke test — the plugin's shared object must dlopen cleanly.
    if(PULP_HAS_CLAP AND APPLE)
        add_test(NAME clap-dlopen-GpuNam
            COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/GpuNam.clap/Contents/MacOS/GpuNam'); print('OK')\"")
        set_tests_properties(clap-dlopen-GpuNam PROPERTIES LABELS "validation;clap" TIMEOUT 20)
    endif()

    # AU validation via auval. killall AudioComponentRegistrar forces a rescan of
    # the freshly-copied bundle (stale-cache trap codified in the auv2 skill).
    if(APPLE AND PULP_HAS_AUSDK)
        add_test(NAME auval-GpuNam
            COMMAND bash -c "d=\"$HOME/Library/Audio/Plug-Ins/Components/GpuNam.auvaltest.component\"; rm -rf \"$d\"; cp -R \"${CMAKE_BINARY_DIR}/AU/GpuNam.component\" \"$d\" && { killall -KILL AudioComponentRegistrar 2>/dev/null || true; sleep 1; auval -v aufx GNam Pulp 2>&1 | tee /dev/stderr | grep -q 'PASS'; }; rc=$?; rm -rf \"$d\"; exit $rc")
        set_tests_properties(auval-GpuNam PROPERTIES
            LABELS "validation;au"
            TIMEOUT 60
            ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
    endif()

    if(PULP_HAS_VST3)
        find_program(PLUGINVAL pluginval)
        if(PLUGINVAL)
            add_test(NAME pluginval-GpuNam-VST3
                COMMAND ${PLUGINVAL} --validate "${CMAKE_BINARY_DIR}/VST3/GpuNam.vst3" --strictness-level 5 --timeout-ms 15000)
            set_tests_properties(pluginval-GpuNam-VST3 PROPERTIES
                LABELS "validation;vst3"
                TIMEOUT 40
                ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
        endif()
    endif()
endif()
