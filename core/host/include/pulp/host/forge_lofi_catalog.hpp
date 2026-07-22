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
//
// DryWetMixer is block-rate on purpose: DryWetMixerT's public API is block-oriented
// (a scalar set_mix() plus a block mix_wet() over an internal per-channel dry
// buffer) and exposes no per-sample gain hook, so a faithful wrapping applies the
// injected mix value at the block's first sample for the whole block. A mix macro
// is not audio-rate-critical, so block granularity is the right, honest tradeoff —
// unlike a filter cutoff sweep, which this catalog keeps per-sample.

#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/ballistics_filter.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/dry_wet_mixer.hpp>
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

inline CustomNodeType make_lfo_node() {
    CustomNodeType t;
    t.type_id = kLfoTypeId;
    t.version = 1;
    t.num_input_ports = 0;   // pure control source
    t.num_output_ports = 1;  // CV out
    t.default_name = "LFO";
    t.lowerable = true;
    t.create = []() -> void* { return new LfoInstance{}; };
    t.destroy = [](void* p) { delete static_cast<LfoInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<LfoInstance*>(p)->sample_rate = sr;
    };
    t.reset = [](void* p) { static_cast<LfoInstance*>(p)->phase = 0.0f; };
    t.baked_params.push_back({kLfoRateHz, 0.01f, 40.0f, 2.0f});
    t.baked_params.push_back({kLfoDepth, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kLfoShape, 0.0f, 3.0f, 0.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& /*in*/, int n,
           const BakedParamView& params) {
            auto* s = static_cast<LfoInstance*>(p);
            float* o = out.channel_ptr(0);
            const float inv_sr = 1.0f / static_cast<float>(s->sample_rate);
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float rate = params.value_at(kLfoRateHz, off);
                const float depth = std::clamp(params.value_at(kLfoDepth, off), 0.0f, 1.0f);
                const int shape = static_cast<int>(
                    std::lround(std::clamp(params.value_at(kLfoShape, off), 0.0f, 3.0f)));
                const float osc = forge_lfo_osc(s->phase, shape);
                o[static_cast<std::size_t>(k)] =
                    std::clamp(0.5f + 0.5f * depth * osc, 0.0f, 1.0f);
                s->phase += rate * inv_sr;
                if (s->phase >= 1.0f) s->phase -= std::floor(s->phase);
            }
        };
    return t;
}

// ── VCA (voltage-controlled amplifier) — "Gain" ──────────────────────────
// Two input ports: port 0 = signal (audio), port 1 = gain CV (control, [0, 1]);
// one output. out = signal · gain · clamp(cv, 0, 1), with gain in [0, 1]. It is a
// pure ATTENUATOR — worst-case gain is 1.0 (unity), never a boost — so no CV node
// can amplify a chain; makeup gain is the `trim` node's job. Feeding an lfo's
// output into port 1 yields tremolo; an inverted env_follower yields a self-
// ducking "pump". The CV is clamped to [0, 1] (a control, never a phase inverter
// or a boost). RT-safe: pure arithmetic, no state.
inline CustomNodeType make_vca_node() {
    CustomNodeType t;
    t.type_id = kVcaTypeId;
    t.version = 1;
    t.num_input_ports = 2;   // 0 = signal, 1 = gain CV
    t.num_output_ports = 1;
    t.default_name = "VCA";
    t.lowerable = true;
    t.create = []() -> void* { return new int{0}; };  // trivial keepalive instance
    t.destroy = [](void* p) { delete static_cast<int*>(p); };
    t.baked_params.push_back({kVcaGain, 0.0f, 1.0f, 1.0f});
    t.process_instance_baked_param =
        [](void* /*p*/, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            const float* sig = in.channel_ptr(0);
            const float* cv = in.channel_ptr(1);
            float* o = out.channel_ptr(0);
            for (int k = 0; k < n; ++k) {
                const float gain = std::clamp(
                    params.value_at(kVcaGain, static_cast<std::int32_t>(k)), 0.0f, 1.0f);
                const float c = std::clamp(cv[static_cast<std::size_t>(k)], 0.0f, 1.0f);
                o[static_cast<std::size_t>(k)] = sig[static_cast<std::size_t>(k)] * gain * c;
            }
        };
    return t;
}

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

inline CustomNodeType make_env_follower_node() {
    CustomNodeType t;
    t.type_id = kEnvFollowerTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Envelope";
    t.lowerable = true;
    t.create = []() -> void* { return new EnvFollowerInstance{}; };
    t.destroy = [](void* p) { delete static_cast<EnvFollowerInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<EnvFollowerInstance*>(p);
        s->env.prepare(static_cast<float>(sr));
        s->env.set_mode(signal::BallisticsFilter::Mode::peak);
        s->last_attack_ms = -1.0f;
        s->last_release_ms = -1.0f;
    };
    t.reset = [](void* p) {
        auto* s = static_cast<EnvFollowerInstance*>(p);
        s->env.reset();
        s->last_attack_ms = -1.0f;
        s->last_release_ms = -1.0f;
    };
    t.baked_params.push_back({kEnvAttackMs, 0.1f, 500.0f, 10.0f});
    t.baked_params.push_back({kEnvReleaseMs, 1.0f, 2000.0f, 150.0f});
    t.baked_params.push_back({kEnvSensitivity, 0.1f, 8.0f, 1.0f});
    t.baked_params.push_back({kEnvInvert, 0.0f, 1.0f, 0.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<EnvFollowerInstance*>(p);
            const float* i = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            // Ballistics are block-rate: a setter recomputes an exp coefficient,
            // so retune only when the injected value actually moved.
            const float atk = params.value_at(kEnvAttackMs, 0);
            const float rel = params.value_at(kEnvReleaseMs, 0);
            if (atk != s->last_attack_ms) { s->env.set_attack_ms(atk); s->last_attack_ms = atk; }
            if (rel != s->last_release_ms) { s->env.set_release_ms(rel); s->last_release_ms = rel; }
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float sens = params.value_at(kEnvSensitivity, off);
                const bool invert = params.value_at(kEnvInvert, off) >= 0.5f;
                float env = s->env.process(i[static_cast<std::size_t>(k)]);
                env = std::clamp(sens * env, 0.0f, 1.0f);
                o[static_cast<std::size_t>(k)] = invert ? (1.0f - env) : env;
            }
        };
    return t;
}

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

