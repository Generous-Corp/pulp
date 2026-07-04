// Phase 0 — parameter-contract equivalence check for DSP hot reload.
// See core/format/include/pulp/format/reload/param_contract.hpp.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/param_contract.hpp>
#include <pulp/state/store.hpp>

using namespace pulp;
using pulp::format::reload::carry_state;
using pulp::format::reload::param_contract_diff;
using pulp::format::reload::param_contracts_match;

namespace {

enum P : state::ParamID { kGain = 1, kFreq = 2, kBypass = 3 };

// Populate a representative 3-param plugin contract. `gain_max` and `freq_name`
// let individual tests perturb exactly one facet. StateStore is non-copyable and
// non-movable, so callers declare it and we fill it in place.
void fill_store(state::StateStore& s, float gain_max = 1.0f,
                const char* freq_name = "Freq", bool bypass_is_trigger = false) {
    s.add_parameter({.id = kGain, .name = "Gain", .unit = "",
                     .range = {0.0f, gain_max, 0.5f, 0.0f}});
    s.add_parameter({.id = kFreq, .name = freq_name, .unit = "Hz",
                     .range = {20.0f, 20000.0f, 440.0f, 0.0f}});
    state::ParamInfo bypass{.id = kBypass, .name = "Bypass", .unit = "",
                            .range = {0.0f, 1.0f, 0.0f, 1.0f}};
    bypass.is_trigger = bypass_is_trigger;
    s.add_parameter(bypass);
}

}  // namespace

TEST_CASE("identical contracts match and carry every value", "[reload][contract]") {
    state::StateStore live; fill_store(live);
    state::StateStore candidate; fill_store(candidate);

    // Operate the live plugin away from defaults.
    live.set_value(kGain, 0.9f);
    live.set_value(kFreq, 1000.0f);
    live.set_value(kBypass, 1.0f);

    REQUIRE(param_contracts_match(live, candidate));
    REQUIRE(param_contract_diff(live, candidate).empty());

    REQUIRE(carry_state(live, candidate) == 3);
    REQUIRE(candidate.get_value(kGain) == 0.9f);
    REQUIRE(candidate.get_value(kFreq) == 1000.0f);
    REQUIRE(candidate.get_value(kBypass) == 1.0f);
}

TEST_CASE("a changed range breaks the contract", "[reload][contract]") {
    state::StateStore live; fill_store(live, /*gain_max=*/1.0f);
    state::StateStore candidate; fill_store(candidate, /*gain_max=*/2.0f);  // gain range widened

    REQUIRE_FALSE(param_contracts_match(live, candidate));
    const auto diff = param_contract_diff(live, candidate);
    REQUIRE(diff.size() == 1);
    REQUIRE(diff[0].find("range/flags changed") != std::string::npos);
}

TEST_CASE("a flipped trigger flag breaks the contract", "[reload][contract]") {
    state::StateStore live; fill_store(live);
    state::StateStore candidate; fill_store(candidate, 1.0f, "Freq", /*bypass_is_trigger=*/true);
    REQUIRE_FALSE(param_contracts_match(live, candidate));
}

TEST_CASE("an added parameter breaks the contract", "[reload][contract]") {
    state::StateStore live; fill_store(live);
    state::StateStore candidate; fill_store(candidate);
    candidate.add_parameter({.id = 4, .name = "Mix", .unit = "%",
                             .range = {0.0f, 100.0f, 50.0f, 0.0f}});
    REQUIRE_FALSE(param_contracts_match(live, candidate));
    const auto diff = param_contract_diff(live, candidate);
    REQUIRE(diff.size() == 1);
    REQUIRE(diff[0].find("added in candidate") != std::string::npos);
}

