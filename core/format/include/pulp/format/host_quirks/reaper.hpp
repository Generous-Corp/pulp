#pragma once

/// @file host_quirks/reaper.hpp
/// Per-host quirks for Cockos REAPER (macOS plan item 5.8).
///
/// REAPER is a forgiving host — it absorbs many spec violations — but it
/// also exposes more permissive configurations (e.g. arbitrary pin
/// connectivity, mid-session FX-chain reloads, anticipative-FX rendering)
/// than the typical VST3 / CLAP / AU host, so several quirks are
/// REAPER-specific accommodations rather than spec bugs.
///
/// This header now owns the **full** REAPER quirk dispatch (DAW-quirks
/// rows 15 + R1-R7), pulled out of `core/format/src/host_quirks.cpp`
/// into a per-host module so the dispatch table doesn't grow further and
/// REAPER-specific lessons live next to each other.
///
/// ## Rows covered
///
/// * row 15 (`reaper_vst3_gesture_ordering`) — REAPER's VST3 automation
///   reader expects parameter gesture-begin / value-change / gesture-end
///   events in a particular order that differs from Cubase / Wavelab.
///   Mis-ordered gestures produce spurious touch-automation lanes. The
///   VST3 adapter consults this flag to switch to REAPER's ordering when
///   the host is detected as REAPER.
/// * row R1 (`reaper_process_while_bypassed`) — REAPER's "Run FX while
///   bypassed" track option calls `process()` on the plugin even while
///   bypassed (so tail state can settle cleanly). Adapters route bypass
///   to `Processor::process_bypassed()` rather than assuming
///   "bypassed = no process call".
/// * row R2 (`reaper_keyboard_passthrough`) — REAPER's "send all
///   keyboard input to plugin" track option routes raw key events into
///   the plugin editor in addition to REAPER's own shortcut handling.
///   `View::on_key_event` must distinguish "consumed" vs "not consumed"
///   so the adapter can propagate unconsumed keys back to the host.
/// * row R3 (`reaper_permissive_bus_arrangements`) — REAPER's FX-route
///   pin matrix is more permissive than Cubase / Live; the VST3 adapter
///   accepts any arrangement whose channel counts the plugin can handle
///   (silence-out the rest) rather than rejecting non-standard layout
///   names. Layers on top of the always-on
///   `silence_unsupported_bus_arrangements` cheap defense.
/// * row R4 (`reaper_anticipative_fx_buffer_variability`) — REAPER's
///   anticipative-FX / offline render path calls `process()` from a
///   non-realtime thread with buffer sizes that may differ from
///   `prepare()`'s `max_buffer_size`. The contract is that
///   `Processor::process()` honors per-block buffer-size variability
///   without re-allocating.
/// * row R5 (`reaper_vst3_gesture_ordering` covers the gesture half;
///   the vendor/product-string identity contract for REAPER's
///   compatibility-allow-list is handled by Track 3.12 — the
///   `PluginDescriptor::manufacturer` string contract — and does not
///   need a dedicated flag here. Documented for cross-reference.)
/// * row R6 (`reaper_midsession_setstate`) — REAPER's per-track FX-state
///   save/restore can fire `setState` at arbitrary times during a
///   session, not just at project-load. `Processor::set_state` must
///   stay safely callable mid-play; no resource re-allocation may
///   require `prepare()` to be re-called.
/// * row R7 (CLAP canary, no dedicated flag) — REAPER's CLAP host is
///   among the most reference-correct in the wild and is used by
///   Track 3.4 as the canary for any CLAP adapter regression. Carried
///   here as documentation; the CLAP-side acceptance lives in the CLAP
///   adapter test matrix.
///
/// Plus the iPlug2-audit lesson layered on top:
///
/// * `reaper_keyboard_only_space` — REAPER's VST2/VST3 `onKeyDown` /
///   `effEditKeyDown` pipeline only delivers a well-formed `keyMsg`
///   payload for the Space key (`VKEY_SPACE`); other keys arrive with
///   malformed key state. Editors that route keyboard input through
///   the format adapter must reject non-Space keys when this flag is
///   set. Documented across REAPER 5.x through current 7.x.
///
/// ## Version handling
///
/// Every REAPER quirk Pulp tracks today is documented across every
/// REAPER vintage Pulp encounters (5.x through current 7.x line), so
/// `apply_reaper` fires every flag unconditionally. If a future REAPER
/// release fixes a specific behavior, add an `is_before(major, minor)`
/// guard at that point.
///
/// ## Tier status
///
/// All of these flags are tagged `Speculative` in `HostQuirksMeta` as
/// of this header extraction: they are documented from REAPER vendor
/// docs + reproducer reports, with per-symptom isolation tests in
/// `test/test_host_quirks.cpp` pinning the dispatch — but the in-DAW
/// bench evidence (driving real REAPER 7.x sessions through Pulp's
/// adapters) is still pending. Promote to `Validated` when the bench
/// rows ship. `reaper_keyboard_only_space` remains `LessonOnly` since
/// it ships as a 2026-05-25 iPlug2-audit catalog lesson without an
/// in-tree bench yet.
///
/// **Reference-Lineage**: cleanroom reproducer=macos-plan-item-5.8
/// docs=https://www.reaper.fm/sdk/vst/vst_ext.php

