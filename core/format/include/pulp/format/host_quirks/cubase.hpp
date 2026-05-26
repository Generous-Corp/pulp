#pragma once

/// @file host_quirks/cubase.hpp
/// Per-host quirks for Steinberg Cubase + Nuendo (treated identically).
///
/// Cubase 10 vintage — DAW-quirks rows 1, 2, 3 (macOS plan item 5.2):
///   * row 1: async window resize after `IPlugView::onSize()` returns.
///   * row 2: parameter automation ordering — value-set must precede
///     gesture-edit notification within the same dispatch cycle.
///   * row 3: integer-rounded `setContentScaleFactor()` requires
///     plugin-side residual correction on fractional-DPI displays.
///
/// Cubase 9 vintage — DAW-quirks row 4 (macOS plan item 5.3):
///   * row 4: state-blob stream reports the wrong size; the adapter
///     must validate the actual read count rather than trusting the
///     host-reported size.
///
/// Version dispatch: Cubase 10 quirks fire on `major >= 10`; the
/// Cubase 9 state-blob quirk only fires on the 9.x line (NOT on
/// Cubase 10+) since 10 fixed the underlying stream bug. Cubase
/// `HostVersion{0,0,0}` (unknown) intentionally leaves both bands
/// off — the adapter falls back to spec-compliant defaults.
///
/// Nuendo is treated as Cubase in the dispatch table
/// (`make_quirks_for` maps `HostType::Nuendo` here) — Nuendo 10
/// inherits the Cubase 10 row, Nuendo 8/9 inherits Cubase 9's row.
///
/// **Reference-Lineage**: cleanroom reproducer=macos-plan-item-5.2/5.3
/// docs=https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Technical+Documentation.html

#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format::host_quirks {

/// Populate Cubase-specific (and Nuendo-as-Cubase) host-gated flags
/// onto `q` based on `v`. No-op when `v` is unknown.
inline void apply_cubase(HostQuirks& q, HostVersion v) {
    // Row 1, 2, 3 — Cubase 10+ (and Nuendo 10+) async-resize +
    // gesture-ordering + fractional-DPI correction.
    if (v.is_at_least(10, 0)) {
        q.cubase10_async_view_resize_queue = true;
        q.cubase10_param_gesture_ordering = true;
        q.cubase10_fractional_scale_correction = true;
    }
    // Row 4 — Cubase 9 (9.0 ≤ v < 10.0) state-blob size validation.
    // Cubase 10 fixed the underlying stream-size bug, so the validation
    // path stays off there.
    if (v.is_at_least(9, 0) && v.is_before(10, 0)) {
        q.cubase9_state_blob_size_validation = true;
    }
}

}  // namespace pulp::format::host_quirks
