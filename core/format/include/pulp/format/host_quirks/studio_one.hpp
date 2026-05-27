#pragma once

/// @file host_quirks/studio_one.hpp
/// Per-host quirks for PreSonus Studio One.
///
/// Studio One's VST3 host serializes `IComponentHandler::restartComponent`
/// notifications through a non-thread-safe lane: calls that arrive from
/// the audio thread (most commonly `kLatencyChanged` from a delay-line
/// resize) can deadlock the host or be silently dropped. Cubase /
/// Nuendo / Reaper accept the call from any thread; Studio One does
/// not.
///
/// The clean-room fix marshals `restartComponent` calls to the UI
/// thread when the host is detected as Studio One. The dispatch lives
/// here; the VST3 adapter consults the flag to decide whether to
/// post the notification through `core/events::EventLoop` rather than
/// invoking the host handler directly.
///
/// ## Rows covered
///
/// * `studio_one_restart_component_ui_thread` — UI-thread restart
///   marshalling for any audio-thread-emitted notification (the field
///   primarily targets `kLatencyChanged` but the adapter can layer on
///   the same defense for `kParamValuesChanged` and friends).
///
/// ## Version handling
///
/// The threading contract appears in every Studio One release Pulp
/// has surveyed (5.x and 6.x). `apply_studio_one` fires the flag
/// unconditionally; if a future Studio One release switches to a
/// thread-safe restart-component lane, add an `is_before(major,
/// minor)` guard at that point.
///
/// ## Tier status
///
/// `LessonOnly` as of the 2026-05-26 iPlug2-audit batch — documented
/// from Steinberg's VST3 SDK threading contract + PreSonus user-forum
/// reports, no in-tree bench yet. Promote to `Speculative` after the
/// per-symptom isolation tests ship; promote to `Validated` after
/// Studio One bench evidence lands.
///
/// **Reference-Lineage**: cleanroom reproducer=#3045
/// docs=https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Workflow+Diagrams/Edit+Controller+Call+Sequence.html

#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format::host_quirks {

/// Populate Studio One host-gated flags onto `q`. `v` is unused today
/// (the only flag here is version-invariant) but kept for signature
/// uniformity with other per-host modules.
inline void apply_studio_one(HostQuirks& q, HostVersion /*v*/) {
    q.studio_one_restart_component_ui_thread = true;
}

}  // namespace pulp::format::host_quirks