#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format::host_quirks {

/// Populate REAPER's main 6 host-gated flags (rows 15 + R1-R4 + R6)
/// onto `q`. `v` is unused today (every row is version-invariant) but
/// kept for signature uniformity with other per-host modules.
inline void apply_reaper(HostQuirks& q, HostVersion /*v*/) {
    // Row 15 — REAPER-specific gesture/value ordering for the VST3
    // automation reader; mis-ordered events otherwise produce spurious
    // touch-automation lanes.
    q.reaper_vst3_gesture_ordering = true;
    // Row R1 — "Run FX while bypassed" routes process() calls to the
    // plugin even when bypassed so tail state can drain cleanly.
    q.reaper_process_while_bypassed = true;
    // Row R2 — "send all keyboard input to plugin" passes keys to the
    // editor in addition to REAPER's own shortcut handling; editors
    // must distinguish consumed vs unconsumed.
    q.reaper_keyboard_passthrough = true;
    // Row R3 — REAPER's FX-route pin matrix is more permissive than
    // typical VST3 hosts; accept channel-count-compatible arrangements
    // even when the layout name doesn't match a standard one.
    q.reaper_permissive_bus_arrangements = true;
    // Row R4 — anticipative-FX renders call process() from a non-RT
    // thread with variable buffer sizes; honor per-block variability
    // without re-allocating.
    q.reaper_anticipative_fx_buffer_variability = true;
    // Row R6 — per-track FX-state save/restore can call setState
    // mid-play; setState must stay RT-safe and callable at any time.
    q.reaper_midsession_setstate = true;
}

/// Populate REAPER keyboard-only-space flag onto `q`. Layered on top of
/// `apply_reaper(...)` so the iPlug2-audit lesson can stay at a
/// different validation tier (LessonOnly today) without changing the
/// rest of the REAPER dispatch (Speculative).
inline void apply_reaper_keyboard(HostQuirks& q, HostVersion /*v*/) {
    q.reaper_keyboard_only_space = true;
}

/// Populate REAPER AU v3 in-process `preferredContentSize` lesson onto
/// `q`. Layered on top of `apply_reaper(...)` so the 2026-05-26
/// iPlug2-audit lesson can stay at a different validation tier
/// (LessonOnly today) without changing the rest of the REAPER
/// dispatch.
///
/// REAPER's AU v3 host is in-process: it creates the `AUAudioUnit` on
/// the main thread via `createAudioUnitWithComponentDescription`,
/// rather than the out-of-process path Logic / GarageBand use. Setting
/// `preferredContentSize` only from `viewDidLoad` is too late on that
/// path — the editor opens at the extension's default size and snaps
/// to the requested size one paint cycle later. AU v3 adapters must
/// additionally set `preferredContentSize` synchronously in
/// `audioUnitInitialized`; setting it twice is harmless on the OOP
/// path.
///
/// Reference: https://developer.apple.com/documentation/audiotoolbox/auaudiounit
/// (in-process host extension contract) + Pulp issue #3044.
inline void apply_reaper_auv3_in_process(HostQuirks& q, HostVersion /*v*/) {
    q.reaper_auv3_in_process_preferred_size_sync = true;
}

}  // namespace pulp::format::host_quirks