inline CustomNodeType make_filter_cv_node() {
    CustomNodeType t;
    t.type_id = kFilterCvTypeId;
    t.version = 1;
    t.num_input_ports = 2;   // 0 = signal, 1 = cutoff CV
    t.num_output_ports = 1;
    t.default_name = "FilterCV";
    t.lowerable = true;
    t.create = []() -> void* { return new FilterCvInstance{}; };
    t.destroy = [](void* p) { delete static_cast<FilterCvInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<FilterCvInstance*>(p);
        s->sample_rate = sr;
        s->svf.set_sample_rate(static_cast<float>(sr));
        s->svf.set_mode(signal::Svf::Mode::lowpass);
        s->last_resonance = -1.0f;
    };
    t.reset = [](void* p) {
        auto* s = static_cast<FilterCvInstance*>(p);
        s->svf.reset();
        s->last_resonance = -1.0f;
    };
    t.baked_params.push_back({kFilterCvBaseHz, 20.0f, 20000.0f, 500.0f});
    t.baked_params.push_back({kFilterCvAmountOct, 0.0f, 8.0f, 3.0f});
    t.baked_params.push_back({kFilterCvResonance, 0.5f, 12.0f, 3.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<FilterCvInstance*>(p);
            const float* sig = in.channel_ptr(0);
            const float* cv = in.channel_ptr(1);
            float* o = out.channel_ptr(0);
            const float nyq_guard = 0.45f * static_cast<float>(s->sample_rate);
            const float q = params.value_at(kFilterCvResonance, 0);
            if (q != s->last_resonance) { s->svf.set_resonance(q); s->last_resonance = q; }
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float base = params.value_at(kFilterCvBaseHz, off);
                const float amount = params.value_at(kFilterCvAmountOct, off);
                const float c = std::clamp(cv[static_cast<std::size_t>(k)], 0.0f, 1.0f);
                float cutoff = base * std::exp2(c * amount);
                cutoff = std::clamp(cutoff, 20.0f, nyq_guard);
                s->svf.set_frequency(cutoff);
                o[static_cast<std::size_t>(k)] = s->svf.process(sig[static_cast<std::size_t>(k)]);
            }
        };
    return t;
}

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

inline CustomNodeType make_delay_cv_node() {
    CustomNodeType t;
    t.type_id = kDelayCvTypeId;
    t.version = 1;
    t.num_input_ports = 2;   // 0 = signal, 1 = time CV
    t.num_output_ports = 1;
    t.default_name = "DelayCV";
    t.lowerable = true;
    t.create = []() -> void* { return new DelayCvInstance{}; };
    t.destroy = [](void* p) { delete static_cast<DelayCvInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<DelayCvInstance*>(p);
        s->sample_rate = sr;
        s->max_delay_samples =
            std::max(1, static_cast<int>(std::ceil(kDelayMaxMs * 0.001 * sr)));
        s->line.prepare(s->max_delay_samples);
    };
    t.reset = [](void* p) { static_cast<DelayCvInstance*>(p)->line.reset(); };
    // base 0.1..50 ms (chorus range) default 15; depth 0..25 ms default 5; feedback
    // 0..0.95 default 0 (chorus); mix 0..1 default 0.5.
    t.baked_params.push_back({kDelayCvBaseMs, 0.1f, 50.0f, 15.0f});
    t.baked_params.push_back({kDelayCvDepthMs, 0.0f, 25.0f, 5.0f});
    t.baked_params.push_back({kDelayCvFeedback, 0.0f, 0.95f, 0.0f});
    t.baked_params.push_back({kDelayCvMix, 0.0f, 1.0f, 0.5f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<DelayCvInstance*>(p);
            const float* sig = in.channel_ptr(0);
            const float* cv = in.channel_ptr(1);
            float* o = out.channel_ptr(0);
            const float sr_per_ms = static_cast<float>(s->sample_rate) * 0.001f;
            const float max_d = static_cast<float>(s->max_delay_samples);
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float base_ms = params.value_at(kDelayCvBaseMs, off);
                const float depth_ms = params.value_at(kDelayCvDepthMs, off);
                float fb = std::clamp(params.value_at(kDelayCvFeedback, off), 0.0f, 0.95f);
                const float mix = std::clamp(params.value_at(kDelayCvMix, off), 0.0f, 1.0f);
                const float c = std::clamp(cv[static_cast<std::size_t>(k)], 0.0f, 1.0f);

                float delay_samples = (base_ms + c * depth_ms) * sr_per_ms;
                delay_samples = std::clamp(delay_samples, 1.0f, max_d);

                const float dry = sig[static_cast<std::size_t>(k)];
                const float wet = s->line.read(delay_samples);
                s->line.push(dry + fb * wet);
                o[static_cast<std::size_t>(k)] = dry * (1.0f - mix) + wet * mix;
            }
        };
    return t;
}

}  // namespace pulp::host::forge_lofi
