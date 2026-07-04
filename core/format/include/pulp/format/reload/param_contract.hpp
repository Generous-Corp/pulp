#pragma once

/// @file param_contract.hpp
/// Parameter-contract equivalence check for DSP hot reload (v2 plan §4.5 / Phase 0).
///
/// A reload builds the candidate logic's parameters into a *scratch* StateStore,
/// separate from the live one. Before swapping, the candidate's parameter
/// contract must be proven equivalent to the running plugin's: the host has
/// automation lanes, saved sessions, and a generic editor all keyed to the live
/// parameter IDs and their normalization ranges. If the candidate added,
/// removed, re-ID'd, or re-ranged a parameter, a hot-swap would silently remap
/// or clip automation — that case is a "requires full reload" boundary, not a
/// hot-swap.
///
/// The contract compared is the *automatable surface*: the set of parameter IDs
/// and, per ID, the normalization range (min/max/default/step/skew) plus the
/// trigger flag (which changes how a value is interpreted). Display-only fields
/// (name, unit) are intentionally NOT part of the contract — relabelling a knob
/// is safe to hot-swap. When the contracts match, carry_state() copies the live
/// values into the candidate so the swap is seamless.
///
/// Dependency-light by design, mirroring build_fingerprint.hpp.

#include <pulp/state/parameter.hpp>
#include <pulp/state/store.hpp>

#include <string>
#include <vector>

