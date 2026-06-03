// Test-only no-alloc scope for the native-component RT-safety hook.
//
// pulp::runtime::ScopedNoAlloc is a no-op under NDEBUG, so a death-test that
// relied on it would pass or fail depending on the build type. This guard is
// ALWAYS active in test builds, so the RT-safety self-test is deterministic
// regardless of Debug/Release. The strong override of
// pulp_rt_trap_if_no_alloc_scope (rt_intercept_test_support.cpp) aborts when
// either this guard OR ScopedNoAlloc reports an active scope.
#pragma once

namespace pulp::native_components::test {

// Enter on every thread that runs an RT region under test. Reentrant.
class RtNoAllocScope {
public:
    RtNoAllocScope() noexcept;
    ~RtNoAllocScope() noexcept;
    RtNoAllocScope(const RtNoAllocScope&) = delete;
    RtNoAllocScope& operator=(const RtNoAllocScope&) = delete;
};

// True while at least one RtNoAllocScope is alive on the calling thread.
bool rt_test_in_no_alloc_scope() noexcept;

}  // namespace pulp::native_components::test
