#pragma once

// Forge lo-fi DSP catalog — the seed of Forge's bake-layer DSP kit.
//
// A set of LOWERABLE custom nodes that wrap real pulp::signal blocks (plus a few
// small catalog-local primitives) as bake-layer parameter-injectable nodes. Each
// factory returns a CustomNodeType ready to register on a SignalGraph and bake();
// its macro knobs are injectable baked params
// (BakedGraphProcessor::claim_param_injection → ParamInjector). Every
// node is transport-independent and reads its param via the block's BakedParamView
// so control-thread knob turns land sample-accurately in the BAKED process()
// without re-baking — the same primitive the DelayLine node (F2-a) established.
//
// Header-only: the wrapped signal blocks (Svf, WaveShaperT, DryWetMixerT) are all
// header-only templates, so including this pulls in no new link dependency for the
// pulp-host library — only a consumer (test/tool) that includes it needs the
// pulp::signal include path.
//
// Macro-knob mapping (the adapter names are intentionally host-facing; the
// analog VCF implementation and calibration remain public pulp::signal code):
//   * Delay (feedback echo)  → "Time"  = time_ms + "Feedback" = feedback
//                                                     (time_ms per-sample, interpolated)
//   * Filter (Svf)           → "Tone"  = cutoff_hz + "Resonance" = resonance
//                                                     (sample-accurate; mode is
//                                                      fixed per registered type)
//   * Analog VCF             → cutoff + cutoff_mod + resonance + drive
//                                                     (sample-accurate; lives in
//                                                      forge_analog_vcf_catalog.hpp,
//                                                      include it directly)
//   * Waveshaper (tanh)      → "Drive" = drive       (sample-accurate)
//   * Dry/Wet (DryWetMixer)  → "Mix"   = mix         (BLOCK-rate; see note below)
//   * Noise                  → "Hiss"  = level       (sample-accurate)
//   * Bitcrush/decimator     → "Crush" = bit_depth + sample_rate_reduction
//                                                     (sample-accurate)
//   * Trim (gain stage)      → "Level" = gain_db     (sample-accurate)
//   * Ping-pong delay        → "Time"  = time_ms + "Feedback" + "Width"
//                                                     (TRUE STEREO: 2-in/2-out)
//   * Reverb / dynamics      → decay/damping/mix and threshold/ballistics
//   * CV composition pack    → LFO, VCA, envelope follower, filter-CV, delay-CV
//   * Stereo motion pack     → auto-pan, width, phaser
//
// Port arity is declared by each factory. Most effects are mono in/out and are
// instanced dual-mono by Forge. Dry/wet and the CV consumers use a second input
// as a separate signal/control port; the LFO has no input; ping-pong, auto-pan,
// and width use two ports as the L/R halves of one logical stereo wire and are
// instanced once across both rails.
//
// DryWetMixer is block-rate on purpose: DryWetMixerT's public API is block-oriented
// (a scalar set_mix() plus a block mix_wet() over an internal per-channel dry
// buffer) and exposes no per-sample gain hook, so a faithful wrapping applies the
// injected mix value at the block's first sample for the whole block. A mix macro
// is not audio-rate-critical, so block granularity is the right, honest tradeoff —
// unlike a filter cutoff sweep, which this catalog keeps per-sample.

#include <pulp/host/custom_node_type.hpp>

#include <pulp/signal/compressor.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/dither.hpp>
#include <pulp/signal/dry_wet_mixer.hpp>
#include <pulp/signal/mid_side.hpp>
#include <pulp/signal/noise_gate.hpp>
#include <pulp/signal/phaser.hpp>
#include <pulp/signal/reverb.hpp>
#include <pulp/signal/ballistics_filter.hpp>
#include <pulp/signal/svf.hpp>
#include <pulp/signal/waveshaper.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::host::forge_lofi {

