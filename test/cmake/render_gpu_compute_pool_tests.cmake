# Render GPU compute pool test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

    # GPU compute staging-buffer pool tests. Reaches into
    # core/render/src/ for the internal pool header — the pool
    # is an implementation detail, not a public API, so the test directly
    # includes the source header rather than going through a public link.
    add_executable(pulp-test-gpu-compute-pool test_gpu_compute_pool.cpp)
    target_link_libraries(pulp-test-gpu-compute-pool
        PRIVATE pulp::render Catch2::Catch2WithMain)
    # Pool tests need Dawn headers directly (webgpu/webgpu_cpp.h,
    # dawn/native/DawnNative.h). pulp::render pulls these in privately,
    # so we mirror the include paths here. `SKIA_INCLUDE_DIRS` is set
    # by FindSkia.cmake; the `/dawn` suffix targets the generated-
    # header directory where `webgpu/webgpu_cpp.h` lives.
    if(PULP_HAS_SKIA)
        target_compile_definitions(pulp-test-gpu-compute-pool
            PRIVATE PULP_HAS_SKIA=1)
        target_include_directories(pulp-test-gpu-compute-pool PRIVATE
            ${SKIA_INCLUDE_DIRS})
        if(EXISTS "${SKIA_INCLUDE_DIRS}/dawn")
            target_include_directories(pulp-test-gpu-compute-pool PRIVATE
                "${SKIA_INCLUDE_DIRS}/dawn")
        endif()
    endif()
    catch_discover_tests(pulp-test-gpu-compute-pool ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # Asynchronous GPU readback: equivalence with the blocking path, in-order
    # delivery of concurrent in-flight requests, bounded deadline expiry, and the
    # audio-thread contract through GpuAudioTransport. Public API only (no Dawn
    # headers); the cases SKIP when no GPU compute device is available.
    pulp_add_test_suite(pulp-test-gpu-compute-async
        SOURCES test_gpu_compute_async.cpp harness/rt_allocation_probe.cpp
        LIBRARIES pulp::render pulp::gpu-audio pulp::audio
        DISCOVERY_ARGS ${PULP_GPU_TEST_DISCOVERY_ARGS})
