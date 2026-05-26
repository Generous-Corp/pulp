#pragma once

/// @file host_quirks/reaper.hpp
/// Per-host quirks for Cockos REAPER.
///
/// REAPER's VST2 and VST3 host wrappers share the same keyboard event
/// pipeline: `onKeyDown` / `onKeyUp` (or the equivalent VST2
/// `effEditKeyDown` opcode) deliver a `keyMsg` payload that is only
/// reliably populated for the Space key (`VKEY_SPACE`). Other keys
/// arrive with malformed key state — virtual key code, modifier bits,
/// and character payload can all be inconsistent or zero. Editors that
/// route keyboard input through the format adapter must reject non-Space
/// keys on REAPER rather than acting on the garbage payload.
///
/// The existing REAPER flags (gesture ordering, process-while-bypassed,
/// keyboard passthrough, permissive bus arrangements, anticipative-FX
/// buffer variability, mid-session setstate) are still set in the
/// dispatch table in `host_quirks.cpp`; this header layers the
/// keyboard-only-space flag on top so the per-host module can grow
/// without expanding the dispatch table further.
///
/// Version handling: the keyboard quirk is documented across every
/// REAPER vintage Pulp encounters (5.x through current 7.x line), so
/// `apply_reaper_keyboard` fires it unconditionally.
///
/// **Reference-Lineage**: cleanroom reproducer=iplug2-quirks-audit-2026-05-25
/// docs=https://www.reaper.fm/sdk/vst/vst_ext.php

#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format::host_quirks {

/// Populate REAPER keyboard-only-space flag onto `q`. Layered on top of
/// the existing REAPER quirks set in `apply_reaper_quirks` so the two
/// can evolve independently.
inline void apply_reaper_keyboard(HostQuirks& q, HostVersion /*v*/) {
    q.reaper_keyboard_only_space = true;
}

}  // namespace pulp::format::host_quirks
