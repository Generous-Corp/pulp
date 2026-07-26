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
/// * Sanitizers / debug allocator hooks can query
///   @c is_in_no_alloc_scope() and abort / log if an allocation
///   sneaks into the scope. This class is the contract surface.
/// * The guard is a no-op in @c NDEBUG so it costs nothing in
///   release builds — same shape as @c PULP_DBG_ASSERT.
///
/// Follows the same approach as dedicated RT-safety libraries like
/// radsan, built on the principle that UI paint should be treated as
/// carefully as the audio thread.
class ScopedNoAlloc {
public:
    // The ctor/dtor symbols are ALWAYS defined out-of-line so the
    // ABI is identical whether Pulp was compiled with NDEBUG or not.
    // Previously the header was guarded with `#ifdef NDEBUG` and the
    // .cpp body was compiled out under NDEBUG, which broke any
    // mixed-mode link (Release SDK + Debug downstream plugin emitted
    // calls to symbols the Release archive didn't ship). The body of
    // each ctor/dtor is conditional on NDEBUG inside scoped_no_alloc.cpp
    // — the symbol exists in both modes but does nothing in Release.
    ScopedNoAlloc() noexcept;
    ~ScopedNoAlloc() noexcept;

    ScopedNoAlloc(const ScopedNoAlloc&) = delete;
    ScopedNoAlloc& operator=(const ScopedNoAlloc&) = delete;
    ScopedNoAlloc(ScopedNoAlloc&&) = delete;
    ScopedNoAlloc& operator=(ScopedNoAlloc&&) = delete;
};

/// Narrow RAII exemption that SUSPENDS the no-alloc contract on the
/// current thread while alive, even when one or more @c ScopedNoAlloc
/// guards are still on the stack.
///
/// Use this only around a bounded operation that is known not to run on a
/// real-time thread but calls code shared with a no-allocation path. Current
/// examples are a subtree-cache miss record and an offscreen screenshot paint
/// pass. Both legitimately allocate while preparing a one-shot result; neither
/// is a live frame or audio callback. While an instance is alive,
/// @c is_in_no_alloc_scope() reports @c false regardless of the
/// @c ScopedNoAlloc depth, and nested @c ScopedNoAlloc guards remain suspended
/// until the outer allowance ends.
///
/// Keep each allowance at the narrowest call-site scope so the contract snaps
/// back immediately afterward. Like @c ScopedNoAlloc, the ctor/dtor symbols are
/// always defined out-of-line and the body is a no-op under @c NDEBUG, so the
/// ABI is identical across mixed-mode links.
class ScopedAllocAllowed {
public:
    ScopedAllocAllowed() noexcept;
    ~ScopedAllocAllowed() noexcept;

    ScopedAllocAllowed(const ScopedAllocAllowed&) = delete;
    ScopedAllocAllowed& operator=(const ScopedAllocAllowed&) = delete;
    ScopedAllocAllowed(ScopedAllocAllowed&&) = delete;
    ScopedAllocAllowed& operator=(ScopedAllocAllowed&&) = delete;
};

/// @return @c true if at least one @c ScopedNoAlloc is alive on the
///         calling thread AND no @c ScopedAllocAllowed is currently
///         suspending the contract. @c false in @c NDEBUG builds always.
bool is_in_no_alloc_scope() noexcept;

/// Depth of nested @c ScopedNoAlloc instances on the current thread.
/// Mostly useful for diagnostics and tests; @c is_in_no_alloc_scope()
/// covers the common "are we in one?" check. Not affected by
/// @c ScopedAllocAllowed — it counts guards, not the suspension.
int no_alloc_scope_depth() noexcept;

/// Depth of nested @c ScopedAllocAllowed instances on the current thread.
/// Diagnostics / tests only. @c 0 in @c NDEBUG builds always.
int no_alloc_allowed_depth() noexcept;

} // namespace pulp::runtime
