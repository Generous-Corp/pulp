#pragma once

// Forge lo-fi DSP catalog — the seed of Forge's bake-layer DSP kit.
//
// A small set of LOWERABLE custom nodes that wrap real pulp::signal blocks (plus
// one new bitcrush/decimator that has no existing block) as bake-layer
// parameter-injectable nodes. Each factory returns a CustomNodeType ready to
// register on a SignalGraph and bake(); its one "macro knob" is an injectable
// baked param (BakedGraphProcessor::claim_param_injection → ParamInjector). Every
// node is transport-independent and reads its param via the block's BakedParamView
// so control-thread knob turns land sample-accurately in the BAKED process()
// without re-baking — the same primitive the DelayLine node (F2-a) established.
//
// Header-only: the wrapped signal blocks (Svf, WaveShaperT, DryWetMixerT) are all
// header-only templates, so including this pulls in no new link dependency for the
// pulp-host library — only a consumer (test/tool) that includes it needs the
// pulp::signal include path.
//
// Macro-knob mapping:
//   * Delay (feedback echo)  → "Time"  = time_ms + "Feedback" = feedback
//                                                     (time_ms per-sample, interpolated)
//   * Filter (Svf lowpass)   → "Tone"  = cutoff_hz   (sample-accurate)
//   * Waveshaper (tanh)      → "Drive" = drive       (sample-accurate)
//   * Dry/Wet (DryWetMixer)  → "Mix"   = mix         (BLOCK-rate; see note below)
//   * Noise                  → "Hiss"  = level       (sample-accurate)
//   * Bitcrush/decimator     → "Crush" = bit_depth + sample_rate_reduction
//                                                     (sample-accurate)
//   * Compressor (CompressorT)→ threshold + ratio + attack + release  (BLOCK-rate)
//   * Gate (NoiseGate + hold) → threshold + attack + hold + release   (BLOCK-rate)
//
// The dynamics nodes (compressor, gate) are BLOCK-rate for the same honest reason
// as Dry/Wet: their macro knobs configure envelope BALLISTICS (attack/release
// times, threshold, ratio), not audio-rate signal content, and the block's own
// envelope follower smooths any step. A control-thread knob turn lands at the
// block's first sample; the per-sample audio path stays the block's real detector.
//
// DryWetMixer is block-rate on purpose: DryWetMixerT's public API is block-oriented
// (a scalar set_mix() plus a block mix_wet() over an internal per-channel dry
// buffer) and exposes no per-sample gain hook, so a faithful wrapping applies the
// injected mix value at the block's first sample for the whole block. A mix macro
// is not audio-rate-critical, so block granularity is the right, honest tradeoff —
// unlike a filter cutoff sweep, which this catalog keeps per-sample.

#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/compressor.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/dry_wet_mixer.hpp>
#include <pulp/signal/noise_gate.hpp>
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
inline constexpr const char* kCompressorTypeId = "forge_lofi_compressor";
inline constexpr const char* kGateTypeId       = "forge_lofi_gate";

// ── Injectable macro-knob param ids ──────────────────────────────────────
// Node-local; the framework namespaces per node so two nodes never collide.
inline constexpr state::ParamID kDelayTimeMs       = 1;  // "Time"
inline constexpr state::ParamID kDelayFeedback     = 2;  // "Feedback"
inline constexpr state::ParamID kFilterCutoffHz    = 1;  // "Tone"
inline constexpr state::ParamID kWaveshaperDrive   = 1;  // "Drive"
inline constexpr state::ParamID kDryWetMix         = 1;  // "Mix"
inline constexpr state::ParamID kNoiseLevel        = 1;  // "Hiss"
inline constexpr state::ParamID kBitcrushBitDepth  = 1;  // "Crush" (depth)
inline constexpr state::ParamID kBitcrushRateDiv   = 2;  // "Crush" (rate reduction)
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

