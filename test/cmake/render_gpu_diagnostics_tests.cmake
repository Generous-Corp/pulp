# GPU-diagnostics sink and Skia log-bridge registrations.
# Included by test/cmake/render_gpu_tests.cmake; keep related registrations here.

# Creates no GPU device, so it deliberately stays outside the pulp_gpu
# RESOURCE_LOCK that serializes the device-owning suites.
add_executable(pulp-test-gpu-diagnostics test_gpu_diagnostics.cpp)
target_link_libraries(pulp-test-gpu-diagnostics PRIVATE
    pulp::render Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    # The forwarding case calls SkLogHandler::onLog directly, so it needs the
    # Skia headers even though pulp::render carries Skia privately.
    target_compile_definitions(pulp-test-gpu-diagnostics PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-gpu-diagnostics PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
catch_discover_tests(pulp-test-gpu-diagnostics)