namespace pulp::format::reload {

/// True iff @p a and @p b describe the same normalization + interpretation
/// contract for one parameter. Ranges are author-specified literals, so exact
/// float equality is intended — a changed bound must be caught.
inline bool param_contract_equal(const state::ParamInfo& a, const state::ParamInfo& b) {
    return a.id == b.id &&
           a.range.min == b.range.min &&
           a.range.max == b.range.max &&
           a.range.default_value == b.range.default_value &&
           a.range.step == b.range.step &&
           a.range.skew == b.range.skew &&
           a.range.symmetric_skew == b.range.symmetric_skew &&
           a.is_trigger == b.is_trigger &&
           // Designation (Bypass / Reset / None) changes how the host and
           // adapters interpret the value per block, so it is part of the
           // contract alongside is_trigger.
           a.designation == b.designation;
}

/// True iff @p store registers each ParamID at most once. A duplicate-ID store
/// is a plugin authoring bug; the contract gate fails closed on it (below)
/// rather than risk a multiset mismatch slipping through.
inline bool has_unique_param_ids(const state::StateStore& store) {
    const auto params = store.all_params();
    for (std::size_t i = 0; i < params.size(); ++i)
        for (std::size_t j = i + 1; j < params.size(); ++j)
            if (params[i].id == params[j].id) return false;
    return true;
}

/// Human-readable contract differences (diagnostics). Empty == equivalent.
/// Reports parameters only in live (removed), only in candidate (added), and
/// IDs present in both whose contract changed. Order-independent (keyed by ID).
inline std::vector<std::string> param_contract_diff(const state::StateStore& live,
                                                    const state::StateStore& candidate) {
    std::vector<std::string> out;
    // Bypass parameters are excluded from the contract: an adapter SYNTHESIZES a
    // bypass into the live store to satisfy a host bypass convention (VST3/AU/CLAP
    // via HostQuirks::synthesize_bypass_parameter, quirk_apply.hpp), so the live
    // store carries a bypass the reloaded logic's define_parameters() never
    // declares. Comparing it would make EVERY in-DAW reload of such a plugin fail
    // "contract differs" (the reload the standalone/capture host accepts) even
    // though the logic's automatable surface is unchanged. The adapter owns the
    // bypass independently of the logic, so it is not part of the swap contract.
    for (const auto& l : live.all_params()) {
        const state::ParamInfo* c = candidate.info(l.id);
        if (!c) {
            // A bypass present in live but not the candidate is adapter-owned
            // (synthesized) — its absence in the logic's contract is not a
            // violation. A non-bypass removal still breaks automation.
            if (state::is_bypass_param(l)) continue;
            out.push_back("parameter " + std::to_string(l.id) + " ('" + l.name +
                          "') removed in candidate");
        } else if (!param_contract_equal(l, *c)) {
            // Present on BOTH sides → still compared (a real bypass whose range/
            // flags changed is a genuine contract change).
            out.push_back("parameter " + std::to_string(l.id) + " ('" + l.name +
                          "') range/flags changed");
        }
    }
    for (const auto& c : candidate.all_params()) {
        if (!live.info(c.id)) {
            if (state::is_bypass_param(c)) continue;  // candidate-only bypass is adapter-ownable
            out.push_back("parameter " + std::to_string(c.id) + " ('" + c.name +
                          "') added in candidate");
        }
    }
    return out;
}

/// Count parameters that are part of the swap contract (i.e. excluding
/// adapter-synthesized bypass params — see param_contract_diff).
inline std::size_t contract_param_count(const state::StateStore& store) {
    std::size_t n = 0;
    for (const auto& p : store.all_params())
        if (!state::is_bypass_param(p)) ++n;
    return n;
}

/// The gate: the candidate's parameter contract is equivalent to the live one
/// (same set of IDs, same range/flags per ID). True == safe to hot-swap and
/// carry state; false == requires a full reload.
inline bool param_contracts_match(const state::StateStore& live,
                                   const state::StateStore& candidate) {
    // Fail closed on duplicate IDs: with duplicates the size + set-membership
    // checks below can't distinguish multisets (e.g. {1,1,2} vs {1,2,2}), so a
    // duplicate-ID store (a plugin bug) must force a full reload, not hot-swap.
    return has_unique_param_ids(live) && has_unique_param_ids(candidate) &&
           contract_param_count(live) == contract_param_count(candidate) &&
           param_contract_diff(live, candidate).empty();
}

/// Result of the SUPERSET contract check (live-swap plan item 2.4 — "add params
/// and stay live"). A candidate is a valid superset when every LIVE parameter is
/// present in the candidate with an identical contract (existing automation /
/// saved sessions stay valid), and the candidate may ALSO declare new parameters.
struct ParamSupersetResult {
    bool is_superset = false;                ///< candidate ⊇ live, shared contracts identical
    std::vector<state::ParamID> added_ids;   ///< IDs new in candidate (candidate order)
};

/// Superset gate: relaxes `param_contracts_match` from "identical" to "candidate
/// is a superset of live". A strict match is the special case `is_superset==true
/// && added_ids.empty()`. When `is_superset` is true with non-empty `added_ids`,
/// the reload can hot-swap AND the host must then rescan the parameter list to
/// pick up the additions (the live-store registration + per-adapter host-notify
/// is item 2.4b — this predicate is pure and mutates nothing). Fails closed on
/// duplicate IDs, exactly like the strict gate.
inline ParamSupersetResult param_contract_superset(const state::StateStore& live,
                                                    const state::StateStore& candidate) {
    ParamSupersetResult result;
    if (!has_unique_param_ids(live) || !has_unique_param_ids(candidate))
        return result;  // is_superset stays false
    // Every live parameter must survive unchanged in the candidate — a removed or
    // re-ranged/re-flagged parameter would break automation, so it is NOT a
    // superset (that case still requires a full reload).
    for (const auto& l : live.all_params()) {
        if (state::is_bypass_param(l)) continue;  // adapter-owned; see param_contract_diff
        const state::ParamInfo* c = candidate.info(l.id);
        if (!c || !param_contract_equal(l, *c))
            return result;
    }
    // Collect the parameters the candidate adds (present in candidate, not live).
    for (const auto& c : candidate.all_params()) {
        if (state::is_bypass_param(c)) continue;
        if (!live.info(c.id))
            result.added_ids.push_back(c.id);
    }
    result.is_superset = true;
    return result;
}

/// Copy each live parameter value into @p candidate for IDs the candidate also
/// defines, so a hot-swap preserves the current sound. Returns the number of
/// values carried. Caller should gate this on param_contracts_match(); it is
/// safe to call regardless (it simply skips IDs the candidate lacks).
///
/// NOTE: this is the ALTERNATE state-continuity model, for hosts that give each
/// processor its OWN StateStore. The canonical standalone-shell path
/// (reload_transaction.hpp) instead binds the new processor directly to the
/// single live store and does not call this — see that file's header.
inline std::size_t carry_state(const state::StateStore& live, state::StateStore& candidate) {
    std::size_t carried = 0;
    for (const auto& l : live.all_params()) {
        if (candidate.info(l.id)) {
            candidate.set_value(l.id, live.get_value(l.id));
            ++carried;
        }
    }
    return carried;
}

} // namespace pulp::format::reload
