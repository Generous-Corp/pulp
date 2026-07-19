#pragma once

#include <pulp/canvas/canvas_capability.hpp>
#include <pulp/runtime/log.hpp>

#include <atomic>
#include <cstdint>

namespace pulp::view {

/// Warn at most once per process, per capability, when a paint path degrades
/// because the active canvas backend does not support the capability it needs.
///
/// The dedup is a relaxed atomic bitmask keyed by the CanvasCapability ordinal:
/// allocation-free on every call after the first, so it is safe inside
/// paint_all's ScopedNoAlloc guard (the one-time first-warn log is a
/// non-realtime event by construction, and the guard is a no-op in Release).
/// `msg` must be a string literal — passed straight to log_warn without format.
///
/// Shared home for the ONE warn-once vocabulary: both View::paint_all's
/// capability-fallback sites and ImageView's image-decoder-missing path route
/// through here, so a new adoption site never re-hand-rolls a second dedup.
/// `inline` gives the static counter a single process-wide instance across
/// every translation unit that includes this header.
inline void warn_capability_fallback_once(canvas::CanvasCapability cap,
                                          const char* msg) {
    // The bitmask has one bit per capability ordinal. Guard the ceiling at
    // compile time so adding a 65th capability is a build error here rather
    // than silent dedup corruption (a shift past the width aliases another
    // cap's bit, or is UB). scene_cache..count are well within 64 today.
    static_assert(
        static_cast<std::uint32_t>(canvas::CanvasCapability::count) <= 64,
        "warn-once dedup uses a 64-bit bitmask keyed by capability ordinal; "
        "widen `warned` past uint64_t if CanvasCapability exceeds 64 members.");
    static std::atomic<std::uint64_t> warned{0};
    const std::uint64_t bit = std::uint64_t{1} << static_cast<std::uint32_t>(cap);
    if ((warned.fetch_or(bit, std::memory_order_relaxed) & bit) != 0) return;
    pulp::runtime::log_warn("{}", msg);
}

}  // namespace pulp::view
