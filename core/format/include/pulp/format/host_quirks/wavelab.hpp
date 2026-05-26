#pragma once

/// @file host_quirks/wavelab.hpp
/// Per-host quirks for Steinberg Wavelab (macOS plan item 5.6).
///
/// DAW-quirks rows 10 + 11 (VST3):
///   * row 10: Wavelab 11.1+ calls `setBusArrangements()` re-entrantly
///     from inside the `prepareToPlay` stack when the plugin invokes
///     `setLatencySamples()`. Without a deferral, the plugin can
///     deadlock or corrupt its activation state. The adapter must
///     defer activation-side effects (layout commit, latency report)
///     until the current stack unwinds.
///   * row 11: Wavelab's state-blob format diverges from standard VST3
///     enough that strict error reporting on a `MemoryStream` read
///     failure causes Wavelab to drop the session's parameter set.
///     The adapter must return success with a logged warning rather
///     than propagating the error — let the host believe the load
///     worked and present a clean parameter set.
///   * row 12 (VST2 editor-thread dispatch) is in Pulp's Deferred list.
///
/// Additional VST3 state-read tolerance:
///   Wavelab's `IBStream::read` can return a non-`kResultTrue` status at
///   end-of-stream while still having populated the supplied buffer.
///   Strict callers that propagate the status reject the load and lose
///   the parameter set. The VST3 adapter accepts the populated buffer
///   when the read count matches the request even if the status is not
///   `kResultTrue`. Version-invariant across the Wavelab versions Pulp
///   targets.
///
/// Version handling: row 10 is documented as Wavelab 11.1+. We fire
/// it on `major >= 11` (the underlying re-entrancy was observed
/// throughout the 11.x line; minor 11.0 is also affected per the
/// reproducer). Row 11 is version-invariant — Wavelab's state-blob
/// divergence predates the 11.x window and remains in the latest
/// release as of the macOS plan, so we fire it unconditionally.
///
/// **Reference-Lineage**: cleanroom reproducer=macos-plan-item-5.6
/// docs=https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Bus+Arrangement+Setup.html

#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format::host_quirks {

/// Populate Wavelab host-gated flags onto `q` based on `v`.
inline void apply_wavelab(HostQuirks& q, HostVersion v) {
    // Row 10 — Wavelab 11+ re-entrant setBusArrangements during
    // prepareToPlay. Adapter must defer activation-side effects.
    if (v.is_at_least(11, 0)) {
        q.wavelab_vst3_defer_activation = true;
    }
    // Row 11 — Wavelab state-blob graceful-restore fallback. Version-
    // invariant: the divergence exists across the Wavelab versions
    // Pulp targets and there's no fixed release on the horizon.
    q.wavelab_state_blob_fallback = true;
    // Wavelab IBStream::read returns non-kResultTrue at end-of-stream
    // even when the buffer is fully populated. Version-invariant.
    q.tolerate_state_read_nontrue_status = true;
}

}  // namespace pulp::format::host_quirks