inline CustomNodeType make_delay_node() {
    CustomNodeType t;
    t.type_id = kDelayTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Delay";
    t.lowerable = true;
    t.create = []() -> void* { return new DelayInstance{}; };
    t.destroy = [](void* p) { delete static_cast<DelayInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<DelayInstance*>(p);
        s->sample_rate = sr;
        s->max_delay_samples =
            std::max(1, static_cast<int>(std::ceil(kDelayMaxMs * 0.001 * sr)));
        s->line.prepare(s->max_delay_samples);
    };
    t.reset = [](void* p) { static_cast<DelayInstance*>(p)->line.reset(); };
    // time_ms: 1 ms .. 2 s, default 250 ms. feedback: 0 .. 0.95, default 0.5.
    t.baked_params.push_back({kDelayTimeMs, 1.0f, kDelayMaxMs, 250.0f});
    t.baked_params.push_back({kDelayFeedback, 0.0f, 0.95f, 0.5f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<DelayInstance*>(p);
            const float* i = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            const float sr_per_ms = static_cast<float>(s->sample_rate) * 0.001f;
            const float max_d = static_cast<float>(s->max_delay_samples);
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float time_ms = params.value_at(kDelayTimeMs, off);
                float fb = params.value_at(kDelayFeedback, off);
                fb = std::clamp(fb, 0.0f, 0.95f);

                float delay_samples = time_ms * sr_per_ms;
                delay_samples = std::clamp(delay_samples, 1.0f, max_d);

                const float dry = i[static_cast<std::size_t>(k)];
                const float wet = s->line.read(delay_samples);
                s->line.push(dry + fb * wet);           // recirculate
                o[static_cast<std::size_t>(k)] = dry + wet;  // dry + echo
            }
        };
    return t;
}

// ── Filter (Svf lowpass) — "Tone" ────────────────────────────────────────
struct FilterInstance {
    signal::Svf svf;
    double sample_rate = 48000.0;
};