// ── Stable type ids ──────────────────────────────────────────────────────
inline constexpr const char* kDelayTypeId      = "forge_lofi_delay";
inline constexpr const char* kFilterTypeId     = "forge_lofi_filter";
inline constexpr const char* kWaveshaperTypeId = "forge_lofi_waveshaper";
inline constexpr const char* kDryWetTypeId     = "forge_lofi_drywet";
inline constexpr const char* kNoiseTypeId      = "forge_lofi_noise";
inline constexpr const char* kBitcrushTypeId   = "forge_lofi_bitcrush";
inline constexpr const char* kBitcrushTpdfTypeId = "forge_lofi_bitcrush_tpdf";
inline constexpr const char* kBitcrushTpdfFirstTypeId = "forge_lofi_bitcrush_tpdf_first";
inline constexpr const char* kBitcrushTpdfSecondTypeId = "forge_lofi_bitcrush_tpdf_second";
inline constexpr const char* kTrimTypeId       = "forge_lofi_trim";
inline constexpr const char* kPingPongTypeId   = "forge_lofi_ping_pong";
inline constexpr const char* kReverbTypeId     = "forge_lofi_reverb";
inline constexpr const char* kCompressorTypeId = "forge_lofi_compressor";
inline constexpr const char* kGateTypeId       = "forge_lofi_gate";

// The filter's response mode is fixed when the node is REGISTERED, not injected:
// each mode is its own registered type, so a baked build's filter character is
// frozen in the artifact and no control-thread write can change it mid-render.
// kFilterTypeId keeps its original identity (lowpass) so artifacts baked before
// the other modes existed keep resolving.
inline constexpr const char* kFilterHighpassTypeId = "forge_lofi_filter_highpass";
inline constexpr const char* kFilterBandpassTypeId = "forge_lofi_filter_bandpass";
inline constexpr const char* kFilterNotchTypeId    = "forge_lofi_filter_notch";

// ── Injectable macro-knob param ids ──────────────────────────────────────
// Node-local; the framework namespaces per node so two nodes never collide.
inline constexpr state::ParamID kDelayTimeMs       = 1;  // "Time"
inline constexpr state::ParamID kDelayFeedback     = 2;  // "Feedback"
inline constexpr state::ParamID kFilterCutoffHz    = 1;  // "Tone"
inline constexpr state::ParamID kFilterResonance   = 2;  // "Resonance"
inline constexpr state::ParamID kWaveshaperDrive   = 1;  // "Drive"
inline constexpr state::ParamID kDryWetMix         = 1;  // "Mix"
inline constexpr state::ParamID kNoiseLevel        = 1;  // "Hiss"
inline constexpr state::ParamID kBitcrushBitDepth  = 1;  // "Crush" (depth)
inline constexpr state::ParamID kBitcrushRateDiv   = 2;  // "Crush" (rate reduction)
inline constexpr state::ParamID kTrimGainDb        = 1;  // "Level"
inline constexpr state::ParamID kPingPongTimeMs    = 1;  // "Time"
inline constexpr state::ParamID kPingPongFeedback  = 2;  // "Feedback"
inline constexpr state::ParamID kPingPongWidth     = 3;  // "Width"
inline constexpr state::ParamID kReverbDecay       = 1;  // "Decay" (RT60 seconds)
inline constexpr state::ParamID kReverbDamping     = 2;  // "Damping"
inline constexpr state::ParamID kReverbMix         = 3;  // "Mix"
inline constexpr state::ParamID kCompThresholdDb   = 1;  // "Threshold"
inline constexpr state::ParamID kCompRatio         = 2;  // "Ratio"
inline constexpr state::ParamID kCompAttackMs      = 3;  // "Attack"
inline constexpr state::ParamID kCompReleaseMs     = 4;  // "Release"
inline constexpr state::ParamID kGateThresholdDb   = 1;  // "Threshold"
inline constexpr state::ParamID kGateAttackMs      = 2;  // "Attack"
inline constexpr state::ParamID kGateHoldMs        = 3;  // "Hold"
inline constexpr state::ParamID kGateReleaseMs     = 4;  // "Release"

// Longest delay the node can address; sizes the bake-time buffer allocation.
inline constexpr float kDelayMaxMs = 2000.0f;

