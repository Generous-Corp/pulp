// RT-safety interception, TEST BUILDS ONLY.
//
// This translation unit is linked ONLY into test executables. It must never
// reach production. It does three things:
//
//   1. Provides a STRONG pulp_rt_trap_if_no_alloc_scope that aborts when an
//      allocation is attempted inside a no-alloc scope. This overrides the weak
//      no-op default in core/native-components. Both the Rust checking global
//      allocator (kind=2) and the C++ operator new below (kind=0) call it.
//   2. Overrides the global C++ operator new/delete so a C++ heap allocation
//      inside a no-alloc scope is trapped too.
//   3. Overrides pthread mutex/rwlock lock entry points so a blocking lock
//      acquisition inside a no-alloc scope is trapped.
//
// The trap itself allocates nothing, locks nothing, and only writes a fixed
// message with write(2) before aborting — safe to call from anywhere,
// including a death-test child. macOS + Linux are the primary enforcement
// platforms; both honor a strong global operator-new override and a Rust
// #[global_allocator] in this executable.

#include "rt_test_scope.hpp"
#include "../harness/rt_allocation_probe.hpp"

#include <pulp/native_components/native_core.h>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <new>

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>  // write, _exit

namespace pulp::native_components::test {
namespace {
thread_local int g_rt_test_depth = 0;
}

RtNoAllocScope::RtNoAllocScope() noexcept { ++g_rt_test_depth; }
RtNoAllocScope::~RtNoAllocScope() noexcept { --g_rt_test_depth; }

bool rt_test_in_no_alloc_scope() noexcept { return g_rt_test_depth > 0; }

}  // namespace pulp::native_components::test

namespace pulp::test {
namespace {
thread_local RtAllocationProbe* g_current_probe = nullptr;
}

RtAllocationProbe::RtAllocationProbe() noexcept
    : previous_(g_current_probe) {
    g_current_probe = this;
}

RtAllocationProbe::~RtAllocationProbe() noexcept {
    g_current_probe = previous_;
}

void rt_allocation_probe_record(std::size_t bytes) noexcept {
    if (g_current_probe == nullptr) return;
    ++g_current_probe->allocation_count_;
    g_current_probe->allocated_bytes_ += bytes;
}

bool rt_allocation_probe_active() noexcept {
    return g_current_probe != nullptr;
}

}  // namespace pulp::test

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

template <typename Fn>
Fn resolve_next_symbol(const char* name) noexcept {
    void* symbol = ::dlsym(RTLD_NEXT, name);
    return reinterpret_cast<Fn>(symbol);
}

template <typename Fn>
Fn cached_next_symbol(std::atomic<Fn>& cache, const char* name) noexcept {
    Fn fn = cache.load(std::memory_order_acquire);
    if (fn != nullptr) {
        return fn;
    }
    fn = resolve_next_symbol<Fn>(name);
    cache.store(fn, std::memory_order_release);
    return fn;
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

extern "C" int pthread_mutex_lock(pthread_mutex_t* mutex) {
    pulp_rt_trap_if_no_alloc_scope(3, 0);
    using Fn = int (*)(pthread_mutex_t*);
    static std::atomic<Fn> real{nullptr};
    Fn fn = cached_next_symbol(real, "pthread_mutex_lock");
    if (fn == nullptr) {
        trap_now(3);
    }
    return fn(mutex);
}

extern "C" int pthread_mutex_trylock(pthread_mutex_t* mutex) {
    pulp_rt_trap_if_no_alloc_scope(3, 0);
    using Fn = int (*)(pthread_mutex_t*);
    static std::atomic<Fn> real{nullptr};
    Fn fn = cached_next_symbol(real, "pthread_mutex_trylock");
    if (fn == nullptr) {
        trap_now(3);
    }
    return fn(mutex);
}

extern "C" int pthread_rwlock_rdlock(pthread_rwlock_t* lock) {
    pulp_rt_trap_if_no_alloc_scope(3, 0);
    using Fn = int (*)(pthread_rwlock_t*);
    static std::atomic<Fn> real{nullptr};
    Fn fn = cached_next_symbol(real, "pthread_rwlock_rdlock");
    if (fn == nullptr) {
        trap_now(3);
    }
    return fn(lock);
}

extern "C" int pthread_rwlock_wrlock(pthread_rwlock_t* lock) {
    pulp_rt_trap_if_no_alloc_scope(3, 0);
    using Fn = int (*)(pthread_rwlock_t*);
    static std::atomic<Fn> real{nullptr};
    Fn fn = cached_next_symbol(real, "pthread_rwlock_wrlock");
    if (fn == nullptr) {
        trap_now(3);
    }
    return fn(lock);
}

// Global operator new/delete overrides (kind = 0 == C++ new). Only allocation
// is forbidden in scope; deletion is always allowed.
void* operator new(std::size_t n) {
    pulp_rt_trap_if_no_alloc_scope(0, n);
    pulp::test::rt_allocation_probe_record(n);
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
    pulp::test::rt_allocation_probe_record(n);
    if (n == 0) {
        n = 1;
    }
    return std::malloc(n);
}

void* operator new[](std::size_t n, const std::nothrow_t& nt) noexcept {
    return operator new(n, nt);
}

// Over-aligned (C++17 aligned-new) variants. Without these, an over-aligned
// allocation inside a no-alloc scope would route to libc++'s default aligned
// new and bypass the trap. Unix-only TU, so posix_memalign is available.
namespace {
void* trap_then_aligned_alloc(std::size_t n, std::align_val_t alignment) noexcept {
    pulp_rt_trap_if_no_alloc_scope(0, n);
    pulp::test::rt_allocation_probe_record(n);
    if (n == 0) {
        n = 1;
    }
    auto align = static_cast<std::size_t>(alignment);
    if (align < sizeof(void*)) {
        align = sizeof(void*);
    }
    void* p = nullptr;
    if (::posix_memalign(&p, align, n) != 0) {
        return nullptr;
    }
    return p;
}
}  // namespace

void* operator new(std::size_t n, std::align_val_t a) {
    void* p = trap_then_aligned_alloc(n, a);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return trap_then_aligned_alloc(n, a);
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t& nt) noexcept {
    return operator new(n, a, nt);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(p);
}
