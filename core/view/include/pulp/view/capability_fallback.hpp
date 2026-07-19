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
    static std::atomic<uint32_t> warned{0};
    const uint32_t bit = 1u << static_cast<uint32_t>(cap);
    if ((warned.fetch_or(bit, std::memory_order_relaxed) & bit) != 0) return;
    pulp::runtime::log_warn("{}", msg);
}

}  // namespace pulp::view
