// Shared WAMv2 C ABI entry point for a Pulp WASM *rack* (an in-worklet chain of
// N processors running inside ONE wasm module and ONE AudioWorkletProcessor).
//
// This is the parallel of wam_entry.cpp: it exposes the SAME wam_* C ABI, but
// against a process-global WamChainBridge instead of a WamProcessorBridge. Both
// entries now #include the identical export definitions from wam_entry_abi.inc
// (they differ only in which global bridge PULP_WAM_BRIDGE names), so the two no
// longer carry copy-pasted, hand-synced bodies. A rack module links EXACTLY ONE
// of the two entry TUs (never both — they define the same C symbols), so
// PulpWam.cmake's pulp_add_wam_rack() substitutes this file for wam_entry.cpp.
// Each rack supplies only the stage list:
//
//     std::vector<std::unique_ptr<pulp::format::Processor>> pulp_wam_make_chain();
//
// resolved at link time (exactly one definition per rack executable).
//
// The rack reuses the entire single-plugin wam_* ABI verbatim — NO new export.
// Parameters are addressed "<stage>:<paramId>" (plain "6" still means stage 0);
// state is a "PWR1" container of per-stage "PWS1" blobs; the descriptor is the
// composite of the endpoints. So the four ABI sites PulpWam.cmake tracks
// (this/wam_entry.cpp via wam_entry_abi.inc, the EXPORTED_FUNCTIONS allowlist,
// wam-runtime.mjs makeBridge, wam-processor.js) need no per-rack change: the
// Node runner and the worklet drive a rack through the identical bridge.

#include <pulp/format/web/wam_adapter.hpp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Provided by each rack's entry translation unit (e.g. midi_transpose_mono_synth
// rack.cpp): the ordered stages of the rack.
std::vector<std::unique_ptr<pulp::format::Processor>> pulp_wam_make_chain();

namespace {
// ChainFactory is a plain function pointer; pulp_wam_make_chain decays into it.
// Not invoked until wam_init().
pulp::format::wam::WamChainBridge g_chain(pulp_wam_make_chain);
} // namespace

#define PULP_WAM_BRIDGE g_chain
#include "wam_entry_abi.inc"
#undef PULP_WAM_BRIDGE
