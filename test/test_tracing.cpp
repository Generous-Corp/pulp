// Tracing subsystem guard tests.
//
// The load-bearing check: a default configure (and therefore every shipping
// build and the normal CI test build) has tracing OFF. Perfetto tracing is
// dev-only — an 80MB ring writing a .pftrace inside a customer DAW is a support
// incident — so PULP_TRACING must never be left on. This suite runs in the
// default configuration, so it passes normally and fails loudly if a build
// leaks PULP_TRACING=ON. It complements the configure-time guarantee in
// tools/cmake/PulpTracing.cmake and the nm scan in AssertNoTracingSymbols.cmake.

#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/trace.hpp>

TEST_CASE("tracing is off by default", "[tracing][rt-safety]") {
    REQUIRE_FALSE(pulp::runtime::kTracingEnabled);
}

TEST_CASE("tracing macros compile as no-ops", "[tracing]") {
    // The macro surface must compile everywhere, expanding to nothing when OFF.
    // These are UI/render/offline categories only — never the live audio path.
    PULP_TRACE_SCOPE("render");
    PULP_TRACE_SCOPE_NAMED("gpu", "submit");
    PULP_TRACE_BEGIN("layout", "reflow");
    PULP_TRACE_END("layout");
    PULP_TRACE_COUNTER("io", "bytes", 4096);
    SUCCEED("trace macros expand without side effects");
}
