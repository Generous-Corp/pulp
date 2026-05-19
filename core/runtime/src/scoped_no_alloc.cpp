#include <pulp/runtime/scoped_no_alloc.hpp>

namespace pulp::runtime {

namespace {
thread_local int g_depth = 0;
}

// Symbol always defined so mixed-mode linking (Release SDK + Debug
// downstream consumer, or vice versa) doesn't see a header that
// emits a call to a symbol the archive omits. The body is the
// no-op variant under NDEBUG. Codex P1 on PR #2316.
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

bool is_in_no_alloc_scope() noexcept {
    return g_depth > 0;
}

int no_alloc_scope_depth() noexcept {
    return g_depth;
}

} // namespace pulp::runtime
