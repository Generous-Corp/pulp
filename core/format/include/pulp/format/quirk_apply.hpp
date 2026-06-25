#pragma once

/// @file quirk_apply.hpp
/// StateStore-aware host-quirk application helpers.
///
/// Kept separate from `host_quirks.hpp` so that header stays free of a
/// `pulp::state` dependency — only adapters that actually mutate the
/// StateStore (to synthesize parameters) include this.

#include <pulp/format/host_quirks.hpp>
#include <pulp/state/parameter.hpp>
#include <pulp/state/store.hpp>

namespace pulp::format {

/// Reserved ParamID for the synthesized Bypass parameter. Chosen
/// high + distinctive so it does not collide with plugin-declared IDs.
/// Stable across builds so host automation envelopes survive reloads.
inline constexpr state::ParamID kSynthesizedBypassParamId = 0x70427970;  // 'pByp'

/// synthesize_bypass_parameter: when the plugin declares
/// no Bypass parameter and the quirk is enforced, inject an automatable
/// boolean "Bypass" parameter into the StateStore so the host gets a
/// bypass control. Returns true if one was synthesized.
///
/// The range (min 0, max 1, default 0, step 1) matches the boolean-bypass
/// shape the VST3 / AU v3 adapters already detect (they tag it kIsBypass /
/// the AU bypass surface and pass-through audio in process when it is
/// engaged), so callers just inject it before their existing bypass-param
/// detection pass and the rest flows through unchanged.
///
/// No-op (returns false) when: the quirk is filtered out, a parameter
/// named "Bypass" already exists, or the reserved ID is already taken.
/// Not real-time safe — call once at adapter init, before processing.
inline bool maybe_synthesize_bypass(state::StateStore& store, const HostQuirks& q) {
    if (!q.synthesize_bypass_parameter) return false;
    for (const auto& p : store.all_params()) {
        // Suppress synthesis only when the plugin already declares a
        // DETECTABLE bypass — a boolean param named "Bypass" matching the
        // exact heuristic the VST3/AU adapters use to tag kIsBypass (name +
        // boolean range: step>=1, 0..1). A non-boolean param merely *named*
        // "Bypass" is not a bypass surface to those adapters, so we still
        // synthesize one rather than leave the host without a bypass control.
        const bool is_detectable_bypass =
            p.name == "Bypass" && p.range.step >= 1.0f &&
            p.range.min == 0.0f && p.range.max == 1.0f;
        if (is_detectable_bypass) return false;
        if (p.id == kSynthesizedBypassParamId) return false;  // ID-collision guard
    }
    state::ParamInfo info;
    info.id = kSynthesizedBypassParamId;
    info.name = "Bypass";
    info.range = {0.0f, 1.0f, 0.0f, 1.0f};  // boolean: step 1 → two states
    store.add_parameter(info);
    return true;
}

}  // namespace pulp::format
