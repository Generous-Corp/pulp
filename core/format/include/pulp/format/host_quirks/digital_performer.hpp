#pragma once

/// @file host_quirks/digital_performer.hpp
/// Per-host quirks for MOTU Digital Performer.
///
/// Digital Performer revives the edit controller on preset-load /
/// project-import paths without re-querying the previously published
/// parameter list. Plug-ins that mutate the parameter set via
/// `restartComponent(kParamValuesChanged | kParamTitlesChanged)` end up
/// with stale labels and stale value-domain bounds in DP's automation
/// lane.
///
/// The clean-room fix layers an additional `kReloadComponent`
/// notification onto the standard restart bundle when the detected
/// host is Digital Performer. Cubase / Logic / Reaper do not require
/// this — they re-query the parameter list whenever the edit
/// controller is recreated.
///
/// ## Rows covered
///
/// * `digital_performer_param_list_reload` — emit
///   `IComponentHandler::kReloadComponent` on top of the standard
///   `kParamValuesChanged | kParamTitlesChanged` notification when the
///   plug-in mutates its parameter list mid-session.
///
/// ## Version handling
///
/// Documented across Digital Performer 10.x and 11.x; no fixed release
/// announced. `apply_digital_performer` fires the flag
/// unconditionally.
///
/// ## Tier status
///
/// `LessonOnly` as of the 2026-05-26 iPlug2-audit batch — documented
/// from Steinberg's VST3 SDK + MOTU Digital Performer Plug-Ins Guide,
/// no in-tree bench yet. Promote to `Speculative` after the
/// per-symptom isolation tests ship; promote to `Validated` after
/// Digital Performer bench evidence lands.
///
/// **Reference-Lineage**: cleanroom reproducer=#3046
/// docs=https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Workflow+Diagrams/Edit+Controller+Call+Sequence.html

#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format::host_quirks {

/// Populate Digital Performer host-gated flags onto `q`. `v` is unused
/// today (the only flag here is version-invariant) but kept for
/// signature uniformity with other per-host modules.
inline void apply_digital_performer(HostQuirks& q, HostVersion /*v*/) {
    q.digital_performer_param_list_reload = true;
}

}  // namespace pulp::format::host_quirks
