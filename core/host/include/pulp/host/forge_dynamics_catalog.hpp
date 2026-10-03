#pragma once

// Dynamics — bake-layer catalog nodes.
//
// The home for the compressor family. It starts with the feedforward
// (transparent/modern) design and is named for the family rather than for that
// one member, because the VCA, FET and diode-bridge lineages that follow share
// this file's conventions and differ only in topology and colour stage —
// splitting them across four headers named after their circuits would scatter
// one set of decisions across four places.
//
// CHANNEL COUNT IS PER MEMBER, not per family — the one thing here that is easy
// to get wrong by pattern-matching. The feedforward design is TRUE STEREO, two
// ports in and two out as ONE logical wire, because it genuinely couples the
// channels: `stereo_link` feeds both detectors from the louder channel so a
// hard-panned hit does not pull the stereo image toward centre. Instancing THAT
// dual-mono would silently discard the link, in the direction that sounds fine
// until someone pans something hard.
//
// The three lineage members below are MONO, one port in and one out, and that is
// not an oversight: none of them has a stereo link, so a stereo instrument would
// be exactly two independent copies — which is what instancing two nodes already
// gives, without pretending there is a stereo image to preserve. Their DSP
// headers say the same thing (stereo composes as two instances driven from a
// caller-computed shared detector signal). Giving them two ports would claim a
// coupling that does not exist in the code.
//
// REALIZATION vs INJECTABLE PARAM, applied per member:
//
//   - Anything that moves `latency_samples()` is a REGISTRATION-TIME argument,
//     because a node whose reported latency changes under the audio thread
//     breaks the host's delay compensation. That is why the VCA member takes
//     its lookahead at construction.
//   - A genuine TOPOLOGY change is a realization too. The diode-bridge member's
//     feedback switch moves the detector from the input to the output, which
//     re-maps the static curve — the module ships a separate
//     `static_curve_feedback_db()` accessor precisely because the measured curve
//     is a different function. Automating that would move the measured ratio
//     under the user, so it is two registered type ids instead.
//   - An ANTIALIASING policy is a realization, following the saturator.
//   - Everything that is a coefficient is an injectable param, including the
//     stepped ones. The FET member's five ratio buttons and the VCA member's
//     negative-ratio mode are front-panel switches over one unchanged signal
//     path with invariant latency, so they inject.
//
// The DETECTOR MODE is an injectable param rather than a registration-time
// realization, unlike the saturator's curve. Peak and RMS share one topology
// and one parameter layout — the switch selects which pre-stage feeds the same
// gain computer — so flipping it is a coefficient-level change, not a change of
// what the node IS. It also has no effect on latency, which is what forces the
// saturator's alias policy to be frozen at registration.
//
// WET ONLY: a compressor's output IS the processed signal. Parallel ("New
// York") compression composes this with make_drywet_node(), which is the
// correct shape for it — the dry path in that topology is the mix bus, not
// something this node should own.

#include <pulp/host/detail/forge_dynamics_catalog_descriptor.hpp>
#include <pulp/host/detail/forge_realization_identity.hpp>
#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/diode_bridge_compressor.hpp>
#include <pulp/signal/feedforward_compressor.hpp>
#include <pulp/signal/fet_compressor.hpp>
#include <pulp/signal/true_peak_limiter.hpp>
#include <pulp/signal/vca_compressor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::host::dynamics {

// ── Stable type ids ───────────────────────────────────────────────────────
inline constexpr const char* kFeedforwardCompressorTypeId = "dynamics.feedforward_compressor";

// ── Injectable param ids ──────────────────────────────────────────────────
// Node-local; the framework namespaces per node so two nodes never collide.
inline constexpr state::ParamID kThresholdDb = 1;      // dB
inline constexpr state::ParamID kRatio = 2;            // :1
inline constexpr state::ParamID kKneeDb = 3;           // dB
inline constexpr state::ParamID kAttackMs = 4;         // ms
inline constexpr state::ParamID kReleaseMs = 5;        // ms
inline constexpr state::ParamID kDetectorMode = 6;     // stepped 0 = peak, 1 = RMS
inline constexpr state::ParamID kRmsWindowMs = 7;      // ms
// Param id 8 is intentionally reserved. Lookahead changes the node's latency,
// so it is fixed by the realization factory rather than injectable automation.
inline constexpr state::ParamID kProgramDependent = 9; // stepped 0/1
inline constexpr state::ParamID kMakeupDb = 10;        // dB
inline constexpr state::ParamID kAutoMakeup = 11;      // stepped 0/1
inline constexpr state::ParamID kStereoLink = 12;      // 0..1

