#include <pulp/runtime/scoped_no_alloc.hpp>

namespace pulp::runtime {

namespace {
thread_local int g_depth = 0;
}

#ifndef NDEBUG

ScopedNoAlloc::ScopedNoAlloc() noexcept {
    ++g_depth;
}

ScopedNoAlloc::~ScopedNoAlloc() noexcept {
    --g_depth;
}

#endif

bool is_in_no_alloc_scope() noexcept {
    return g_depth > 0;
}

int no_alloc_scope_depth() noexcept {
    return g_depth;
}

} // namespace pulp::runtime
