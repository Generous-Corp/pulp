#pragma once

namespace pulp::runtime {

/// RAII guard that marks the current thread as "must not allocate".
///
/// Construct a @c ScopedNoAlloc at the entry of a real-time-safe
/// region (audio callback, paint cycle) and let it destruct on the
/// way out. While any instance is alive on the calling thread,
/// @c is_in_no_alloc_scope() returns @c true.
///
/// On its own this class only tracks state — it does not intercept
/// @c operator new. The intent is that:
///
/// * Pulp's audio (@c Processor::process) and paint
///   (@c View::paint_all) paths are wrapped in @c ScopedNoAlloc, so
///   the contract is enforced uniformly.
/// * Sanitizers / debug allocator hooks query
///   @c is_in_no_alloc_scope() and abort / log if an allocation
///   sneaks into the scope. We provide an opt-in hook library for
///   that in a follow-up; today the class is the contract surface.
/// * The guard is a no-op in @c NDEBUG so it costs nothing in
///   release builds — same shape as @c PULP_DBG_ASSERT.
///
/// Mirrors JUCE's @c JUCE_FORCE_DEBUG_ALLOCATIONS pattern and
/// dedicated RT-safety libraries like radsan.
///
/// Sliced after sudara "Big List of JUCE Tips" #28: treat paint
/// like the audio thread.
class ScopedNoAlloc {
public:
#ifndef NDEBUG
    ScopedNoAlloc() noexcept;
    ~ScopedNoAlloc() noexcept;
#else
    ScopedNoAlloc() noexcept = default;
    ~ScopedNoAlloc() noexcept = default;
#endif

    ScopedNoAlloc(const ScopedNoAlloc&) = delete;
    ScopedNoAlloc& operator=(const ScopedNoAlloc&) = delete;
    ScopedNoAlloc(ScopedNoAlloc&&) = delete;
    ScopedNoAlloc& operator=(ScopedNoAlloc&&) = delete;
};

/// @return @c true if at least one @c ScopedNoAlloc is alive on the
///         calling thread. @c false in @c NDEBUG builds always.
bool is_in_no_alloc_scope() noexcept;

/// Depth of nested @c ScopedNoAlloc instances on the current thread.
/// Mostly useful for diagnostics and tests; @c is_in_no_alloc_scope()
/// covers the common "are we in one?" check.
int no_alloc_scope_depth() noexcept;

} // namespace pulp::runtime
