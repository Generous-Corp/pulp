# Optional benchmark test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

# ── Yoga layout-pass benchmark ──────────────────────────────────────────────
#
# Measures the per-frame cost of View::layout_children() -> yoga_layout(),
# which rebuilds and destroys the whole YGNode tree on every call. Runs by
# default (it is a few seconds) because it doubles as a timing + allocation
# regression gate on the pre-paint layout path; filter it with
# `ctest -LE bench` or run it alone with `ctest -R "Yoga layout"`.
pulp_add_test_suite(pulp-test-yoga-layout-bench
    LIBRARIES pulp::view
    LABELS "bench"
    TIMEOUT 120)

# ── Zero-copy benchmark ─────────────────────────────────────────────────────
#
# Tooling-only tests, gated on PULP_BENCHMARK. The perf-counter unit
# test links pulp::render, which is only added to the build when
# PULP_ENABLE_GPU=ON (see root CMakeLists.txt). Gating the test on the
# render target's presence lets -DPULP_BENCHMARK=ON -DPULP_ENABLE_GPU=OFF
# configure cleanly without a missing-target error. The integration
# test shell-outs to pulp-ui-preview which is currently
# Apple-desktop-only (same guard as the target).
# Benchmark coverage remains opt-in so normal test builds stay cheap.
if(PULP_BENCHMARK AND TARGET pulp-render)
    add_executable(pulp-test-bench-perf-counters test_bench_perf_counters.cpp)
    target_link_libraries(pulp-test-bench-perf-counters PRIVATE
        pulp::render Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-bench-perf-counters)

    if(APPLE AND NOT PULP_IOS)
        add_executable(pulp-test-bench-integration test_bench_integration.cpp)
        target_link_libraries(pulp-test-bench-integration PRIVATE
            pulp::platform Catch2::Catch2WithMain)
        if(TARGET pulp-ui-preview)
            add_dependencies(pulp-test-bench-integration pulp-ui-preview)
        endif()
        catch_discover_tests(pulp-test-bench-integration)
    endif()
endif()

# ── Oscillator throughput benchmark ─────────────────────────────────────────
#
# Tooling-only, gated on PULP_BENCHMARK like the zero-copy benchmark above,
# but deliberately does NOT require `pulp-render`/PULP_ENABLE_GPU: oscillator
# `next()` throughput has nothing to do with the GPU pipeline, so this target
# stays buildable with just `-DPULP_BENCHMARK=ON` (pulp::signal is
# header-only and always present). ${choc_SOURCE_DIR} is named directly
# rather than linking a Pulp target that re-exports it, mirroring
# test/cmake/design_import_tool_cli_tests.cmake's pulp-test-import-design-tool.
if(PULP_BENCHMARK)
    pulp_add_test_suite(pulp-test-osc-bench
        LIBRARIES pulp::signal
        INCLUDE_DIRS ${choc_SOURCE_DIR}
        LABELS "bench")
endif()
