#pragma once

/// @file host_quirks/bitwig.hpp
/// Per-host quirks for Bitwig Studio (macOS plan item 5.5).
///
/// DAW-quirks rows 8 + 9 (VST3):
///   * row 8: on Linux / BSD, Bitwig does not auto-invalidate after a
///     host-initiated editor resize. The adapter must force a full-view
///     `View::invalidate()` after `IPlugView::onSize()` returns so the
///     plugin's UI actually repaints. **No-op off Linux** — the flag is
///     set unconditionally because the Linux VST3 adapter is the only
///     call site that consults it.
///   * row 9: older Bitwig versions (< 6.0) call `setBusArrangements()`
///     while the plugin is active, in violation of the VST3 spec. The
///     adapter must treat that call as advisory — either silently ignore
///     when the new arrangement matches the current one, or transparently
///     deactivate / reconfigure / reactivate. Modern Bitwig (6.0+) honors
///     the spec, so the workaround stays off there.
///
/// Version handling: row 8 is platform-keyed (Linux), not version-keyed,
/// so it fires unconditionally and the adapter no-ops off Linux. Row 9
/// only fires on `major < 6.0`. `HostVersion{0,0,0}` (unknown) is treated
/// as legacy — the conservative workaround stays on so a misdetected
/// older Bitwig still survives.
///
/// **Reference-Lineage**: cleanroom reproducer=macos-plan-item-5.5
/// docs=https://www.bitwig.com/userguide/latest/

#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format::host_quirks {

/// Populate Bitwig host-gated flags onto `q` based on `v`.
inline void apply_bitwig(HostQuirks& q, HostVersion v) {
    // Row 8 — Linux-only repaint workaround. No-op off Linux at the
    // adapter call site; flag is unconditional here.
    q.bitwig_vst3_linux_repaint_after_resize = true;
    // Row 9 — setBusArrangements-while-active workaround. Fixed in
    // Bitwig 6.0; conservative for unknown versions (treated as legacy).
    if (v.is_before(6, 0)) {
        q.bitwig_vst3_setbusarrangements_while_active = true;
    }
}

}  // namespace pulp::format::host_quirks
