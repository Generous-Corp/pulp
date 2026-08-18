// Proves the sanitizer guard points the right way.
//
// The guard decides whether the trusted-host launch tests run at all, so an
// inverted one is silent in both directions: flip it and the sanitizer lane goes
// red again exactly as before, while every other lane quietly stops exercising
// the launcher. Neither shows up as a failure of the guard itself.
//
// This asserts the direction that is true for the build it is compiled into, so
// the sanitizer lane proves the skip fires and every other lane proves it does
// not. One test, both directions, no sanitizer build needed to cover the common
// case.

#include "support/control_runtime_closure_sanitizer.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the runtime-closure sanitizer guard matches this build",
          "[inspect][control][security][sanitizer-guard]") {
    const bool skipped = pulp::test::skip_when_sanitizer_perturbs_runtime_closure();
#if defined(PULP_TEST_WITH_SANITIZER)
    // A sanitizer runtime is loaded into every child, so the pinned-closure
    // premise cannot hold and the guard must stop the caller.
    REQUIRE(skipped);
#else
    // No sanitizer: the launch tests must actually run. This is the assertion
    // that catches a guard which skips everywhere and reports green.
    REQUIRE_FALSE(skipped);
#endif
}