// ── Delay (feedback echo) — "Time" + "Feedback" ──────────────────────────
// A self-contained lo-fi feedback delay: a single interpolated DelayLine with a
// recirculating feedback path, its output being dry + wet (an audible echo you
// can drop straight between audio_in and audio_out). Both knobs are injectable
// on the BAKED graph — no re-bake:
//   * time_ms  (1 .. kDelayMaxMs): the tap position, read per-sample with linear
//     interpolation, so a "Time" sweep glides (the classic tape-delay pitch
//     smear) instead of stepping.
//   * feedback (0 .. 0.95): the recirculation gain; clamped below unity so the
//     tail always decays (no runaway — the verify gate's boundedness check).
// The DelayLine buffer is sized once at prepare() for kDelayMaxMs; time_ms is
// clamped into [1, buffer] each sample, so an injected value can never read past
// the allocation. RT-safe: prepare() allocates, process() is pure arithmetic.
struct DelayInstance {
    signal::DelayLine line;
    double sample_rate = 48000.0;
    int max_delay_samples = 1;
};

CustomNodeType make_delay_node();

// ── Filter (Svf) — "Tone" + "Resonance" ──────────────────────────────────
// One factory, four registered types — one per SVF response mode. The mode is
// a REGISTRATION-time choice rather than a param because it selects which of
// the TPT structure's simultaneous outputs is read: a build authored as a
// bandpass is a bandpass for the artifact's life, and no injected value can
// turn it into something else mid-render.
//
// Resonance (Q) is injectable alongside cutoff, which is what makes a sweep
// sing rather than merely dim: at Q = 12 the response peaks ~25 dB above the
// Q = 0.707 (maximally flat) reference at cutoff. Bandpass at high Q is the
// "telephone"/formant color; notch is the phaser-adjacent hollow.
//
// Cost note: SvfT recomputes its coefficients (one tan()) inside EVERY setter,
// so calling both setters per sample would double the transcendental cost of a
// static-Q sweep. Resonance is therefore written only when the injected value
// actually moves, and cutoff (which recomputes against the current Q) is
// written every sample — one tan() per sample except while Q is in motion.
struct FilterInstance {
    signal::Svf svf;
    double sample_rate = 48000.0;
    float resonance = 0.707f;
};

CustomNodeType make_filter_node(signal::Svf::Mode mode = signal::Svf::Mode::lowpass);

// ── Waveshaper (tanh saturation) — "Drive" ───────────────────────────────
struct WaveshaperInstance {
    signal::WaveShaper ws;  // default curve = tanh_clip
};

CustomNodeType make_waveshaper_node();

// ── Dry/Wet mixer — "Mix" ────────────────────────────────────────────────
// Two input ports: port 0 = dry, port 1 = wet; one output. mix crossfades
// dry→wet at BLOCK rate (see the header note on DryWetMixerT's block API).
struct DryWetInstance {
    signal::DryWetMixer dwm;
};

CustomNodeType make_drywet_node();

// ── Noise (deterministic white) — "Hiss" ─────────────────────────────────
// Adds seeded white noise scaled by `level` to the input, raising the noise
// floor. Deterministic splitmix64 (no clock, no random_device) — the same
// pattern pulp::signal::osc's NoiseSource uses — so a baked render is
// reproducible and the audio path is pure integer arithmetic (RT-safe).
struct NoiseInstance {
    std::uint64_t rng = 0x9E3779B97F4A7C15ull;
    static constexpr std::uint64_t kSeed = 0x9E3779B97F4A7C15ull;
};

// White sample in [-1, 1) from a splitmix64 stream advanced in place.
inline float forge_white_next(std::uint64_t& state) noexcept {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    // Top 24 bits → [0, 2) → [-1, 1).
    return (static_cast<float>(z >> 40) * (1.0f / 8388608.0f)) - 1.0f;
}

CustomNodeType make_noise_node();

// ── Bitcrush / decimator — "Crush" (NEW; no existing block) ───────────────
// Two effects, both injectable:
//   * bit_depth (1..16): mid-tread amplitude quantization to 2^bit_depth levels
//     across [-1, 1]. Fractional depths are honored (exp2), so a "Crush" knob
//     can glide continuously between bit depths.
//   * sample_rate_reduction (1..64): sample-and-hold decimation — a new input is
//     latched only every `reduction` samples; between latches the held value is
//     repeated, so the effective sample rate is SR/reduction. Fractional
//     reductions are honored via a phase accumulator.
// Order: sample-and-hold first (decimate), then quantize the held value — the
// conventional lo-fi topology (decimate → requantize).
struct BitcrushInstance {
    float held = 0.0f;   // last latched (decimated) sample
    float phase = 0.0f;  // sample-and-hold phase accumulator
    signal::DitherQuantizer quantizer;
};