/// This node's lookahead ceiling, in ms. The DSP header supports any value up
/// to `kMaxLookaheadMsCeiling`; this is the value THIS node instantiates, which
/// fixes the ring-buffer size at `prepare()` and therefore the largest latency
/// the node can ever report. A future node can bake a larger ceiling without a
/// header change.
/// [design parameter] default 10 ms, range 0 .. 50 ms.
inline constexpr float kNodeMaxLookaheadMs = 10.0f;

struct FeedforwardCompressorInstance {
    signal::FeedforwardCompressor compressor;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The topology is feedforward — the sidechain reads the input, never the
/// output — so there is no loop to bound and no small-signal-gain argument to
/// make. The gain computer can only ever reduce, so the bound is exactly the
/// makeup ceiling: `10^(24/20)`. Asserted directly by the DSP suite's
/// makeup-gain bound test rather than reasoned about here.
float feedforward_compressor_worst_case_gain();

/// The transparent/modern compressor as a lowerable custom node.
///
/// `lookahead_ms` is a construction-time realization axis because it changes
/// intrinsic latency. Non-zero values therefore receive stable, collision-free
/// type identities; the historical zero-lookahead node retains its base id.
CustomNodeType make_feedforward_compressor_node(float lookahead_ms = 0.0f);

namespace true_peak {

inline constexpr const char* kTypeId = "dynamics.true_peak_limiter";
inline constexpr state::ParamID kCeilingDbtp = 1;
inline constexpr state::ParamID kReleaseMs = 2;

struct Instance {
    static constexpr std::size_t kControlTableSize = 4097;
    signal::TruePeakLimiter limiter;
    bool prepared = false;
    std::array<double, kControlTableSize> ceiling_table{};
    std::array<double, kControlTableSize> release_table{};
    float last_ceiling_dbtp = -1.0f;
    float last_release_ms = 100.0f;

    static double table_value(const std::array<double, kControlTableSize>& table, double value,
                              double minimum, double maximum) noexcept;

    void prepare(double sample_rate);

