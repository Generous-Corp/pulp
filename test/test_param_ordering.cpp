// Cross-version parameter-ordering guard (WS-4 / G5).
//
// A plugin's host-facing parameter identity (== state::ParamID, which maps 1:1
// onto VST3 ParameterInfo::id, AU AudioUnitParameterID, and CLAP
// clap_param_info_t::id) must stay stable across releases, and previously
// shipped parameters must keep their registration index. diff_param_ordering()
// is the author-side gate that fails a build when a descriptor edit would
// silently remap or clip automation users already recorded.
#include <catch2/catch_test_macros.hpp>

#include <pulp/state/param_ordering.hpp>
#include <pulp/state/store.hpp>

using namespace pulp::state;
using Kind = ParamOrderingViolation::Kind;

namespace {

enum P : ParamID { kGain = 10, kFreq = 20, kMix = 30, kDrive = 40 };

// v1 descriptor: three parameters at indices 0,1,2.
void fill_v1(StateStore& s) {
    s.add_parameter({.id = kGain, .name = "Gain", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    s.add_parameter({.id = kFreq, .name = "Freq", .unit = "Hz",
                     .range = {20.0f, 20000.0f, 440.0f, 0.0f}});
    s.add_parameter({.id = kMix, .name = "Mix", .unit = "%",
                     .range = {0.0f, 100.0f, 50.0f, 0.0f}});
}

}  // namespace

TEST_CASE("param ordering: identical descriptors are stable", "[state][ordering]") {
    StateStore a; fill_v1(a);
    StateStore b; fill_v1(b);
    REQUIRE(param_ordering_stable(a, b));
    REQUIRE(diff_param_ordering(a, b).empty());
}

TEST_CASE("param ordering: appending a new parameter at the end is safe",
          "[state][ordering]") {
    StateStore v1; fill_v1(v1);
    StateStore v2; fill_v1(v2);
    // v2 adds a fourth parameter at the tail — the only always-safe evolution.
    v2.add_parameter({.id = kDrive, .name = "Drive", .range = {0.0f, 1.0f, 0.0f, 0.0f}});

    REQUIRE(param_ordering_stable(v1, v2));
    REQUIRE(diff_param_ordering(v1, v2).empty());
    // And it is directional: dropping back to v1 from v2 IS a removal.
    REQUIRE_FALSE(param_ordering_stable(v2, v1));
}

TEST_CASE("param ordering: inserting a parameter in the middle shifts indices",
          "[state][ordering]") {
    StateStore v1; fill_v1(v1);
    StateStore v2;
    v2.add_parameter({.id = kGain, .name = "Gain", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    // Inserted between Gain and Freq — shoves Freq (idx1) and Mix (idx2) down.
    v2.add_parameter({.id = kDrive, .name = "Drive", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    v2.add_parameter({.id = kFreq, .name = "Freq", .unit = "Hz",
                      .range = {20.0f, 20000.0f, 440.0f, 0.0f}});
    v2.add_parameter({.id = kMix, .name = "Mix", .unit = "%",
                      .range = {0.0f, 100.0f, 50.0f, 0.0f}});

    const auto violations = diff_param_ordering(v1, v2);
    REQUIRE_FALSE(violations.empty());
    // Index 1 used to be Freq (kFreq), now holds Drive (kDrive).
    REQUIRE(violations[0].kind == Kind::IdChangedAtIndex);
    REQUIRE(violations[0].index == 1);
    REQUIRE(violations[0].old_id == kFreq);
    REQUIRE(violations[0].new_id == kDrive);
    // Index 2 used to be Mix (kMix), now holds Freq (kFreq).
    REQUIRE(violations[1].kind == Kind::IdChangedAtIndex);
    REQUIRE(violations[1].index == 2);
    REQUIRE(violations[1].old_id == kMix);
    REQUIRE(violations[1].new_id == kFreq);
}

TEST_CASE("param ordering: re-ID'ing a parameter in place is a violation",
          "[state][ordering]") {
    StateStore v1; fill_v1(v1);
    StateStore v2;
    v2.add_parameter({.id = kGain, .name = "Gain", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    v2.add_parameter({.id = kFreq, .name = "Freq", .unit = "Hz",
                      .range = {20.0f, 20000.0f, 440.0f, 0.0f}});
    // Same slot (idx 2), but Mix's ID changed 30 -> 99. Automation keyed to 30
    // is now orphaned AND the slot points at a different ID.
    v2.add_parameter({.id = 99, .name = "Mix", .unit = "%",
                      .range = {0.0f, 100.0f, 50.0f, 0.0f}});

    const auto violations = diff_param_ordering(v1, v2);
    REQUIRE(violations.size() == 2);
    REQUIRE(violations[0].kind == Kind::IdChangedAtIndex);
    REQUIRE(violations[0].index == 2);
    REQUIRE(violations[0].old_id == kMix);
    REQUIRE(violations[0].new_id == 99);
    // The old ID vanished entirely, so a removal is also reported.
    REQUIRE(violations[1].kind == Kind::ParamRemoved);
    REQUIRE(violations[1].old_id == kMix);
}

TEST_CASE("param ordering: removing the tail parameter is a removal",
          "[state][ordering]") {
    StateStore v1; fill_v1(v1);
    StateStore v2;
    v2.add_parameter({.id = kGain, .name = "Gain", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    v2.add_parameter({.id = kFreq, .name = "Freq", .unit = "Hz",
                      .range = {20.0f, 20000.0f, 440.0f, 0.0f}});
    // Mix (idx 2) dropped.
    const auto violations = diff_param_ordering(v1, v2);
    REQUIRE(violations.size() == 1);
    REQUIRE(violations[0].kind == Kind::ParamRemoved);
    REQUIRE(violations[0].index == 2);
    REQUIRE(violations[0].old_id == kMix);
    REQUIRE(violations[0].new_id == 0);
}

TEST_CASE("param ordering: reordering two parameters is caught at both slots",
          "[state][ordering]") {
    StateStore v1; fill_v1(v1);
    StateStore v2;
    // Freq and Gain swapped positions; both keep their IDs but move index.
    v2.add_parameter({.id = kFreq, .name = "Freq", .unit = "Hz",
                      .range = {20.0f, 20000.0f, 440.0f, 0.0f}});
    v2.add_parameter({.id = kGain, .name = "Gain", .range = {0.0f, 1.0f, 0.5f, 0.0f}});
    v2.add_parameter({.id = kMix, .name = "Mix", .unit = "%",
                      .range = {0.0f, 100.0f, 50.0f, 0.0f}});

    const auto violations = diff_param_ordering(v1, v2);
    REQUIRE(violations.size() == 2);
    REQUIRE(violations[0].index == 0);
    REQUIRE(violations[0].old_id == kGain);
    REQUIRE(violations[0].new_id == kFreq);
    REQUIRE(violations[1].index == 1);
    REQUIRE(violations[1].old_id == kFreq);
    REQUIRE(violations[1].new_id == kGain);
    // Both IDs still exist, so neither reorder produces a spurious ParamRemoved.
    for (const auto& v : violations)
        REQUIRE(v.kind == Kind::IdChangedAtIndex);
}