CustomNodeType
make_bitcrush_node(signal::DitherMode dither = signal::DitherMode::none,
                   signal::NoiseShapingOrder shaping = signal::NoiseShapingOrder::none);

// ── Trim (gain stage) — "Level" ──────────────────────────────────────────
// A plain injectable gain in dB. It exists because every level decision in a
// generated chain — the boost in front of a saturator, the make-up behind it,
// the output level of the whole plugin — otherwise has nowhere to live: the
// graph's built-in Gain node freezes its value at build time and cannot be
// bound to a macro. ±24 dB spans "barely nudged" to "slammed into the drive"
// in both directions, and 0 dB is exactly unity, so an untouched Trim is
// bit-transparent.
//
// exp10 is memoized against the last dB value: a Level knob is static for most
// of its life, so the common case costs a compare instead of a pow() per sample,
// while a moving knob still resolves per sample.
struct TrimInstance {
    float last_db = 0.0f;
    float gain = 1.0f;
};

CustomNodeType make_trim_node();

// ── Ping-pong delay — TRUE STEREO, "Time" + "Feedback" + "Width" ─────────
// The one node in this catalog whose two ports are the LEFT and RIGHT halves of
// a single logical wire rather than two logical inputs. It cannot be expressed
// as two independent mono instances: the effect IS the cross-coupling, each
// channel's delay recirculating into the OTHER channel's line, so an echo
// bounces L → R → L → R at `time_ms` intervals.
//
//   wet_l = line_l.read(d);            wet_r = line_r.read(d);
//   line_l.push(in_l + fb * wet_r);    line_r.push(in_r + fb * wet_l);
//
// With signal in the left channel only, echo 1 leaves left, echo 2 leaves right,
// echo 3 left … — the alternation is a property of the topology, not of a pan
// LFO, so it holds at every delay time and feedback setting.
//
// Because the loop traverses BOTH lines before returning to its own channel,
// the round-trip gain is fb², so the tail decays faster than a mono delay at
// the same setting; feedback is still clamped below unity so a runaway is
// structurally impossible.
//
// `width` collapses the bounce toward the centre: 1 keeps the taps hard L/R,
// 0 sums both taps equally into both outputs (a plain stereo delay with no
// bounce), values between crossfade. The dry signal is never touched, so the
// output is dry + wet exactly like the mono delay node.
struct PingPongInstance {
    signal::DelayLine line_l;
    signal::DelayLine line_r;
    double sample_rate = 48000.0;
    int max_delay_samples = 1;
};

CustomNodeType make_ping_pong_node();

// ── Reverb (FDN) — "Decay" + "Damping" + "Mix" ───────────────────────────
// Wraps the SDK's algorithmic reverb (signal::Reverb): a 4-channel feedback
// delay network with a Hadamard (unitary, energy-preserving) mixing matrix.
// Honestly named — it is a good ALGORITHMIC reverb, not a plate/spring/
// convolution emulation — and its three macros are the three controls that
// define a reverb's character:
//   * decay   (0.1 .. 10 s): the RT60. The FDN's per-round feedback is
//     10^(-3·avg_delay/decay), so the tail reaches −60 dB at exactly `decay`
//     seconds by construction — the macro reads directly as reverb time.
//   * damping (0 .. 0.99): a one-pole lowpass in the feedback path; higher
//     values roll the tail's highs off sooner, darkening it — a bright hall at
//     0, a dark room near the top.
//   * mix     (0 .. 1): dry/wet crossfade, applied inside the reverb.
// The feedback is structurally below unity for every finite decay, so the tail
// always decays. The raw FDN can nevertheless build up a large steady-state
// response at the 10 s maximum (stability alone is not a gain bound), so the
// wet path is normalized by 1/48. Across supported audio sample rates this
// conservatively covers the FDN's 1/(1-feedback) induced-gain bound and keeps
// the node below Forge's declared 2x worst-case gain.
//
// Mono in → mono out (dual-mono in the stereo spine): the FDN's two stereo taps
// are summed to one output, and running one instance per channel rail gives a
// naturally decorrelated stereo tail. RT-safe: prepare() sizes the delay lines,
// process() is pure arithmetic. signal::Reverb recomputes its feedback with one
// pow() per sample internally; that is the SDK block's fixed cost and cannot be
// avoided through its API — a reverb tail is not cutoff-sweep-sensitive, so it
// is an honest, acceptable cost. decay and damping are written only when the
// injected value actually moves (the setters are otherwise wasted work); mix is
// a cheap store and is written every sample.
struct ReverbInstance {
    signal::Reverb reverb;
    float last_decay = -1.0f;
    float last_damping = -1.0f;
};

