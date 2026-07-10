#pragma once

/// @file param_ordering.hpp
/// Cross-version parameter-ordering guard.
///
/// A plugin's host-facing parameter identity must stay stable across releases:
/// a DAW session, a saved preset, and a host automation lane are all keyed to a
/// parameter's *host-facing ID*, and generic/host editors present parameters in
/// *index* (registration) order. Re-ordering, re-ID'ing, or dropping a
/// previously-shipped parameter silently remaps or clips automation that users
/// already recorded.
///
/// The host-facing ID is the same value in every format Pulp targets, because
/// `state::ParamID` maps 1:1 onto each:
///
/// - **VST3** — `Steinberg::Vst::ParameterInfo::id` (the automation key;
///   `getParameterInfo`/`getParamStringByValue` are all keyed by it).
/// - **AU v2** — `AudioUnitParameterID` (the `inParameterID`/`inID` passed to
///   `GetParameterInfo`/`GetParameter`).
/// - **CLAP** — `clap_param_info_t::id` (the stable `clap_id`; hosts persist
///   automation against it, independent of the `get_info` index).
///
/// All three key automation by ID, so **ID stability is the primary invariant**.
/// Index stability is the secondary invariant: several host paths (generic
/// editors, VST3 unit ordering, positionally-saved legacy presets) surface
/// parameters in registration order, so a retained ID that moves index is also
/// a user-visible regression. The only always-safe schema evolution is
/// therefore **append-only**: keep every previously-shipped parameter at its
/// existing index with its existing ID, and add new parameters at the end.
///
/// This utility diffs two descriptor versions (two `all_params()` spans, or two
/// `StateStore`s) and reports every violation of that rule. It is intended for
/// an author-side unit test / CI gate — not the audio path.

#include <pulp/state/parameter.hpp>
#include <pulp/state/store.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace pulp::state {

/// One way a new descriptor version broke a previously-shipped parameter's
/// host-facing identity.
struct ParamOrderingViolation {
    enum class Kind {
        /// The parameter that used to occupy @c index now carries a different
        /// host-facing ID (it was re-ID'd, or a parameter was inserted/removed
        /// ahead of it and shifted it). Automation keyed to @c old_id at that
        /// slot now targets @c new_id.
        IdChangedAtIndex,
        /// A host-facing ID present in the old version is absent from the new
        /// version entirely. Any saved automation for it is orphaned.
        ParamRemoved,
    };

    Kind kind;
    std::size_t index;   ///< Registration index in the OLD version.
    ParamID old_id;      ///< The ID the old version shipped at this slot.
    ParamID new_id;      ///< The ID now at this index (0 for ParamRemoved).
};

/// Diff two parameter descriptors (old vs new) and return every ordering /
/// identity violation, in old-version index order.
///
/// The rule enforced is append-only stability:
///   1. For every index present in BOTH versions, the host-facing ID must be
///      identical (catches re-ordering, insertion-in-the-middle, and re-ID).
///   2. Every ID shipped in the old version must still exist somewhere in the
///      new version (catches removals, including a shrink at the tail).
///
/// Adding new parameters at the end of @p new_params is NOT a violation.
inline std::vector<ParamOrderingViolation> diff_param_ordering(
    std::span<const ParamInfo> old_params,
    std::span<const ParamInfo> new_params) {
    std::vector<ParamOrderingViolation> violations;

    const auto id_present_in_new = [&](ParamID id) {
        for (const auto& p : new_params)
            if (p.id == id) return true;
        return false;
    };

    for (std::size_t i = 0; i < old_params.size(); ++i) {
        const ParamID old_id = old_params[i].id;
        if (i < new_params.size()) {
            const ParamID new_id = new_params[i].id;
            if (new_id != old_id) {
                violations.push_back({ParamOrderingViolation::Kind::IdChangedAtIndex,
                                      i, old_id, new_id});
                // A position mismatch already means this slot moved; if the old
                // ID also vanished entirely, that is the more serious removal —
                // report it too so a caller sees the orphaned automation.
                if (!id_present_in_new(old_id)) {
                    violations.push_back({ParamOrderingViolation::Kind::ParamRemoved,
                                          i, old_id, 0});
                }
            }
        } else {
            // The new version is shorter — this old slot has no counterpart.
            violations.push_back({ParamOrderingViolation::Kind::ParamRemoved,
                                  i, old_id, 0});
        }
    }
    return violations;
}

/// StateStore convenience overload.
inline std::vector<ParamOrderingViolation> diff_param_ordering(
    const StateStore& old_store, const StateStore& new_store) {
    return diff_param_ordering(old_store.all_params(), new_store.all_params());
}

/// True iff @p new_params is an append-only, identity-preserving evolution of
/// @p old_params (no re-order, re-ID, or removal of a shipped parameter).
inline bool param_ordering_stable(std::span<const ParamInfo> old_params,
                                  std::span<const ParamInfo> new_params) {
    return diff_param_ordering(old_params, new_params).empty();
}

/// StateStore convenience overload.
inline bool param_ordering_stable(const StateStore& old_store,
                                  const StateStore& new_store) {
    return diff_param_ordering(old_store, new_store).empty();
}

} // namespace pulp::state
