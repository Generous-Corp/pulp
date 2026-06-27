// SPDX-License-Identifier: MIT
//
// test_threejs_resources.cpp — Three.js IIFE bundle lookup contract.
//
// Verifies the platform contract of
// `pulp::view::threejs_iife_source()`:
//
// - On iOS / iOS Simulator: caller side should get `std::nullopt`
//   when no `.appex` is staged (the unit test runner is not an
//   `.appex`) — this guards against the "missing resource silently
//   returns empty string" failure mode.
// - On non-iOS: always returns `std::nullopt` (desktop lanes don't
//   use the IIFE bundle).
//
// The "loads correctly when staged" assertion runs on iOS only via a
// staged fixture under `test/fixtures/threejs/threejs/three.iife.js`
// (iOS bundles are flat — the subdirectory mirrors the runtime
// `[bundle pathForResource:ofType:inDirectory:@"threejs"]` lookup,
// NOT a macOS `Contents/Resources/` layout). That fixture is created
// on demand; we do NOT ship a pre-staged copy because the generated
// bundle output may differ per upstream Three.js pin.

#include <catch2/catch_test_macros.hpp>

#include "pulp/view/threejs_resources.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

TEST_CASE("threejs_iife_source returns nullopt when no resource is present",
          "[threejs-resources]") {
    // Without staging a fixture, no resource is reachable from the
    // unit-test binary's bundle. On every platform the function must
    // return std::nullopt (rather than throwing, asserting, or
    // returning an empty string we'd then accidentally evaluate()).
    const auto source = pulp::view::threejs_iife_source();
    REQUIRE(!source.has_value());
}

#if !defined(__APPLE__) || !(TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)

TEST_CASE("threejs_iife_source is unconditionally nullopt on non-iOS",
          "[threejs-resources][non-ios]") {
    // Belt-and-suspenders: the stub impl always returns nullopt; this
    // catches a future regression where a desktop adapter
    // accidentally surfaces the iOS IIFE bundle path.
    REQUIRE(!pulp::view::threejs_iife_source().has_value());
}

#endif