inline constexpr float kReverbWetNormalization = 1.0f / 48.0f;

CustomNodeType make_reverb_node();

// ── Compressor (feed-forward dynamics) — "Threshold/Ratio/Attack/Release" ─
// A real feed-forward compressor: wraps signal::CompressorT unchanged, exposing
// its four musical macros. Above the threshold the gain is reduced by the ratio;
// the reduction follows the attack/release ballistics. No makeup gain is applied
// (makeup_db = 0) — the node's job is to REDUCE level above threshold, never to
// restore it, so a "loud input above threshold" is always measurably quieter and
// the classic transfer function holds: out_db = threshold + (in_db-threshold)/ratio
// for in above the (soft-knee) threshold. Deliberate level lives on the E2 "trim"
// node, so a makeup knob here would be clutter.
//
// RT-safe: prepare() sets the sample rate (lookahead stays 0, so no allocation);
// process() only copies the Params struct and runs the scalar per-sample path.
struct CompressorInstance {
    signal::Compressor comp;
};

CustomNodeType make_compressor_node();

// ── Gate (noise gate with hold) — "Threshold/Attack/Hold/Release" ─────────
// A noise gate / downward expander with a hold stage. The expander transfer
// curve and range are the same as signal::NoiseGateT (fixed ratio 10:1, floor
// -80 dB): below the threshold the gain is pulled down toward the floor. Two
// deliberate differences from wrapping NoiseGateT directly:
//   * HOLD — NoiseGateT has no hold parameter, and a gate without one chatters on
//     material that dips momentarily below threshold. This node keeps the gate
//     fully open for `hold_ms` after the signal was last above threshold, THEN
//     releases. Hold is the fourth macro the dynamics vocabulary expects.
//   * CONVENTIONAL BALLISTICS — attack opens the gate (target rising toward 0 dB),
//     release closes it (target falling toward the floor). This is the standard
//     gate mapping a user/generator expects from "Attack"/"Release"; NoiseGateT's
//     own follower maps the two the other way, so this node is NOT a drop-in of
//     that block — it shares its expansion curve and floor, not its envelope
//     direction.
// The below-threshold expansion curve (gain = -(threshold - in)*(ratio-1),
// floored at the range) is NoiseGateT's; this node adds the hold stage on top.
//
// RT-safe: prepare() only stores the sample rate; process() is pure scalar
// arithmetic (one exp() per block for each coefficient, no allocation).
struct GateInstance {
    double sample_rate = 48000.0;
    float envelope_db = 0.0f;     // current gain-reduction envelope (0 = open)
    float hold_remaining = 0.0f;  // samples left in the hold window
    static constexpr float kRatio = 10.0f;      // gate expansion ratio (NoiseGateT default)
    static constexpr float kRangeDb = -80.0f;   // maximum attenuation (floor)
};

CustomNodeType make_gate_node();

// ═══ CV primitive pack (composition unlock) ═══
// ── CV primitive pack ────────────────────────────────────────────────────
// The composition unlock: control-signal-as-audio-port nodes. An lfo/env_follower
// emits a UNIPOLAR control signal [0, 1] on its audio output; a consumer (vca,
// filter_cv, delay_cv) reads that control signal on a dedicated CV INPUT PORT and
// interprets it per the port's fixed unit. No graph-level modulation edges are
// involved — modulation is ordinary audio topology the graph already routes and
// bakes, so tremolo/auto-wah/chorus/pump become compositions rather than nodes.
inline constexpr const char* kLfoTypeId         = "forge_lofi_lfo";
inline constexpr const char* kVcaTypeId         = "forge_lofi_vca";
inline constexpr const char* kEnvFollowerTypeId = "forge_lofi_env_follower";
inline constexpr const char* kFilterCvTypeId    = "forge_lofi_filter_cv";
inline constexpr const char* kDelayCvTypeId     = "forge_lofi_delay_cv";

