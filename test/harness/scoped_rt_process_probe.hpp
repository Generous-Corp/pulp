#pragma once

// Shared RT-process probe for asserting that a realtime path neither allocates
// nor takes a blocking lock.
//
// Two backends, selected by the same PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS
// switch the native-core RT-safety suite uses:
//
//   * Trap build (UNIX, define set): enter an always-on RtNoAllocScope plus a
//     ScopedNoAlloc. The strong override in rt_intercept_test_support.cpp then
//     ABORTS the binary if an allocation or a pthread mutex/rwlock lock is
//     attempted inside the scope. allocation_count() is reported as 0 because a
//     violation would have aborted before the assertion ran — the death is the
//     signal.
//   * Counting build (everything else): wrap a thread-local RtAllocationProbe
//     that counts allocations via the harness operator-new override, so
//     allocation_count() == 0 is the assertion.
//
// Either way the calling test asserts allocation_count() == 0; the trap build
// additionally guarantees lock-freedom. ScopedNoAlloc is entered in both builds
// so its production placements (View::paint, the format adapters) stay coherent
// with the test contract. See test/native_components/rt_intercept_test_support.cpp.

#if PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS
#include "../native_components/rt_test_scope.hpp"
#else
#include "rt_allocation_probe.hpp"
#endif

#include <pulp/runtime/scoped_no_alloc.hpp>

#include <cstddef>

namespace pulp::test {

class ScopedRtProcessProbe {
public:
    ScopedRtProcessProbe() = default;
    ~ScopedRtProcessProbe() = default;

    ScopedRtProcessProbe(const ScopedRtProcessProbe&) = delete;
    ScopedRtProcessProbe& operator=(const ScopedRtProcessProbe&) = delete;

    std::size_t allocation_count() const noexcept {
#if PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS
        return 0;
#else
        return allocation_probe_.allocation_count();
#endif
    }

    std::size_t allocated_bytes() const noexcept {
#if PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS
        return 0;
#else
        return allocation_probe_.allocated_bytes();
#endif
    }

private:
#if PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS
    pulp::native_components::test::RtNoAllocScope rt_scope_;
#else
    pulp::test::RtAllocationProbe allocation_probe_;
#endif
    pulp::runtime::ScopedNoAlloc no_alloc_;
};

}  // namespace pulp::test
