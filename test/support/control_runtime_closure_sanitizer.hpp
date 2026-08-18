#pragma once

// Trusted-host launch tests cannot hold their premise in a sanitizer build.
//
// `ControlTrustedHostSnapshot::loaded_runtime_closure_matches_policy()` walks the
// launched child's executable memory regions and rejects any mapped image that is
// neither an Apple platform image (`/System/Library/`, `/usr/lib/`, verified
// Rosetta) nor one of the inventory's pinned files. That is the security boundary
// these tests exist to prove: an unexpected dylib inside a trusted host IS a
// policy violation, and the check is right to fail closed on one.
//
// A sanitizer build injects exactly such a dylib into every process it produces.
// Measured on macOS, `clang++ -fsanitize=undefined` links
// `@rpath/libclang_rt.ubsan_osx_dynamic.dylib`, which dyld resolves under the
// Xcode toolchain:
//
//   /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/
//     usr/lib/clang/<v>/lib/darwin/libclang_rt.ubsan_osx_dynamic.dylib
//
// That path is not a platform image and cannot be in a pinned inventory, so the
// policy correctly rejects the child and the launch never reaches `Launched`.
// ASan behaves the same way; Apple's clang has no static sanitizer runtime on
// macOS, so there is no build configuration that avoids it.
//
// So the failure is not flake and not a product defect — the test's premise is
// false under a sanitizer, and no amount of retrying changes that. Relaxing the
// production check to tolerate a foreign dylib would "fix" these tests by
// deleting the property they verify, which is the one thing not worth doing.
//
// Coverage is preserved where it counts: these cases still run, and still gate,
// on every non-sanitizer lane — including the required `macos` gate. What is
// skipped is a lane that could only ever report a false red.
//
// `PULP_TEST_WITH_SANITIZER` is defined by the test target when CMake configured
// with `PULP_SANITIZER`, matching the existing convention in
// `test/cmake/timeline_tests.cmake`.

#include <catch2/catch_test_macros.hpp>

namespace pulp::test {

// Returns true when the caller should stop: a sanitizer runtime is loaded into
// every child this process launches, so a pinned-runtime-closure assertion
// cannot pass. Reports the reason rather than passing silently.
inline bool skip_when_sanitizer_perturbs_runtime_closure() {
#if defined(PULP_TEST_WITH_SANITIZER)
    SUCCEED("skipped under a sanitizer build: the sanitizer runtime dylib is a "
            "foreign image in every launched child, which the trusted-host "
            "runtime-closure policy correctly rejects (see "
            "test/support/control_runtime_closure_sanitizer.hpp)");
    return true;
#else
    return false;
#endif
}

} // namespace pulp::test
