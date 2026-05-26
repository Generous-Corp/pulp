#pragma once

/// @file host_quirks/auv3_cross_host.hpp
/// AU v3 cross-host quirks (macOS plan item 5.11).
///
/// DAW-quirks rows 21 + 22 — these are not specific to any single AU v3
/// host; they're contract-level accommodations every AU v3 wrapper
/// surface needs. The dispatch table layers this helper on top of the
/// per-host helpers for any host that exposes an AU v3 plug-in surface
/// (today: Logic Pro + GarageBand on macOS; the same flags also apply
/// when Pulp's AU v3 plug-in extension runs as a child process of an
/// iOS / macOS host that exposes the AU v3 wrapper surface — those
/// hosts are currently reported as `HostType::Unknown` and the AU v3
/// adapter is responsible for re-applying this helper when it detects
/// the wrapper-provided host identifier at runtime).
///
///   * row 21: AU v3 expects the bypass parameter to be tracked on two
///     surfaces — the plugin-provided bypass param and the platform's
///     `AUValue` for `getShouldBypassEffect`. One-sided updates cause
///     stale UI (if only the platform side updates) or stale audio (if
///     only the plugin side updates). The AU v3 adapter must register a
///     single bypass parameter that proxies both ways.
///   * row 22: AU v3 plug-ins run in a sandboxed extension; the host
///     executable name is not visible to the bundle. Host detection
///     must consume the wrapper-provided host identifier (e.g.
///     `AUHostingService`-reported value) instead of the executable-
///     path heuristic. The flag tells the host-detection layer to
///     prefer the wrapper-reported identifier when set.
///
/// Version handling: both rows are AU v3 contract behavior, not host-
/// version-keyed. The helper fires both flags unconditionally for any
/// host that exposes an AU v3 surface.
///
/// **Reference-Lineage**: cleanroom reproducer=macos-plan-item-5.11
/// docs=https://developer.apple.com/documentation/audiotoolbox/auaudiounit

#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format::host_quirks {

/// Populate AU v3 cross-host flags onto `q`. Layered on top of the
/// per-host helper for any host with an AU v3 surface. Version-invariant
/// — the flags reflect AU v3 contract requirements rather than per-host
/// version bands.
inline void apply_auv3_cross_host(HostQuirks& q, HostVersion /*v*/) {
    // Row 21 — dual-track bypass (plugin param ↔ AU v3 platform AUValue).
    q.au_v3_bypass_dual_tracking = true;
    // Row 22 — prefer wrapper-reported host ID over executable-path
    // heuristic at the host-detection layer.
    q.au_v3_host_id_from_wrapper = true;
}

}  // namespace pulp::format::host_quirks