    void set_controls(float ceiling_dbtp, float release_ms) noexcept;
};

CustomNodeType make_node(float lookahead_ms = 5.0f, bool linked = true);

}  // namespace true_peak

// ── The VCA lineage (Blackmer/dbx) ────────────────────────────────────────
//
// One node type. Nothing here changes topology and nothing changes latency
// except the lookahead, which is therefore taken at construction.
namespace vca {

inline constexpr const char* kTypeId = "dynamics.vca_compressor";

// Node-local ids; the framework namespaces per node, so these numbers may
// restart at 1 without colliding with the feedforward member's.
inline constexpr state::ParamID kThresholdDb = 1;     // dB
inline constexpr state::ParamID kRatio = 2;           // :1
inline constexpr state::ParamID kKneeDb = 3;          // dB, 0 = hard, > 0 = OverEasy
inline constexpr state::ParamID kTimeMs = 4;          // ms — ONE control, both directions
inline constexpr state::ParamID kMakeupDb = 5;        // dB
inline constexpr state::ParamID kMix = 6;             // 0..1
inline constexpr state::ParamID kNegativeRatio = 7;   // stepped 0/1 — "infinity+"
inline constexpr state::ParamID kNegRatioAmount = 8;  // :1, negative
inline constexpr state::ParamID kCeilingDb = 9;       // dB, positive magnitude

using Comp = signal::VcaCompressor;

struct Instance {
    signal::VcaCompressor compressor;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// Feedforward topology: the detector reads the input, so there is no loop to
/// bound. The gain command is bounded above by the makeup ceiling and below by
/// `−ceiling_db`, and the DSP suite asserts both rather than assuming them —
/// this is that asserted upper bound, not a fresh estimate.
float vca_compressor_worst_case_gain();

/// `lookahead_ms` is a construction-time argument, not a param: it IS this
/// member's `latency_samples()`. The DSP sizes its ring for the whole declared
/// range at `prepare()`, so any value in range is allocation-free — it is frozen
/// here for the host's sake, not the allocator's.
CustomNodeType make_vca_compressor_node(float lookahead_ms = 0.0f,
                                        double attack_release_k = Comp::kRatioKDefault);

}  // namespace vca

// ── The FET lineage (1176) ────────────────────────────────────────────────
//
// One node type, and no realization axis at all: `latency_samples()` is a
// constant 16 for every parameter setting, so there is nothing here that has to
// be frozen at registration.
namespace fet {

inline constexpr const char* kTypeId = "dynamics.fet_compressor";

inline constexpr state::ParamID kInputGainDb = 1;        // dB — the only lever in
inline constexpr state::ParamID kOutputGainDb = 2;       // dB — makeup
inline constexpr state::ParamID kRatio = 3;              // stepped 0..4, see kRatioSteps
inline constexpr state::ParamID kAttackUs = 4;           // µs
inline constexpr state::ParamID kReleaseMs = 5;          // ms
inline constexpr state::ParamID kKneeDb = 6;             // dB
inline constexpr state::ParamID kTransformerAmount = 7;  // 0..1
inline constexpr state::ParamID kMix = 8;                // 0..1

/// The ratio switch's positions, in injection order: 4:1, 8:1, 12:1, 20:1,
/// all-buttons-in. ABI is the documented distinct circuit state, not a fifth
/// ratio — but in this model it is five coefficient changes over an unchanged
/// signal path with unchanged latency, so it is a stepped PARAM (a front-panel
/// switch a user automates) rather than a fifth registered realization.
inline constexpr float kRatioSteps = 4.0f;

using Comp = signal::FetCompressor;

struct Instance {
    signal::FetCompressor compressor;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// This member has a feedback loop, so series law 8 bites hardest here. The
/// number is NOT an estimate: the DSP exposes a closed-form ℓ∞ bound assembled
/// from its shipped stages — input gain × divider supremum × the resampling
/// pair's ℓ1 product × output gain × transformer supremum — and its suite
/// asserts both that realised gain never exceeds it and that the divider
/// supremum really is exactly 1. Reported at the node's PARAMETER CEILINGS,
/// because a baked param can be automated anywhere in its declared range.
float fet_compressor_worst_case_gain();

CustomNodeType make_fet_compressor_node();

}  // namespace fet

// ── The diode-bridge lineage ──────────────────────────────────────────────
//
// TWO node types, split on the detection topology. See the header note: moving
// the detector from the input to the output re-maps the static curve, which is
// a different design rather than a mode of one.
namespace diode {

/// Feedback detection — the lineage's own topology, and the DSP's default.
inline constexpr const char* kTypeId = "dynamics.diode_bridge_compressor";
/// Feedforward detection — the same bridge, sensed from the input.
inline constexpr const char* kFeedforwardTypeId = "dynamics.diode_bridge_compressor_feedforward";

inline constexpr state::ParamID kThresholdDb = 1;   // dB
inline constexpr state::ParamID kRatio = 2;         // :1, kLimitRatio and above = limit
inline constexpr state::ParamID kKneeDb = 3;        // dB
inline constexpr state::ParamID kAttackMs = 4;      // ms
inline constexpr state::ParamID kReleaseMs = 5;     // ms
inline constexpr state::ParamID kMakeupDb = 6;      // dB, positive only
inline constexpr state::ParamID kCharacter = 7;     // 0..1, bridge + transformer drive
inline constexpr state::ParamID kMixPercent = 8;    // %
inline constexpr state::ParamID kScHpfHz = 9;       // Hz — LF de-sensitisation
inline constexpr state::ParamID kAutoRelease = 10;  // stepped 0/1

using Comp = signal::DiodeBridgeCompressor;

struct Instance {
    signal::DiodeBridgeCompressor compressor;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The DSP's own static bound: makeup ceiling times both transformer brackets'
/// peak gain, with the bridge contributing at most 1 because it is an
/// attenuator. Its suite asserts that bound; this reports it rather than
/// re-deriving it, so the two cannot drift.
float diode_bridge_compressor_worst_case_gain();

/// `feedback` picks the registered type id; `adaa` is the antialiasing policy,
/// frozen at registration following the saturator. ADAA does not move this
/// member's latency — it reports 0 either way, because the bridge is memoryless
/// and the one-poles add phase rather than delay — so the reason it is frozen is
/// the saturator's other one: it is a fidelity/CPU policy for the artifact's
/// life, not a control anyone automates, and flipping it per sample would swap a
/// memoryless evaluation for a difference quotient mid-waveform and click.
CustomNodeType make_diode_bridge_compressor_node(bool feedback = true, bool adaa = true);

}  // namespace diode

}  // namespace pulp::host::dynamics