// CV pack injectable macros.
inline constexpr state::ParamID kLfoRateHz         = 1;  // LFO "Rate"
inline constexpr state::ParamID kLfoDepth          = 2;  // LFO "Depth"
inline constexpr state::ParamID kLfoShape          = 3;  // LFO "Shape" (0..3)
inline constexpr state::ParamID kVcaGain           = 1;  // VCA base "Gain"
inline constexpr state::ParamID kEnvAttackMs       = 1;  // env-follower "Attack"
inline constexpr state::ParamID kEnvReleaseMs      = 2;  // env-follower "Release"
inline constexpr state::ParamID kEnvSensitivity    = 3;  // env-follower "Sensitivity"
inline constexpr state::ParamID kEnvInvert         = 4;  // env-follower "Invert" (duck)
inline constexpr state::ParamID kFilterCvBaseHz    = 1;  // filter-cv base "Cutoff"
inline constexpr state::ParamID kFilterCvAmountOct = 2;  // filter-cv CV "Amount" (oct)
inline constexpr state::ParamID kFilterCvResonance = 3;  // filter-cv "Resonance"
inline constexpr state::ParamID kDelayCvBaseMs     = 1;  // delay-cv base "Time"
inline constexpr state::ParamID kDelayCvDepthMs    = 2;  // delay-cv CV "Depth" (ms)
inline constexpr state::ParamID kDelayCvFeedback   = 3;  // delay-cv "Feedback"
inline constexpr state::ParamID kDelayCvMix        = 4;  // delay-cv "Mix"

// LFO shape enumeration (rounded from the injectable kLfoShape float).
enum class LfoShape : int { sine = 0, triangle = 1, saw = 2, square = 3 };

// One bipolar [-1, 1] LFO sample for a normalized phase [0, 1) and a shape id.
inline float forge_lfo_osc(float phase, int shape) noexcept {
    switch (shape) {
        case static_cast<int>(LfoShape::triangle):
            return 1.0f - 4.0f * std::fabs(phase - 0.5f);        // /\ bipolar
        case static_cast<int>(LfoShape::saw):
            return 2.0f * phase - 1.0f;                          // rising ramp
        case static_cast<int>(LfoShape::square):
            return phase < 0.5f ? 1.0f : -1.0f;                  // ±1 pulse
        case static_cast<int>(LfoShape::sine):
        default: {
            constexpr float kTwoPi = 6.28318530717958647692f;
            return std::sin(kTwoPi * phase);
        }
    }
}

// ── LFO (control source) — "Rate" + "Depth" + "Shape" ────────────────────
// A 0-input / 1-output control SOURCE: a free-running low-frequency oscillator
// whose output is a UNIPOLAR control signal in [0, 1] on port 0 (an audio port
// carrying CV, not sound). It is meant to feed a CV input port (vca gain,
// filter_cv cutoff, delay_cv time), not the speakers.
//   * rate_hz (0.01 .. 40): oscillation frequency, read per-sample.
//   * depth   (0 .. 1): swing around the 0.5 midpoint; 1 → full [0, 1], 0 → flat 0.5.
//   * shape   (0..3): sine / triangle / saw / square (rounded).
// Output: cv = clamp(0.5 + 0.5·depth·osc(phase), 0, 1). Phase starts at 0 and is
// deterministic, so a baked render is reproducible. RT-safe: pure arithmetic.
struct LfoInstance {
    double sample_rate = 48000.0;
    float phase = 0.0f;  // normalized [0, 1)
};

CustomNodeType make_lfo_node();

// ── VCA (voltage-controlled amplifier) — "Gain" ──────────────────────────
// Two input ports: port 0 = signal (audio), port 1 = gain CV (control, [0, 1]);
// one output. out = signal · gain · clamp(cv, 0, 1), with gain in [0, 1]. It is a
// pure ATTENUATOR — worst-case gain is 1.0 (unity), never a boost — so no CV node
// can amplify a chain; makeup gain is the `trim` node's job. Feeding an lfo's
// output into port 1 yields tremolo; an inverted env_follower yields a self-
// ducking "pump". The CV is clamped to [0, 1] (a control, never a phase inverter
// or a boost). RT-safe: pure arithmetic, no state.
CustomNodeType make_vca_node();

