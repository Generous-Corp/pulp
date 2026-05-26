#pragma once

/// @file host_quirks/ableton_live.hpp
/// Per-host quirks for Ableton Live (macOS plan item 5.4).
///
/// DAW-quirks rows 5 + 6 (VST3):
///   * row 5: Live calls `checkSizeConstraint()` even when the plugin
///     advertises `canResize() == false`. The adapter must always
///     return valid bounds (fall back to current size) rather than
///     failing, otherwise Live silently refuses to display the editor.
///   * row 6: on Windows with per-monitor-V2 DPI, Live's host window
///     may not be ready when the plugin attempts to attach. The
///     adapter must defer view creation until the system-window
///     handle is non-null and retry on the next vsync. **No-op on
///     macOS / Linux** — the flag is set unconditionally because the
///     Windows VST3 adapter is the only call site that consults it.
///   * row 7 (VST2) is in Pulp's Deferred-by-design list and intentionally
///     not wired here.
///
/// Version handling: both rows 5 + 6 have been present in every Live
/// VST3 host vintage Pulp encounters, so `apply_ableton_live` fires
/// them regardless of `HostVersion`. If a future Live release fixes
/// either, add an `is_before(major)` guard at that point.
///
/// Additionally, Live 10.1.13 specifically ships a string-length
/// parsing bug where the `IInfoListener::getString` callbacks for
/// channel name, channel UID, and index-namespace queries write past
/// the spec-required buffer length and corrupt adjacent memory. The
/// adapter must allocate those buffers at twice the spec size on this
/// exact build. The flag is gated on `HostVersion == {10, 1, 13}` so
/// no other Live version pays the doubled-allocation cost.
///
/// **Reference-Lineage**: cleanroom reproducer=macos-plan-item-5.4
/// docs=https://help.ableton.com/hc/en-us/articles/4419010492444-Working-with-VST-Plug-Ins

#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format::host_quirks {

/// Populate Ableton Live host-gated flags onto `q`. `v` is unused
/// today (both rows are version-invariant) but kept for signature
/// uniformity with other per-host modules.
inline void apply_ableton_live(HostQuirks& q, HostVersion v) {
    // Row 5 — VST3 always-return-valid-bounds from checkSizeConstraint.
    q.live_vst3_canresize_ignore = true;
    // Row 6 — VST3 Windows per-monitor-DPI defer. No-op off Windows
    // at the adapter call site; the flag is unconditional here so
    // the Windows path picks it up automatically.
    q.live_vst3_windows_dpi_defer = true;
    // Live 10.1.13 — getString channel-name / channel-UID / index-
    // namespace buffer-doubling. Exact-version gate (major+minor+patch)
    // so no other Live release pays the doubled-allocation cost.
    if (v.major == 10 && v.minor == 1 && v.patch == 13) {
        q.double_string_buffer_for_live_10_1_13 = true;
    }
}

}  // namespace pulp::format::host_quirks
