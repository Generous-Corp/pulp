# Dawn proc-table installer seam test registrations.
# Included by render_gpu_tests.cmake under PULP_ENABLE_GPU.

    # The installer policy itself, on fresh registry instances. It reaches into
    # core/render/src/ for the internal registry header and needs no GPU.
    add_executable(pulp-test-gpu-proc-installer test_gpu_proc_installer.cpp)
    target_link_libraries(pulp-test-gpu-proc-installer
        PRIVATE pulp::gpu-device-api Catch2::Catch2WithMain)
    target_include_directories(pulp-test-gpu-proc-installer PRIVATE
        ${CMAKE_SOURCE_DIR}/core/render/src)
    catch_discover_tests(pulp-test-gpu-proc-installer)

    # The process-global policy through the real GpuSurface and GpuCompute. One
    # test case per process: the first GPU object seals the policy.
    pulp_add_test_suite(pulp-test-gpu-proc-installer-host-policy
        SOURCES test_gpu_proc_installer_host_policy.cpp
        LIBRARIES pulp::render
        DISCOVERY_ARGS ${PULP_GPU_TEST_DISCOVERY_ARGS})
