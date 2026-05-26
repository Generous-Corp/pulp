#pragma once

/// @file host_quirks/pro_tools.hpp
/// Per-host quirks for Avid Pro Tools (macOS plan item 5.9).
///
/// Pro Tools is the lone AAX host in Pulp's catalog, and AAX is a
/// developer-supplied SDK lane (`PULP_AAX_SDK` is opt-in). The flags
/// below are populated on `HostType::ProTools` detection regardless of
/// whether the Avid SDK is wired up at build time — adapters that want
/// to use them must themselves be guarded by the AAX SDK presence. This
/// keeps the quirks dispatch table consistent across builds (so
/// validated-only / off filtering behaves identically with or without
/// the SDK), and lets non-AAX adapters consult the cross-host
/// `aax_vendor_version_unknown` lesson without dragging in the AAX
/// build dependency.
///
/// ## Rows covered
///
/// * row 16 (`pro_tools_aax_sidechain_negotiation`) — AAX's signal-graph
///   model treats sidechain as an explicit edge: the host sends
///   `connect` / `disconnect` events the plugin must handle and reflect
///   in its bus state. Missing handlers leave the sidechain bus
///   orphaned. The AAX adapter wires sidechain-connect / disconnect
///   handlers that flip the sidechain-bus active flag and re-publish
///   latency if it changes.
/// * row 17 (`pro_tools_aax_latency_callback_push`) — Pro Tools' AAX
///   wrapper uses an **active push** model for PDC: latency is
///   reported via `Controller()->SetSignalLatency()`, not by passive
///   query. The AAX adapter subscribes to the plugin's
///   latency-changed signal and pushes to AAX's controller on change +
///   once at `prepareToPlay`.
/// * row 18 (`pro_tools_aax_mono_second_bus`) — historical AAX
///   constraint: Pro Tools assumes a mono cue on the second input bus
///   when declaring sidechain support. The AAX adapter presents only a
///   mono variant in its descriptor when sidechain is declared.
///
/// Plus the iPlug2-audit lesson layered on top:
///
/// * `aax_vendor_version_unknown` — the AAX wrapper does not reliably
///   surface its host vendor version through the AAX specification
///   surface Pulp can query at runtime (the field can return zero or
///   stale data depending on init order). Adapters and quirks must
///   treat the Pro Tools version as unknown and avoid version-gated
///   branching for this host.
///
/// ## Version handling
///
/// Pro Tools' AAX vendor version is unreliable by construction (per
/// the `aax_vendor_version_unknown` lesson above), so there is no
/// point in version-keying any of these rows: `apply_pro_tools` fires
/// every flag unconditionally. Callers that want to gate by Pro Tools
/// release should branch on `aax_vendor_version_unknown` and treat
/// the version as opaque rather than calling `HostVersion::is_at_least`.
///
/// ## Tier status
///
/// All three AAX-quirk rows (16-18) are tagged `Speculative` in
/// `HostQuirksMeta` as of this header extraction: they are documented
/// from Avid AAX documentation + reproducer reports, with per-symptom
/// isolation tests in `test/test_host_quirks.cpp` pinning the dispatch
/// — but the in-DAW bench evidence (driving real Pro Tools sessions
/// through Pulp's AAX adapter) is still pending and is itself gated on
/// the Avid SDK being present. Promote to `Validated` when the bench
/// rows ship under `PULP_AAX_SDK`. `aax_vendor_version_unknown`
/// remains `LessonOnly` since it ships as a 2026-05-25 iPlug2-audit
/// catalog lesson without an in-tree bench yet.
///
/// **Reference-Lineage**: cleanroom reproducer=macos-plan-item-5.9
/// docs=Avid AAX SDK documentation (developer-supplied; opt-in lane)

#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format::host_quirks {

/// Populate Pro Tools' main 3 AAX host-gated flags (rows 16-18) onto
/// `q`. `v` is unused today (every row is version-invariant by design
/// — see `aax_vendor_version_unknown`) but kept for signature
/// uniformity with other per-host modules.
inline void apply_pro_tools(HostQuirks& q, HostVersion /*v*/) {
    // Row 16 — AAX sidechain is an explicit-edge model; wire the
    // connect/disconnect handlers and re-publish latency on changes.
    q.pro_tools_aax_sidechain_negotiation = true;
    // Row 17 — Pro Tools needs an active push to
    // Controller()->SetSignalLatency() on change + once at
    // prepareToPlay; passive getLatencySamples() is not enough.
    q.pro_tools_aax_latency_callback_push = true;
    // Row 18 — historical AAX constraint: declare only a mono sidechain
    // variant in the AAX descriptor.
    q.pro_tools_aax_mono_second_bus = true;
}

/// Populate the AAX vendor-version-unknown lesson onto `q`. Layered on
/// top of `apply_pro_tools(...)` so the iPlug2-audit lesson can stay
/// at a different validation tier (LessonOnly today) without changing
/// the rest of the Pro Tools dispatch (Speculative).
inline void apply_pro_tools_aax_vendor_version_unknown(HostQuirks& q,
                                                      HostVersion /*v*/) {
    q.aax_vendor_version_unknown = true;
}

}  // namespace pulp::format::host_quirks