// ── Envelope follower (audio → CV) — "Attack" + "Release" + more ──────────
// One input (audio), one output (a UNIPOLAR control signal). Tracks the input
// level with independent attack/release ballistics and emits it as CV in [0, 1],
// ready to drive a filter_cv cutoff (auto-wah) or a vca gain (dynamics/pump).
//   * attack_ms / release_ms: ballistics, sampled at block rate (a setter
//     recomputes coefficients, so it is not audio-rate — honest and RT-safe).
//   * sensitivity (0.1 .. 8): scales the envelope before clamping to [0, 1].
//   * invert (0/1): when set, emits 1 − env, so a loud input DUCKS the CV
//     (self-pump / ducking when this envelope drives a vca).
struct EnvFollowerInstance {
    signal::BallisticsFilter env;
    float last_attack_ms = -1.0f;
    float last_release_ms = -1.0f;
};

CustomNodeType make_env_follower_node();

// ── Filter with cutoff CV (svf v2) — "Cutoff" + "Amount" + "Resonance" ────
// Two input ports: port 0 = signal (audio), port 1 = cutoff CV (control, [0, 1]);
// one output. A resonant lowpass whose cutoff sweeps with the CV:
//     cutoff_hz = clamp(base · 2^(cv · amount_oct), 20, 0.45·sr)
// so a CV of 0 sits at the base cutoff and a CV of 1 opens `amount_oct` octaves
// above it. env_follower → port 1 gives auto-wah; lfo → port 1 gives a filter
// sweep. cutoff is clamped below Nyquist so the TPT tan() prewarp stays stable at
// any host rate. RT-safe: prepare() sets rate; process() is a per-sample retune.
struct FilterCvInstance {
    signal::Svf svf;
    double sample_rate = 48000.0;
    float last_resonance = -1.0f;
};

CustomNodeType make_filter_cv_node();

// ── Delay with time CV (delay v2) — "Time" + "Depth" + "Feedback" + "Mix" ─
// Two input ports: port 0 = signal (audio), port 1 = time CV (control, [0, 1]);
// one output. An interpolated delay whose tap MOVES with the CV:
//     time_ms = base_ms + cv · depth_ms
// so an lfo on port 1 produces chorus/flanger/vibrato (pitch smear from the moving
// read tap). feedback recirculates (clamped < 1 so the tail always decays) and mix
// blends dry/wet. RT-safe: prepare() allocates; process() is pure arithmetic.
struct DelayCvInstance {
    signal::DelayLine line;
    double sample_rate = 48000.0;
    int max_delay_samples = 1;
};

CustomNodeType make_delay_cv_node();

// ═══ Stereo motion pack (spatial effects) ═══
// Three catalog nodes that MOVE or SHAPE the stereo image rather than colour a
// single channel. Two are TRUE STEREO (2-in/2-out, their ports the L/R halves of
// one logical wire — the cross-channel relationship IS the effect, so they cannot
// be expressed as two independent mono instances); one is dual-mono but carries
// its own internal LFO, so it is self-modulated the way the composition doc's
// "self-modulated node" pattern intends — no graph modulation edges involved.
inline constexpr const char* kAutoPanTypeId = "forge_lofi_auto_pan";
inline constexpr const char* kWidthTypeId   = "forge_lofi_width";
inline constexpr const char* kPhaserTypeId  = "forge_lofi_phaser";

// Stereo-motion injectable macros (node-local ids; the framework namespaces
// per node, so reusing 1..N across nodes never collides).
inline constexpr state::ParamID kAutoPanRateHz  = 1;  // auto-pan LFO "Rate"
inline constexpr state::ParamID kAutoPanDepth   = 2;  // auto-pan "Depth" (pan swing)
inline constexpr state::ParamID kAutoPanShape   = 3;  // auto-pan LFO "Shape" (0..3)
inline constexpr state::ParamID kWidthAmount    = 1;  // width "Width" (0 mono .. 2 wide)
inline constexpr state::ParamID kPhaserRateHz   = 1;  // phaser LFO "Rate"
inline constexpr state::ParamID kPhaserDepth    = 2;  // phaser sweep "Depth"
inline constexpr state::ParamID kPhaserFeedback = 3;  // phaser "Feedback" (resonance)
inline constexpr state::ParamID kPhaserStages   = 4;  // phaser allpass "Stages" (2..8)

