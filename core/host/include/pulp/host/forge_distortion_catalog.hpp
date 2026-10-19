#pragma once

// Distortion — bake-layer catalog node.
//
// Composes the circuit-modelled clipper family into one node:
//
//   input trim → pre-tone → drive → CLIPPER → post-tone → output trim
//
// with the clipper (and only the clipper) running inside the house
// oversampling wrapper. The tone stages ride at the oversampled rate too, but
// only because they sit between the rails of one prepare/process pair — they are
// linear and carry no antialiasing requirement of their own.
//
// TOPOLOGY IS A REGISTRATION-TIME REALIZATION, like the saturator's curve and
// unlike the compressor's detector mode. `to_ground` and `in_loop` are different
// circuits, not different coefficients: one clips a fixed voltage after a gain
// stage, the other clips inside a feedback loop whose capacitor makes the clip
// frequency-dependent. A baked build authored as an overdrive should stay an
// overdrive.
//
// THE OVERSAMPLING TIER IS ALSO A REALIZATION, and for the harder reason: it
// determines `latency_samples()`. A node whose reported latency moves under the
// audio thread breaks the host's delay compensation. ×1 is the zero-latency
// no antialiasing; ×2/×4/×8 report the composed oversampler's own exact group delay.
//
// MONO, one port in and one out. Every stage here is per-sample with no
// cross-channel state, so a stereo instrument would be two independent copies —
// which is what instancing two nodes already gives.

#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/custom_node_type.hpp>

#include <pulp/signal/distortion.hpp>
#include <pulp/signal/oversampling.hpp>
#include <pulp/signal/units.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace pulp::host::distortion {

using DiodeModel = signal::DiodeModel;
using Topology = signal::ClipperTopology;

/// Oversampling tiers this node registers. ×1 is the zero-latency tier
/// and does NOT antialias.
enum class OversampleTier : std::uint8_t { x1, x2, x4, x8 };

// ── Stable type ids ───────────────────────────────────────────────────────
// One id per (topology, tier) pair, because both are frozen at registration.
inline constexpr const char* kToGroundX1TypeId = "distortion.to_ground.x1";
inline constexpr const char* kToGroundX2TypeId = "distortion.to_ground.x2";
inline constexpr const char* kToGroundX4TypeId = "distortion.to_ground.x4";
inline constexpr const char* kToGroundX8TypeId = "distortion.to_ground.x8";
inline constexpr const char* kInLoopX1TypeId = "distortion.in_loop.x1";
inline constexpr const char* kInLoopX2TypeId = "distortion.in_loop.x2";
inline constexpr const char* kInLoopX4TypeId = "distortion.in_loop.x4";
inline constexpr const char* kInLoopX8TypeId = "distortion.in_loop.x8";

// ── Injectable param ids ──────────────────────────────────────────────────
inline constexpr state::ParamID kDriveDb = 1;       // dB
inline constexpr state::ParamID kSymmetry = 2;      // −1..+1
inline constexpr state::ParamID kDiodeModel = 3;    // stepped 0=Si, 1=Ge, 2=LED
inline constexpr state::ParamID kPreToneHz = 4;     // Hz
inline constexpr state::ParamID kPostToneHz = 5;    // Hz
inline constexpr state::ParamID kToneMix = 6;       // 0..1
inline constexpr state::ParamID kOutputDb = 7;      // dB
// Added after the original seven IDs shipped on the feature branch. Keep it at
// 8 so saved graphs retain the meaning of every existing parameter ID.
inline constexpr state::ParamID kPreGainDb = 8;     // dB

/// Drive range. The primary "how hard" control.
/// [design parameter] default 12 dB, range 0 .. 40 dB.
inline constexpr float kDriveDbMin = 0.0f;
inline constexpr float kDriveDbMax = 40.0f;
inline constexpr float kDriveDbDefault = 12.0f;

/// Output trim. [design parameter] default 0 dB, range −24 .. +12 dB.
inline constexpr float kOutputDbMin = -24.0f;
inline constexpr float kOutputDbMax = 12.0f;

struct DistortionInstance {
    // Two clipper realizations, one selected per registration.
    //
    // `to_ground` uses DiodeClipperT DIRECTLY rather than FeedbackClipperT's
    // to-ground mode, because that mode bundles the op-amp's own Rf/Rin gain —
    // 10.85× at the defaults. Bundling it here would double-count against the
    // node's `drive_db`: the minimum drive setting would already be 20.7 dB of
    // gain, so the node would arrive fully clipped at drive 0 and the knob
    // would do nothing but change how square the result is. The spec's chain is
    // `drive → clipper`, and this is that chain.
    signal::DiodeClipper ground_clipper;
    signal::FeedbackClipper loop_clipper;
    signal::ToneStack tone;
    signal::OversamplerT<float> oversampler;
    float drive = 1.0f;
    float output = 1.0f;
    int latency = 0;
};

/// Returns the integer oversampling factor for a registered tier.
int oversample_factor(OversampleTier tier);

/// Stable type id for a topology/tier realization.
const char* distortion_type_id(Topology topology, OversampleTier tier);

/// Default display name for a topology.
const char* distortion_default_name(Topology topology);

/// Worst-case linear gain for the Forge registry (series law 8).
float distortion_worst_case_gain();

/// One factory, eight registered realizations (2 topologies x 4 tiers).
CustomNodeType make_distortion_node(Topology topology, OversampleTier tier = OversampleTier::x4);

/// Descriptor for the distortion family and its registered realizations.
ForgeNodeDescriptor distortion_descriptor();

/// Reported latency for a tier at the supplied sample rate.
int distortion_latency_samples(OversampleTier tier, double sample_rate);

}  // namespace pulp::host::distortion
