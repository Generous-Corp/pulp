// ScopedFlushDenormals: verify it enables hardware flush-to-zero for its
// lifetime and restores the caller's previous FP mode on exit.

#include <pulp/signal/scoped_flush_denormals.hpp>

#include <catch2/catch_test_macros.hpp>

using pulp::signal::ScopedFlushDenormals;
using pulp::signal::denormals_are_flushed;
using pulp::signal::kHardwareFlushSupported;

TEST_CASE("ScopedFlushDenormals enables and restores the FP mode",
          "[signal][denormal][numeric-mode]") {
    const bool before = denormals_are_flushed();
    {
        ScopedFlushDenormals guard;
        if constexpr (kHardwareFlushSupported) {
            REQUIRE(denormals_are_flushed());
        }
    }
    // The previous mode is always restored, supported platform or not.
    REQUIRE(denormals_are_flushed() == before);
}

TEST_CASE("ScopedFlushDenormals nests and restores to the enclosing mode",
          "[signal][denormal][numeric-mode]") {
    const bool before = denormals_are_flushed();
    {
        ScopedFlushDenormals outer;
        const bool after_outer = denormals_are_flushed();
        {
            ScopedFlushDenormals inner;
            if constexpr (kHardwareFlushSupported) {
                REQUIRE(denormals_are_flushed());
            }
        }
        // Leaving the inner scope restores the outer scope's mode, not "off".
        REQUIRE(denormals_are_flushed() == after_outer);
    }
    REQUIRE(denormals_are_flushed() == before);
}

TEST_CASE("ScopedFlushDenormals flushes a denormal arithmetic result to zero",
          "[signal][denormal][numeric-mode]") {
    if constexpr (!kHardwareFlushSupported) {
        SKIP("hardware flush-to-zero not available on this target");
    } else {
        // volatile defeats constant folding so the multiply happens at runtime
        // under the active FP mode. 1e-30 * 1e-15 ~= 1e-45, below FLT_MIN
        // (~1.18e-38) => a denormal result, flushed to 0 when FTZ/FZ is on.
        volatile float tiny = 1e-30f;
        volatile float scale = 1e-15f;
        {
            ScopedFlushDenormals guard;
            volatile float flushed = tiny * scale;
            REQUIRE(flushed == 0.0f);
        }
        // Without the guard the same product is a (nonzero) denormal — but only
        // when the ambient (restored) mode is gradual underflow. If the test
        // process started with flush-to-zero already enabled, the guard
        // correctly restored that, so skip the gradual-underflow assertion.
        if (!denormals_are_flushed()) {
            volatile float unflushed = tiny * scale;
            REQUIRE(unflushed > 0.0f);
            REQUIRE(unflushed < 1.18e-38f);
        }
    }
}
