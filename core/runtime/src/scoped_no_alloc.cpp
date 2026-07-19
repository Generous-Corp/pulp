#include <pulp/runtime/scoped_no_alloc.hpp>

namespace pulp::runtime {

namespace {
thread_local int g_depth = 0;
// Number of live ScopedAllocAllowed instances suspending the contract.
thread_local int g_allow_depth = 0;
}

// Symbol always defined so mixed-mode linking (Release SDK + Debug
// downstream consumer, or vice versa) doesn't see a header that
// emits a call to a symbol the archive omits. The body is the
// no-op variant under NDEBUG.
ScopedNoAlloc::ScopedNoAlloc() noexcept {
#ifndef NDEBUG
    ++g_depth;
#endif
}

ScopedNoAlloc::~ScopedNoAlloc() noexcept {
#ifndef NDEBUG
    --g_depth;
#endif
}

// Same always-defined / NDEBUG-no-op shape as ScopedNoAlloc so the ABI is
// stable across mixed-mode links. A live instance suspends the contract for
// the one-time subtree-cache record (see the header).
ScopedAllocAllowed::ScopedAllocAllowed() noexcept {
#ifndef NDEBUG
    ++g_allow_depth;
#endif
}

ScopedAllocAllowed::~ScopedAllocAllowed() noexcept {
#ifndef NDEBUG
    --g_allow_depth;
#endif
}

bool is_in_no_alloc_scope() noexcept {
    // A ScopedAllocAllowed suspends the contract even while ScopedNoAlloc
    // guards remain on the stack (the cache-miss record legitimately
    // allocates and is non-realtime by definition).
    return g_depth > 0 && g_allow_depth == 0;
}

int no_alloc_scope_depth() noexcept {
    return g_depth;
}

int no_alloc_allowed_depth() noexcept {
    return g_allow_depth;
}

} // namespace pulp::runtime
