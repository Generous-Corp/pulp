// RT-safety interception, TEST BUILDS ONLY.
//
// This translation unit is linked ONLY into the opt-in Rust FFI test
// executable (PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS). It must never reach
// production. It does two things:
//
//   1. Provides a STRONG pulp_rt_trap_if_no_alloc_scope that aborts when an
//      allocation is attempted inside a no-alloc scope. This overrides the weak
//      no-op default in core/native-components. Both the Rust checking global
//      allocator (kind=2) and the C++ operator new below (kind=0) call it.
//   2. Overrides the global C++ operator new/delete so a C++ heap allocation
//      inside a no-alloc scope is trapped too.
//
// The trap itself allocates nothing, locks nothing, and only writes a fixed
// message with write(2) before aborting — safe to call from anywhere,
// including a death-test child. macOS + Linux are the primary enforcement
// platforms (per the Phase 1 design); both honour a strong global
// operator-new override and a Rust #[global_allocator] in this executable.

#include "rt_test_scope.hpp"

#include <pulp/native_components/native_core.h>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#include <unistd.h>  // write, _exit

namespace pulp::native_components::test {
namespace {
thread_local int g_rt_test_depth = 0;
}

RtNoAllocScope::RtNoAllocScope() noexcept { ++g_rt_test_depth; }
RtNoAllocScope::~RtNoAllocScope() noexcept { --g_rt_test_depth; }

bool rt_test_in_no_alloc_scope() noexcept { return g_rt_test_depth > 0; }

}  // namespace pulp::native_components::test

namespace {

bool in_no_alloc_scope() noexcept {
    // Either the always-on test guard or the (NDEBUG-gated) production marker.
    return pulp::native_components::test::rt_test_in_no_alloc_scope() ||
           pulp::runtime::is_in_no_alloc_scope();
}

[[noreturn]] void trap_now(std::int32_t kind) noexcept {
    static const char msg[] =
        "[pulp-rt-trap] allocation inside no-alloc scope\n";
    // write(2) is async-signal-safe and never allocates.
    ssize_t r = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)r;
    (void)kind;
    // Abort via SIGABRT; the parent death-test asserts the child died here.
    std::abort();
}

}  // namespace

// Strong override of the contract trap. Called by the Rust checking allocator
// and the C++ operator new below. Returns quickly when not in a no-alloc scope.
extern "C" void pulp_rt_trap_if_no_alloc_scope(std::int32_t kind,
                                               std::size_t /*bytes*/) {
    if (in_no_alloc_scope()) {
        trap_now(kind);
    }
}

// Global operator new/delete overrides (kind = 0 == C++ new). Only allocation
// is forbidden in scope; deletion is always allowed.
void* operator new(std::size_t n) {
    pulp_rt_trap_if_no_alloc_scope(0, n);
    if (n == 0) {
        n = 1;
    }
    void* p = std::malloc(n);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](std::size_t n) { return operator new(n); }

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    pulp_rt_trap_if_no_alloc_scope(0, n);
    if (n == 0) {
        n = 1;
    }
    return std::malloc(n);
}

void* operator new[](std::size_t n, const std::nothrow_t& nt) noexcept {
    return operator new(n, nt);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}
