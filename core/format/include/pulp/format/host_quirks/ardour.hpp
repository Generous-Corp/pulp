#pragma once

/// @file host_quirks/ardour.hpp
/// Per-host quirks for Ardour and its Harrison Mixbus 32C derivative.
///
/// Ardour's VST3 host wrapper has a long-standing `setBusArrangements`
/// callback that returns `kResultOk` without applying the request and
/// can leave the internal layout state inconsistent for subsequent
/// `activate` calls. Mixbus 32C inherits the same wrapper. The safe
/// behavior for both hosts is to skip the call entirely from the VST3
/// adapter and accept whatever bus arrangement the host published by
/// default.
///
/// Version handling: the quirk has been present in every Ardour vintage
/// Pulp encounters and there is no fixed release on the horizon, so
/// `apply_ardour` / `apply_mixbus32c` fire it unconditionally.
///
/// **Reference-Lineage**: cleanroom reproducer=iplug2-quirks-audit-2026-05-25
/// docs=https://ardour.org/manual/

#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format::host_quirks {

/// Populate Ardour host-gated flags onto `q`. `v` is unused today (the
/// only flag here is version-invariant) but kept for signature
/// uniformity with other per-host modules.
inline void apply_ardour(HostQuirks& q, HostVersion /*v*/) {
    q.skip_bus_arrangement_call = true;
}

/// Populate Mixbus 32C host-gated flags onto `q`. Mixbus 32C is an
/// Ardour derivative and inherits the same `setBusArrangements`
/// behavior; this helper exists so the dispatch table can branch on
/// the dedicated `HostType` value without losing the host identity at
/// the call site.
inline void apply_mixbus32c(HostQuirks& q, HostVersion /*v*/) {
    q.skip_bus_arrangement_call = true;
}

}  // namespace pulp::format::host_quirks