// ── Auto-pan — TRUE STEREO, "Rate" + "Depth" + "Shape" ───────────────────
// An LFO drives an equal-power balance across the two channels: as the LFO
// sweeps the pan position from left to right, the left gain follows cos(θ) while
// the right follows sin(θ), so the two channel gains are ANTI-CORRELATED — when
// one rises the other falls. That opposition is the effect and is why it is true
// stereo: a per-channel copy could not see the other channel to oppose it.
//   * rate_hz (0.01 .. 20): LFO frequency, read per-sample.
//   * depth   (0 .. 1): how far the pan swings from centre; 0 pins both gains at
//     the centre value (cos(π/4) = 0.707) → the two channels track together
//     (the static negative control), >0 opens the anti-correlated swing.
//   * shape   (0..3): sine / triangle / saw / square LFO (rounded).
// Equal-power gains never exceed 1, so the node can only attenuate each channel —
// its worst-case multiplicative gain is unity. RT-safe: pure arithmetic; the LFO
// phase starts at 0 so a baked render is reproducible.
struct AutoPanInstance {
    double sample_rate = 48000.0;
    float phase = 0.0f;  // normalized [0, 1)
};

CustomNodeType make_auto_pan_node();

// ── Width — TRUE STEREO, "Width" ─────────────────────────────────────────
// A mid-side width control backed by the shared orthonormal transform. It
// splits stereo into energy-preserving MID/SIDE coordinates, scales SIDE by
// `width`, and applies the inverse transform.
// width = 0 collapses the side entirely → L' = R' = (L + R) / 2, a mono image;
// width = 1 reconstructs the original L/R exactly (unity); width > 1 amplifies
// the side, widening the image past the original. It is true stereo because it
// operates on the SUM and DIFFERENCE of the two channels — the relationship
// between them —
// which a per-channel instance cannot compute.
//   * width (0 .. 2): mono at 0, unity at 1, up to 2× the side energy at 2.
// Worst-case gain: at width = 2, L' = 1.5·L − 0.5·R, whose peak magnitude for
// |L|,|R| ≤ 1 is 2.0. The transform is pure arithmetic with no state, but the
// node still owns a trivial instance so it takes the same instanced code path as
// every other catalog node (RAII create/destroy) rather than a special-case
// stateless path.
struct WidthInstance {};

CustomNodeType make_width_node();

// ── Phaser — allpass-chain sweep, "Rate" + "Depth" + "Feedback" + "Stages" ─
// A classic phaser: a cascade of first-order allpass sections whose corner
// frequency is swept by the node's OWN internal LFO, mixed 50/50 with the dry
// signal so the moving allpass phase carves a set of notches that glide up and
// down the spectrum. It is SELF-MODULATED — the LFO lives inside the node, not on
// a graph modulation edge — so it is dual-mono: one instance per channel rail,
// each seeded identically, which keeps the L/R null test intact.
//   * rate_hz  (0.01 .. 20): sweep LFO frequency.
//   * depth    (0 .. 1): sweep span; 0 pins the sweep at its low corner → the
//     notches sit STILL (the static negative control), >0 sets them gliding.
//   * feedback (0 .. 0.9): allpass recirculation; deepens and sharpens the
//     notches. Clamped below unity so the resonance cannot run away.
//   * stages   (2 .. 8): number of allpass sections (rounded) → number of
//     notches. The wrapped Phaser clamps to its own 2..8 range as well.
// Worst-case gain: the allpass chain is unity-magnitude, so the feedback loop
// sums toward 1/(1 − 0.9) = 10 at resonance; the 50/50 mix halves that, but 10 is
// the conservative upper bound the path-gain lint reads. RT-safe: PhaserT uses
// fixed member storage and denormal-snaps its feedback state.
struct PhaserInstance {
    signal::Phaser phaser;
    double sample_rate = 48000.0;
};

CustomNodeType make_phaser_node();

}  // namespace pulp::host::forge_lofi

#include <pulp/host/detail/forge_lofi_catalog_descriptor.hpp>