inline CustomNodeType make_filter_node() {
    CustomNodeType t;
    t.type_id = kFilterTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Tone";
    t.lowerable = true;
    t.create = []() -> void* { return new FilterInstance{}; };
    t.destroy = [](void* p) { delete static_cast<FilterInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<FilterInstance*>(p);
        s->sample_rate = sr;
        s->svf.set_sample_rate(static_cast<float>(sr));
        s->svf.set_resonance(0.707f);            // Butterworth-ish, no peak
        s->svf.set_mode(signal::Svf::Mode::lowpass);
    };
    t.reset = [](void* p) { static_cast<FilterInstance*>(p)->svf.reset(); };
    // cutoff_hz: 20 Hz .. 20 kHz, default fully open (transparent until turned).
    t.baked_params.push_back({kFilterCutoffHz, 20.0f, 20000.0f, 20000.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<FilterInstance*>(p);
            const float* i = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            for (int k = 0; k < n; ++k) {
                const float cutoff =
                    params.value_at(kFilterCutoffHz, static_cast<std::int32_t>(k));
                s->svf.set_frequency(cutoff);  // sample-accurate retune (a tan()/sample)
                o[static_cast<std::size_t>(k)] =
                    s->svf.process(i[static_cast<std::size_t>(k)]);
            }
        };
    return t;
}

// ── Waveshaper (tanh saturation) — "Drive" ───────────────────────────────
struct WaveshaperInstance {
    signal::WaveShaper ws;  // default curve = tanh_clip
};

inline CustomNodeType make_waveshaper_node() {
    CustomNodeType t;
    t.type_id = kWaveshaperTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Drive";
    t.lowerable = true;
    t.create = []() -> void* { return new WaveshaperInstance{}; };
    t.destroy = [](void* p) { delete static_cast<WaveshaperInstance*>(p); };
    // drive: unity (transparent-ish) .. 64x (hard saturation), default unity.
    t.baked_params.push_back({kWaveshaperDrive, 1.0f, 64.0f, 1.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<WaveshaperInstance*>(p);
            const float* i = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            for (int k = 0; k < n; ++k) {
                const float drive =
                    params.value_at(kWaveshaperDrive, static_cast<std::int32_t>(k));
                s->ws.set_drive(drive);
                o[static_cast<std::size_t>(k)] =
                    s->ws.process(i[static_cast<std::size_t>(k)]);
            }
        };
    return t;
}

// ── Dry/Wet mixer — "Mix" ────────────────────────────────────────────────
// Two input ports: port 0 = dry, port 1 = wet; one output. mix crossfades
// dry→wet at BLOCK rate (see the header note on DryWetMixerT's block API).
struct DryWetInstance {
    signal::DryWetMixer dwm;
};

inline CustomNodeType make_drywet_node() {
    CustomNodeType t;
    t.type_id = kDryWetTypeId;
    t.version = 1;
    t.num_input_ports = 2;  // 0 = dry, 1 = wet
    t.num_output_ports = 1;
    t.default_name = "Mix";
    t.lowerable = true;
    t.create = []() -> void* { return new DryWetInstance{}; };
    t.destroy = [](void* p) { delete static_cast<DryWetInstance*>(p); };
    t.prepare = [](void* p, double /*sr*/, int max_block) {
        static_cast<DryWetInstance*>(p)->dwm.prepare(/*max_channels=*/1, max_block);
    };
    t.reset = [](void* p) { static_cast<DryWetInstance*>(p)->dwm.reset(); };
    // mix: 0 (fully dry) .. 1 (fully wet), default centered.
    t.baked_params.push_back({kDryWetMix, 0.0f, 1.0f, 0.5f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<DryWetInstance*>(p);
            const float* dry = in.channel_ptr(0);
            const float* wet = in.channel_ptr(1);
            float* o = out.channel_ptr(0);
            // Block-rate: sample the injected mix at the block's first sample.
            s->dwm.set_mix(params.value_at(kDryWetMix, 0));
            for (int k = 0; k < n; ++k) o[static_cast<std::size_t>(k)] = wet[static_cast<std::size_t>(k)];
            s->dwm.push_dry(&dry, 1, n);   // stores the dry channel (latency 0 → memcpy)
            s->dwm.mix_wet(&o, 1, n);      // blends dry·(1-mix) + wet·mix in place
        };
    return t;
}

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

inline CustomNodeType make_noise_node() {
    CustomNodeType t;
    t.type_id = kNoiseTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Hiss";
    t.lowerable = true;
    t.create = []() -> void* { return new NoiseInstance{}; };
    t.destroy = [](void* p) { delete static_cast<NoiseInstance*>(p); };
    t.reset = [](void* p) { static_cast<NoiseInstance*>(p)->rng = NoiseInstance::kSeed; };
    // level: 0 (clean) .. 1 (full-scale hiss), default clean.
    t.baked_params.push_back({kNoiseLevel, 0.0f, 1.0f, 0.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<NoiseInstance*>(p);
            const float* i = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            for (int k = 0; k < n; ++k) {
                const float level =
                    params.value_at(kNoiseLevel, static_cast<std::int32_t>(k));
                o[static_cast<std::size_t>(k)] =
                    i[static_cast<std::size_t>(k)] + level * forge_white_next(s->rng);
            }
        };
    return t;
}

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
};

inline CustomNodeType make_bitcrush_node() {
    CustomNodeType t;
    t.type_id = kBitcrushTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Crush";
    t.lowerable = true;
    t.create = []() -> void* { return new BitcrushInstance{}; };
    t.destroy = [](void* p) { delete static_cast<BitcrushInstance*>(p); };
    t.reset = [](void* p) {
        auto* s = static_cast<BitcrushInstance*>(p);
        s->held = 0.0f;
        s->phase = 0.0f;
    };
    // bit_depth: 16 (near-lossless) down to 1 (3-level) — default lossless.
    t.baked_params.push_back({kBitcrushBitDepth, 1.0f, 16.0f, 16.0f});
    // sample_rate_reduction: 1 (full rate) .. 64 — default full rate.
    t.baked_params.push_back({kBitcrushRateDiv, 1.0f, 64.0f, 1.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<BitcrushInstance*>(p);
            const float* i = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float bits = params.value_at(kBitcrushBitDepth, off);
                float reduction = params.value_at(kBitcrushRateDiv, off);
                if (reduction < 1.0f) reduction = 1.0f;

                // Sample-and-hold decimation: latch a fresh input every
                // `reduction` samples; repeat the held value in between.
                s->phase += 1.0f;
                if (s->phase >= reduction) {
                    s->phase -= reduction;
                    s->held = i[static_cast<std::size_t>(k)];
                }

                // Mid-tread quantization to 2^bits levels across [-1, 1].
                const float scale = std::exp2(bits - 1.0f);  // 2^(bits-1)
                o[static_cast<std::size_t>(k)] = std::round(s->held * scale) / scale;
            }
        };
    return t;
}

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

inline CustomNodeType make_compressor_node() {
    CustomNodeType t;
    t.type_id = kCompressorTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Comp";
    t.lowerable = true;
    t.create = []() -> void* { return new CompressorInstance{}; };
    t.destroy = [](void* p) { delete static_cast<CompressorInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<CompressorInstance*>(p)->comp.set_sample_rate(
            static_cast<float>(sr));
    };
    t.reset = [](void* p) { static_cast<CompressorInstance*>(p)->comp.reset(); };
    // threshold_db: -60 .. 0, default -18. ratio: 1 (transparent) .. 20, default 4.
    // attack_ms: 0.1 .. 100, default 10. release_ms: 10 .. 1000, default 150.
    t.baked_params.push_back({kCompThresholdDb, -60.0f, 0.0f, -18.0f});
    t.baked_params.push_back({kCompRatio, 1.0f, 20.0f, 4.0f});
    t.baked_params.push_back({kCompAttackMs, 0.1f, 100.0f, 10.0f});
    t.baked_params.push_back({kCompReleaseMs, 10.0f, 1000.0f, 150.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<CompressorInstance*>(p);
            const float* i = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            // Block-rate ballistics: sample the four knobs at the block's first
            // sample (see the header note on the dynamics nodes).
            signal::Compressor::Params cp;
            cp.threshold_db = params.value_at(kCompThresholdDb, 0);
            cp.ratio = std::max(1.0f, params.value_at(kCompRatio, 0));
            cp.attack_ms = params.value_at(kCompAttackMs, 0);
            cp.release_ms = params.value_at(kCompReleaseMs, 0);
            cp.knee_db = 6.0f;    // musical soft knee
            cp.makeup_db = 0.0f;  // reduce-only: never restores level
            s->comp.set_params(cp);
            for (int k = 0; k < n; ++k)
                o[static_cast<std::size_t>(k)] =
                    s->comp.process(i[static_cast<std::size_t>(k)]);
        };
    return t;
}

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

inline CustomNodeType make_gate_node() {
    CustomNodeType t;
    t.type_id = kGateTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Gate";
    t.lowerable = true;
    t.create = []() -> void* { return new GateInstance{}; };
    t.destroy = [](void* p) { delete static_cast<GateInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<GateInstance*>(p)->sample_rate = sr;
    };
    t.reset = [](void* p) {
        auto* s = static_cast<GateInstance*>(p);
        s->envelope_db = 0.0f;
        s->hold_remaining = 0.0f;
    };
    // threshold_db: -80 .. 0, default -50. attack_ms: 0.1 .. 50, default 1.
    // hold_ms: 0 .. 500, default 50. release_ms: 10 .. 1000, default 100.
    t.baked_params.push_back({kGateThresholdDb, -80.0f, 0.0f, -50.0f});
    t.baked_params.push_back({kGateAttackMs, 0.1f, 50.0f, 1.0f});
    t.baked_params.push_back({kGateHoldMs, 0.0f, 500.0f, 50.0f});
    t.baked_params.push_back({kGateReleaseMs, 10.0f, 1000.0f, 100.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<GateInstance*>(p);
            const float* i = in.channel_ptr(0);
            float* o = out.channel_ptr(0);

            // Block-rate ballistics: sample the four knobs at the block's first
            // sample (see the header note on the dynamics nodes).
            const float sr = static_cast<float>(s->sample_rate);
            const float threshold_db = params.value_at(kGateThresholdDb, 0);
            const float attack_ms = params.value_at(kGateAttackMs, 0);
            const float hold_ms = std::max(0.0f, params.value_at(kGateHoldMs, 0));
            const float release_ms = params.value_at(kGateReleaseMs, 0);

            const float hold_samples = hold_ms * 0.001f * sr;
            const float attack_coeff =
                attack_ms > 0.0f
                    ? 1.0f - std::exp(-1.0f / (attack_ms * 0.001f * sr))
                    : 1.0f;
            const float release_coeff =
                release_ms > 0.0f
                    ? 1.0f - std::exp(-1.0f / (release_ms * 0.001f * sr))
                    : 1.0f;

            for (int k = 0; k < n; ++k) {
                const float x = i[static_cast<std::size_t>(k)];
                const float abs_in = std::max(std::fabs(x), 1e-10f);
                const float in_db = 20.0f * std::log10(abs_in);

                // Key + hold: re-arm the hold window whenever the detector is above
                // threshold; otherwise let it count down.
                if (in_db >= threshold_db) {
                    s->hold_remaining = hold_samples;
                } else if (s->hold_remaining > 0.0f) {
                    s->hold_remaining -= 1.0f;
                }
                const bool open = (in_db >= threshold_db) || (s->hold_remaining > 0.0f);

                // Target gain: fully open (0 dB) while keyed/held, otherwise the
                // NoiseGateT downward-expansion curve, floored at the range.
                float target_db = 0.0f;
                if (!open) {
                    const float below = threshold_db - in_db;
                    target_db = std::max(-below * (GateInstance::kRatio - 1.0f),
                                         GateInstance::kRangeDb);
                }

                // Attack opens (env rising), release closes (env falling).
                const float coeff = (target_db > s->envelope_db) ? attack_coeff : release_coeff;
                s->envelope_db += coeff * (target_db - s->envelope_db);
                s->envelope_db = std::max(s->envelope_db, GateInstance::kRangeDb);
                s->envelope_db = signal::snap_to_zero(s->envelope_db);

                const float gain = std::pow(10.0f, s->envelope_db / 20.0f);
                o[static_cast<std::size_t>(k)] = x * gain;
            }
        };
    return t;
}

}  // namespace pulp::host::forge_lofi
