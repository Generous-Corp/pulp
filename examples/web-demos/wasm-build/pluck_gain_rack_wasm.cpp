// Example WAM rack entry TU: pulp-pluck → pulp-gain.
//
// This exists to give the SDK's own CI committed COMPILE + RUNTIME coverage of
// the in-worklet chain path (WamChainBridge, wam_chain_entry.cpp, and the
// pulp_add_wam_rack CMake helper). Without a committed consumer, that path was
// only ever built out-of-tree and would regress silently.
//
// A MIDI note-on drives the pluck instrument (stage 0), whose audio is then
// scaled by the gain effect (stage 1) — so one wasm module exercises MIDI
// routing to stage 0, cross-stage audio flow, per-stage parameter addressing
// ("0:<id>" / "1:<id>"), and the "PWR1" multi-stage state container.
#include "pulp_gain.hpp"
#include "pulp_pluck.hpp"
#include <memory>
#include <vector>

std::vector<std::unique_ptr<pulp::format::Processor>> pulp_wam_make_chain() {
    std::vector<std::unique_ptr<pulp::format::Processor>> stages;
    stages.push_back(pulp::examples::create_pulp_pluck());  // stage 0: instrument
    stages.push_back(pulp::examples::create_pulp_gain());   // stage 1: effect
    return stages;
}
