#pragma once

/// @file host_quirks/fl_studio.hpp
/// Per-host quirks for Image-Line FL Studio.
///
/// DAW-quirks rows 13 + 14 (VST3):
///   * row 13: FL's Patcher routing layer violates the VST3 activation
///     contract — it can call `process()` before `setActive(true)` and
///     can call `setBusArrangements()` concurrently with `process()`.
///     The adapter must wrap `setActive` / `process` / `setBusArrangements`
///     in a mutex when FL Studio is detected to prevent state races and
///     undefined-behavior overlap. Cheaper than diagnosing the data race
///     after the fact.
///   * row 14: FL's state-blob format diverges enough from reference VST3
///     that the `MemoryStream`-shaped reader path fails outright. The
///     adapter must skip that reader and fall through to a tolerant
///     unknown-format reader so FL sessions deserialize cleanly.
///
/// Version handling: both quirks have been present across every FL
/// Studio VST3 vintage Pulp encounters (the Patcher contract issue is
/// architectural; the state-blob divergence predates the 20.x window
/// and is still present). `apply_fl_studio` fires both flags regardless
/// of `HostVersion`. If a future FL release fixes either, add an
/// `is_before(major)` guard at that point.
///
/// **Reference-Lineage**: cleanroom reproducer=macos-plan-item-5.7
/// docs=https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/wrapper.htm

#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format::host_quirks {

/// Populate FL Studio host-gated flags onto `q`. `v` is unused today
/// (both rows are version-invariant) but kept for signature uniformity
/// with other per-host modules.
inline void apply_fl_studio(HostQuirks& q, HostVersion /*v*/) {
    // Row 13 — mutex around setActive / process / setBusArrangements
    // to survive FL Patcher's concurrent / out-of-order dispatch.
    q.fl_studio_setactive_process_mutex = true;
    // Row 14 — skip MemoryStream-shaped reader; fall through to a
    // tolerant unknown-format reader for FL state blobs.
    q.fl_studio_state_reader_skip = true;
}

}  // namespace pulp::format::host_quirks
