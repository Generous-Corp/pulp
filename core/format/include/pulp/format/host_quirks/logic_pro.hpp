#pragma once

/// @file host_quirks/logic_pro.hpp
/// Per-host quirks for Apple Logic Pro + GarageBand (macOS plan
/// item 5.10).
///
/// DAW-quirks rows 19 + 20 (AU v1 / v2):
///   * row 19: Logic hangs when the AU plugin probes for more than 8
///     channels during channel-count enumeration. The AU v2 adapter
///     hard-caps `maxChannelsToProbeFor` at 8 when Logic (or GarageBand,
///     which inherits Logic's AU host stack) is detected; the default
///     stays at 64 for other AU hosts.
///   * row 20: `getTailLengthSeconds()` is documented as `NSTimeInterval`
///     (seconds, wall-clock), not a pre-computed sample count. The AU
///     v2 adapter keeps a cached sample rate fresh across
///     `prepareToPlay` events and converts `Processor::tail_seconds()`
///     into AU's expected wall-clock units on demand.
///
/// Version handling: both rows are present across every Logic /
/// GarageBand vintage Pulp targets — the channel-probe hang has been
/// observed from Logic 8 through Logic 11, and the tail-time unit
/// convention is part of the AU v2 contract. `apply_logic_pro` fires
/// both regardless of `HostVersion`. GarageBand is treated as Logic by
/// the dispatch table (`make_quirks_for` maps `HostType::GarageBand`
/// here) — same AU host stack, same accommodations.
///
/// **Reference-Lineage**: cleanroom reproducer=macos-plan-item-5.10
/// docs=https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/AudioUnitProgrammingGuide/Introduction/Introduction.html

#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format::host_quirks {

/// Populate Logic Pro (and GarageBand-as-Logic) AU host-gated flags
/// onto `q`. `v` is unused today (both rows are version-invariant) but
/// kept for signature uniformity with other per-host modules.
inline void apply_logic_pro(HostQuirks& q, HostVersion /*v*/) {
    // Row 19 — Logic hangs above 8 probed channels. Hard-cap at 8.
    q.logic_au_channel_probe_cap = 8;
    // Row 20 — Tail-time samples-to-seconds conversion via cached
    // sample rate (kept fresh in the AU v2 adapter's prepare path).
    q.logic_au_tail_time_conversion = true;
}

}  // namespace pulp::format::host_quirks
