# GPU fault-continuity test plug-in (WP-h1): a stereo GpuConvolver on the
# shared-I/O Dawn path that records its own input/output and fires one GPU
# fault on request, for tools/testing/daw-smoke/reaper_smoke.py --mode gpu-fault.
# Test-only, so it is built only when PULP_GPU_AUDIO_FAULT_HOOKS is ON, and only
# where the shared-I/O path exists (macOS with Skia-provided Dawn).

include(${CMAKE_CURRENT_LIST_DIR}/cmake/gpu_fault_guards.cmake)
pulp_gpu_fault_refuse_vellum()

if(PULP_BUILD_TESTS)
    # The refusal itself, and its positive control, in a throwaway project.
    foreach(_vellum ON OFF)
        add_test(NAME gpu-fault-continuity-vellum-guard-${_vellum}
            COMMAND ${CMAKE_COMMAND}
                -S ${CMAKE_CURRENT_SOURCE_DIR}/cmake/vellum-guard-test
                -B ${CMAKE_CURRENT_BINARY_DIR}/vellum-guard-${_vellum}
                -DWITH_VELLUM=${_vellum})
    endforeach()
    set_tests_properties(gpu-fault-continuity-vellum-guard-ON PROPERTIES
        PASS_REGULAR_EXPRESSION "refuses to build when Vellum::Gpu")
    set_tests_properties(gpu-fault-continuity-vellum-guard-OFF PROPERTIES
        PASS_REGULAR_EXPRESSION "gpu-fault vellum guard: configured")
endif()

if(NOT (PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO AND TARGET pulp::gpu-audio AND TARGET pulp::render))
    message(STATUS "gpu-fault-continuity: shared-I/O GPU audio unavailable; not built")
    return()
endif()

# GpuConvolver's shared-I/O path is compiled in only by these two options;
# without them SharedRequired refuses and every gpu-fault run is INCONCLUSIVE.
if(NOT (PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF AND PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER))
    message(WARNING "gpu-fault-continuity: configure with -DPULP_GPU_AUDIO_EXACT_PROVIDER_PROOF=ON "
                    "-DPULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER=ON, or the plug-in "
                    "cannot reach the shared-I/O GPU path it exists to test")
endif()

pulp_add_plugin(PulpGpuFaultContinuity
    FORMATS           CLAP VST3 AU
    GPU_UI
    SOURCES           gpu_fault_continuity.cpp gpu_fault_interposer.cpp gpu_fault_view.cpp
                      gpu_fault_rendezvous.cpp
    PLUGIN_NAME       "Pulp GPU Fault Continuity"
    BUNDLE_ID         "com.pulp.gpu-fault-continuity"
    MANUFACTURER      "Pulp"
    VERSION           "0.1.0"
    CATEGORY          Effect
    PLUGIN_CODE       "GpFc"
    MANUFACTURER_CODE "Pulp"
)

# The render_force_loss hook casts the surface's device accessor to a
# wgpu::Device, so it needs the Dawn headers pulp::render keeps private.
function(_pulp_gpu_fault_dawn_headers target)
    # SKIA_INCLUDE_DIRS is a list that already carries Dawn's include root.
    if(PULP_HAS_SKIA AND PULP_SKIA_PROVIDES_DAWN AND SKIA_INCLUDE_DIRS)
        target_include_directories(${target} PRIVATE ${SKIA_INCLUDE_DIRS})
        target_compile_definitions(${target} PRIVATE PULP_GPU_FAULT_HAS_DAWN=1)
    endif()
endfunction()

if(TARGET PulpGpuFaultContinuity_Core)
    target_link_libraries(PulpGpuFaultContinuity_Core PUBLIC
        pulp::gpu-audio pulp::render pulp::view pulp::canvas)
    # The proc-table interposer refuses to compile without this (it injects
    # device loss); only this test-only plug-in and its test define it.
    target_compile_definitions(PulpGpuFaultContinuity_Core PUBLIC PULP_GPU_FAULT_TEST_ONLY=1)
    _pulp_gpu_fault_dawn_headers(PulpGpuFaultContinuity_Core)
endif()

if(PULP_BUILD_TESTS)
    add_executable(gpu-fault-continuity-test
        test_gpu_fault_continuity.cpp gpu_fault_continuity.cpp gpu_fault_interposer.cpp
        gpu_fault_view.cpp gpu_fault_rendezvous.cpp test_gpu_fault_rendezvous.cpp
        ${CMAKE_SOURCE_DIR}/test/harness/rt_allocation_probe.cpp)
    target_link_libraries(gpu-fault-continuity-test PRIVATE
        pulp::format pulp::gpu-audio pulp::render pulp::view pulp::canvas
        Catch2::Catch2WithMain)
    target_include_directories(gpu-fault-continuity-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/test)
    target_compile_definitions(gpu-fault-continuity-test PRIVATE PULP_GPU_FAULT_TEST_ONLY=1)
    _pulp_gpu_fault_dawn_headers(gpu-fault-continuity-test)
    catch_discover_tests(gpu-fault-continuity-test)
endif()