TEST_CASE("a removed parameter breaks the contract", "[reload][contract]") {
    // Remove a NON-bypass param (kFreq). A removed bypass is adapter-owned and
    // does NOT break the contract (see the dedicated bypass test below); a removed
    // real parameter still does, since automation/sessions are keyed to it.
    state::StateStore live; fill_store(live);
    state::StateStore candidate;
    candidate.add_parameter({.id = kGain, .name = "Gain", .unit = "",
                             .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    state::ParamInfo bypass{.id = kBypass, .name = "Bypass", .unit = "",
                            .range = {0.0f, 1.0f, 0.0f, 1.0f}};
    candidate.add_parameter(bypass);
    // kFreq omitted.
    REQUIRE_FALSE(param_contracts_match(live, candidate));
    const auto diff = param_contract_diff(live, candidate);
    REQUIRE(diff.size() == 1);
    REQUIRE(diff[0].find("removed in candidate") != std::string::npos);
    REQUIRE(diff[0].find("Freq") != std::string::npos);
}

TEST_CASE("a relabelled parameter still matches (name is cosmetic)", "[reload][contract]") {
    state::StateStore live; fill_store(live, 1.0f, "Freq");
    state::StateStore candidate; fill_store(candidate, 1.0f, "Frequency");  // same id/range, new name
    REQUIRE(param_contracts_match(live, candidate));
    REQUIRE(param_contract_diff(live, candidate).empty());
}

TEST_CASE("a changed designation breaks the contract", "[reload][contract]") {
    state::StateStore live; fill_store(live);
    state::StateStore candidate;
    candidate.add_parameter({.id = kGain, .name = "Gain", .unit = "",
                             .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    candidate.add_parameter({.id = kFreq, .name = "Freq", .unit = "Hz",
                             .range = {20.0f, 20000.0f, 440.0f, 0.0f}});
    // Same id/range as live's Bypass, but now declared a Bypass control — a
    // per-block interpretation change the contract must catch.
    state::ParamInfo bypass{.id = kBypass, .name = "Bypass", .unit = "",
                            .range = {0.0f, 1.0f, 0.0f, 1.0f}};
    bypass.designation = state::ParamDesignation::Bypass;
    candidate.add_parameter(bypass);
    REQUIRE_FALSE(param_contracts_match(live, candidate));
}

TEST_CASE("a duplicate-ID store fails the contract closed", "[reload][contract]") {
    state::StateStore live; fill_store(live);
    state::StateStore candidate; fill_store(candidate);
    // Author bug: register kGain twice. The gate must refuse to hot-swap a
    // duplicate-ID store rather than risk a multiset mismatch slipping through.
    candidate.add_parameter({.id = kGain, .name = "Gain (dup)", .unit = "",
                             .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    REQUIRE_FALSE(param_contracts_match(live, candidate));
    REQUIRE_FALSE(param_contracts_match(candidate, live));
}

TEST_CASE("carry_state skips ids the candidate lacks", "[reload][contract]") {
    state::StateStore live; fill_store(live);
    live.set_value(kGain, 0.7f);
    live.set_value(kFreq, 880.0f);

    state::StateStore candidate;  // only kGain
    candidate.add_parameter({.id = kGain, .name = "Gain", .unit = "",
                             .range = {0.0f, 1.0f, 0.5f, 0.0f}});

    REQUIRE(carry_state(live, candidate) == 1);
    REQUIRE(candidate.get_value(kGain) == 0.7f);
}

// ── Superset gate (item 2.4: add params and stay live) ────────────────────────
TEST_CASE("superset gate: an identical contract is a superset with no additions",
          "[reload][contract][2.4]") {
    using pulp::format::reload::param_contract_superset;
    state::StateStore live; fill_store(live);
    state::StateStore candidate; fill_store(candidate);
    auto r = param_contract_superset(live, candidate);
    REQUIRE(r.is_superset);
    REQUIRE(r.added_ids.empty());        // strict match is the empty-additions case
}

TEST_CASE("superset gate: a candidate that ADDS a parameter is a superset",
          "[reload][contract][2.4]") {
    using pulp::format::reload::param_contract_superset;
    state::StateStore live; fill_store(live);            // 3 params
    state::StateStore candidate; fill_store(candidate);  // same 3 ...
    candidate.add_parameter({.id = 4, .name = "Drive", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 0.0f}});  // ... + 1 new
    auto r = param_contract_superset(live, candidate);
    REQUIRE(r.is_superset);
    REQUIRE(r.added_ids == std::vector<state::ParamID>{4});
    // The strict gate still REJECTS it (sizes differ) — superset is the opt-in path.
    REQUIRE_FALSE(param_contracts_match(live, candidate));
}

TEST_CASE("superset gate: removing or re-ranging a live parameter is NOT a superset",
          "[reload][contract][2.4]") {
    using pulp::format::reload::param_contract_superset;
    SECTION("removed") {
        // A removed NON-bypass param is not a superset (breaks automation). A
        // removed bypass is adapter-owned and would still be a superset — covered
        // by the dedicated bypass test — so remove kFreq here.
        state::StateStore live; fill_store(live);            // kGain,kFreq,kBypass
        state::StateStore candidate;                          // missing kFreq
        candidate.add_parameter({.id = kGain, .name = "Gain", .unit = "",
                                 .range = {0.0f, 1.0f, 0.5f, 0.0f}});
        state::ParamInfo bypass{.id = kBypass, .name = "Bypass", .unit = "",
                                .range = {0.0f, 1.0f, 0.0f, 1.0f}};
        candidate.add_parameter(bypass);
        auto r = param_contract_superset(live, candidate);
        REQUIRE_FALSE(r.is_superset);
    }
    SECTION("re-ranged") {
        state::StateStore live; fill_store(live);
        state::StateStore candidate; fill_store(candidate, /*gain_max=*/2.0f);  // kGain bound changed
        candidate.add_parameter({.id = 4, .name = "Drive", .unit = "",
                                 .range = {0.0f, 1.0f, 0.0f, 0.0f}});
        auto r = param_contract_superset(live, candidate);
        REQUIRE_FALSE(r.is_superset);   // a changed shared contract disqualifies it
    }
}

TEST_CASE("adapter-synthesized bypass is excluded from the reload contract",
          "[reload][contract][issue-1_9]") {
    // In a real DAW the adapter SYNTHESIZES a bypass into the live store
    // (HostQuirks::synthesize_bypass_parameter). The reloaded logic's candidate
    // declares only its own params (no bypass). The reload must still be accepted
    // — the bypass is adapter-owned, not the logic's swappable contract.
    // Regression: in-DAW reload was rejected "parameter contract differs" because
    // live carried the synthesized bypass the candidate never declares.
    state::StateStore live;
    live.add_parameter({.id = kGain, .name = "Gain", .unit = "",
                        .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    live.add_parameter({.id = kFreq, .name = "Freq", .unit = "Hz",
                        .range = {20.0f, 20000.0f, 440.0f, 0.0f}});
    state::ParamInfo bypass{.id = kBypass, .name = "Bypass", .unit = "",
                            .range = {0.0f, 1.0f, 0.0f, 1.0f}};
    bypass.designation = state::ParamDesignation::Bypass;
    live.add_parameter(bypass);

    state::StateStore candidate;  // reloaded logic: only its own params, no bypass
    candidate.add_parameter({.id = kGain, .name = "Gain", .unit = "",
                             .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    candidate.add_parameter({.id = kFreq, .name = "Freq", .unit = "Hz",
                             .range = {20.0f, 20000.0f, 440.0f, 0.0f}});

    REQUIRE(param_contracts_match(live, candidate));
    REQUIRE(param_contract_diff(live, candidate).empty());
    // Symmetric: whichever side carries the adapter bypass, the swap is accepted.
    REQUIRE(param_contracts_match(candidate, live));

    // A REAL contract change (a non-bypass param added) must still be rejected.
    candidate.add_parameter({.id = 4, .name = "Extra", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    REQUIRE_FALSE(param_contracts_match(live, candidate));
}
