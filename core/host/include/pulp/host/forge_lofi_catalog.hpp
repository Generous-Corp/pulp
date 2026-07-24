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
// Macro-knob mapping:
//   * Delay (feedback echo)  → "Time"  = time_ms + "Feedback" = feedback
//                                                     (time_ms per-sample, interpolated)
//   * Filter (Svf)           → "Tone"  = cutoff_hz + "Resonance" = resonance
//                                                     (sample-accurate; mode is
//                                                      fixed per registered type)
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

#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/compressor.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/dry_wet_mixer.hpp>
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

inline CustomNodeType make_filter_node(
    signal::Svf::Mode mode = signal::Svf::Mode::lowpass) {
    CustomNodeType t;
    switch (mode) {
        case signal::Svf::Mode::highpass: t.type_id = kFilterHighpassTypeId; break;
        case signal::Svf::Mode::bandpass: t.type_id = kFilterBandpassTypeId; break;
        case signal::Svf::Mode::notch:    t.type_id = kFilterNotchTypeId;    break;
        case signal::Svf::Mode::lowpass:
        default:                          t.type_id = kFilterTypeId;         break;
    }
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Tone";
    t.lowerable = true;
    t.create = []() -> void* { return new FilterInstance{}; };
    t.destroy = [](void* p) { delete static_cast<FilterInstance*>(p); };
    t.prepare = [mode](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<FilterInstance*>(p);
        s->sample_rate = sr;
        s->svf.set_sample_rate(static_cast<float>(sr));
        s->resonance = 0.707f;                   // Butterworth-ish, no peak
        s->svf.set_resonance(s->resonance);
        s->svf.set_mode(mode);
    };
    t.reset = [](void* p) { static_cast<FilterInstance*>(p)->svf.reset(); };
    // cutoff_hz: 20 Hz .. 20 kHz, default fully open (transparent until turned).
    t.baked_params.push_back({kFilterCutoffHz, 20.0f, 20000.0f, 20000.0f});
    // resonance (Q): 0.5 (gently damped) .. 12 (a singing peak), default 0.707
    // — maximally flat, so an un-bound Resonance is the old fixed behavior.
    t.baked_params.push_back({kFilterResonance, 0.5f, 12.0f, 0.707f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<FilterInstance*>(p);
            const float* i = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float res = params.value_at(kFilterResonance, off);
                if (res != s->resonance) {
                    s->resonance = res;
                    s->svf.set_resonance(res);
                }
                const float cutoff = params.value_at(kFilterCutoffHz, off);
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

inline CustomNodeType make_trim_node() {
    CustomNodeType t;
    t.type_id = kTrimTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Level";
    t.lowerable = true;
    t.create = []() -> void* { return new TrimInstance{}; };
    t.destroy = [](void* p) { delete static_cast<TrimInstance*>(p); };
    t.reset = [](void* p) {
        auto* s = static_cast<TrimInstance*>(p);
        s->last_db = 0.0f;
        s->gain = 1.0f;
    };
    // gain_db: −24 .. +24 dB, default 0 (unity — transparent until turned).
    t.baked_params.push_back({kTrimGainDb, -24.0f, 24.0f, 0.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<TrimInstance*>(p);
            const float* i = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            for (int k = 0; k < n; ++k) {
                const float db =
                    params.value_at(kTrimGainDb, static_cast<std::int32_t>(k));
                if (db != s->last_db) {
                    s->last_db = db;
                    s->gain = std::exp2(db * (1.0f / 6.020599913279624f));
                }
                o[static_cast<std::size_t>(k)] =
                    i[static_cast<std::size_t>(k)] * s->gain;
            }
        };
    return t;
}

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

inline CustomNodeType make_ping_pong_node() {
    CustomNodeType t;
    t.type_id = kPingPongTypeId;
    t.version = 1;
    t.num_input_ports = 2;   // 0 = left, 1 = right (ONE logical stereo wire)
    t.num_output_ports = 2;
    t.default_name = "Ping Pong";
    t.lowerable = true;
    t.create = []() -> void* { return new PingPongInstance{}; };
    t.destroy = [](void* p) { delete static_cast<PingPongInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<PingPongInstance*>(p);
        s->sample_rate = sr;
        s->max_delay_samples =
            std::max(1, static_cast<int>(std::ceil(kDelayMaxMs * 0.001 * sr)));
        s->line_l.prepare(s->max_delay_samples);
        s->line_r.prepare(s->max_delay_samples);
    };
    t.reset = [](void* p) {
        auto* s = static_cast<PingPongInstance*>(p);
        s->line_l.reset();
        s->line_r.reset();
    };
    // Same time/feedback envelope as the mono delay; width defaults to a full
    // hard bounce (the thing the node is asked for by name).
    t.baked_params.push_back({kPingPongTimeMs, 1.0f, kDelayMaxMs, 350.0f});
    t.baked_params.push_back({kPingPongFeedback, 0.0f, 0.95f, 0.45f});
    t.baked_params.push_back({kPingPongWidth, 0.0f, 1.0f, 1.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<PingPongInstance*>(p);
            const float* il = in.channel_ptr(0);
            const float* ir = in.channel_ptr(1);
            float* ol = out.channel_ptr(0);
            float* or_ = out.channel_ptr(1);
            const float sr_per_ms = static_cast<float>(s->sample_rate) * 0.001f;
            const float max_d = static_cast<float>(s->max_delay_samples);
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float time_ms = params.value_at(kPingPongTimeMs, off);
                const float fb = std::clamp(params.value_at(kPingPongFeedback, off),
                                            0.0f, 0.95f);
                const float width = std::clamp(params.value_at(kPingPongWidth, off),
                                               0.0f, 1.0f);

                float delay_samples = time_ms * sr_per_ms;
                delay_samples = std::clamp(delay_samples, 1.0f, max_d);

                const float dry_l = il[static_cast<std::size_t>(k)];
                const float dry_r = ir[static_cast<std::size_t>(k)];
                const float wet_l = s->line_l.read(delay_samples);
                const float wet_r = s->line_r.read(delay_samples);

                s->line_l.push(dry_l + fb * wet_r);  // right tap feeds the left line
                s->line_r.push(dry_r + fb * wet_l);  // and vice versa — the bounce

                const float same = 0.5f + 0.5f * width;
                const float cross = 0.5f - 0.5f * width;
                ol[static_cast<std::size_t>(k)] = dry_l + same * wet_l + cross * wet_r;
                or_[static_cast<std::size_t>(k)] = dry_r + same * wet_r + cross * wet_l;
            }
        };
    return t;
}

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

inline CustomNodeType make_reverb_node() {
    CustomNodeType t;
    t.type_id = kReverbTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Reverb";
    t.lowerable = true;
    t.create = []() -> void* { return new ReverbInstance{}; };
    t.destroy = [](void* p) { delete static_cast<ReverbInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<ReverbInstance*>(p);
        s->reverb.prepare(static_cast<float>(sr));
        // Keep signal::Reverb fully wet. The wrapper applies the injectable
        // dry/wet blend after normalizing the wet path to its advertised bound.
        s->reverb.set_mix(1.0f);
        s->last_decay = -1.0f;    // force a setter write on the first sample
        s->last_damping = -1.0f;
    };
    t.reset = [](void* p) { static_cast<ReverbInstance*>(p)->reverb.reset(); };
    // decay: 0.1 .. 10 s (RT60), default 2 s. damping: 0 .. 0.99, default 0.3.
    // mix: 0 .. 1, default 0.3 — a subtle wet blend an untouched reverb sits at.
    t.baked_params.push_back({kReverbDecay, 0.1f, 10.0f, 2.0f});
    t.baked_params.push_back({kReverbDamping, 0.0f, 0.99f, 0.3f});
    t.baked_params.push_back({kReverbMix, 0.0f, 1.0f, 0.3f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<ReverbInstance*>(p);
            const float* i = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float decay = params.value_at(kReverbDecay, off);
                if (decay != s->last_decay) {
                    s->last_decay = decay;
                    s->reverb.set_decay(decay);
                }
                const float damping = params.value_at(kReverbDamping, off);
                if (damping != s->last_damping) {
                    s->last_damping = damping;
                    s->reverb.set_damping(damping);
                }
                const float mix =
                    std::clamp(params.value_at(kReverbMix, off), 0.0f, 1.0f);
                const float dry = i[static_cast<std::size_t>(k)];
                const auto st = s->reverb.process(i[static_cast<std::size_t>(k)]);
                const float wet =
                    0.5f * (st.left + st.right) * kReverbWetNormalization;
                o[static_cast<std::size_t>(k)] = dry * (1.0f - mix) + wet * mix;
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

inline CustomNodeType make_auto_pan_node() {
    CustomNodeType t;
    t.type_id = kAutoPanTypeId;
    t.version = 1;
    t.num_input_ports = 2;   // 0 = left, 1 = right (ONE logical stereo wire)
    t.num_output_ports = 2;
    t.default_name = "Auto Pan";
    t.lowerable = true;
    t.create = []() -> void* { return new AutoPanInstance{}; };
    t.destroy = [](void* p) { delete static_cast<AutoPanInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<AutoPanInstance*>(p)->sample_rate = sr;
    };
    t.reset = [](void* p) { static_cast<AutoPanInstance*>(p)->phase = 0.0f; };
    t.baked_params.push_back({kAutoPanRateHz, 0.01f, 20.0f, 1.0f});
    t.baked_params.push_back({kAutoPanDepth, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kAutoPanShape, 0.0f, 3.0f, 0.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<AutoPanInstance*>(p);
            const float* il = in.channel_ptr(0);
            const float* ir = in.channel_ptr(1);
            float* ol = out.channel_ptr(0);
            float* or_ = out.channel_ptr(1);
            const float inv_sr = 1.0f / static_cast<float>(s->sample_rate);
            constexpr float kHalfPi = 1.57079632679489661923f;
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float rate = params.value_at(kAutoPanRateHz, off);
                const float depth =
                    std::clamp(params.value_at(kAutoPanDepth, off), 0.0f, 1.0f);
                const int shape = static_cast<int>(std::lround(
                    std::clamp(params.value_at(kAutoPanShape, off), 0.0f, 3.0f)));

                const float osc = forge_lfo_osc(s->phase, shape);   // [-1, 1]
                const float pan = std::clamp(depth * osc, -1.0f, 1.0f);
                const float position = (pan + 1.0f) * 0.5f;         // [0, 1]
                const float theta = position * kHalfPi;
                const float lg = std::cos(theta);
                const float rg = std::sin(theta);
                ol[static_cast<std::size_t>(k)] = il[static_cast<std::size_t>(k)] * lg;
                or_[static_cast<std::size_t>(k)] = ir[static_cast<std::size_t>(k)] * rg;

                s->phase += rate * inv_sr;
                if (s->phase >= 1.0f) s->phase -= std::floor(s->phase);
            }
        };
    return t;
}

// ── Width — TRUE STEREO, "Width" ─────────────────────────────────────────
// A mid-side width control. It splits the stereo signal into a MID (the mono
// sum) and a SIDE (the difference) component, scales the SIDE by `width`, and
// recombines:
//   mid = (L + R) / 2;  side = (L − R) / 2;
//   L' = mid + width·side;  R' = mid − width·side.
// width = 0 collapses the side entirely → L' = R' = mid, a mono image; width = 1
// reconstructs the original L/R exactly (unity); width > 1 amplifies the side,
// widening the image past the original. It is true stereo because it operates on
// the SUM and DIFFERENCE of the two channels — the relationship between them —
// which a per-channel instance cannot compute.
//   * width (0 .. 2): mono at 0, unity at 1, up to 2× the side energy at 2.
// Worst-case gain: at width = 2, L' = 1.5·L − 0.5·R, whose peak magnitude for
// |L|,|R| ≤ 1 is 2.0. The transform is pure arithmetic with no state, but the
// node still owns a trivial instance so it takes the same instanced code path as
// every other catalog node (RAII create/destroy) rather than a special-case
// stateless path.
struct WidthInstance {};

inline CustomNodeType make_width_node() {
    CustomNodeType t;
    t.type_id = kWidthTypeId;
    t.version = 1;
    t.num_input_ports = 2;   // 0 = left, 1 = right (ONE logical stereo wire)
    t.num_output_ports = 2;
    t.default_name = "Width";
    t.lowerable = true;
    t.create = []() -> void* { return new WidthInstance{}; };
    t.destroy = [](void* p) { delete static_cast<WidthInstance*>(p); };
    t.prepare = [](void* /*p*/, double /*sr*/, int /*max_block*/) {};
    t.reset = [](void* /*p*/) {};
    t.baked_params.push_back({kWidthAmount, 0.0f, 2.0f, 1.0f});
    t.process_instance_baked_param =
        [](void* /*p*/, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            const float* il = in.channel_ptr(0);
            const float* ir = in.channel_ptr(1);
            float* ol = out.channel_ptr(0);
            float* or_ = out.channel_ptr(1);
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                const float width =
                    std::clamp(params.value_at(kWidthAmount, off), 0.0f, 2.0f);
                const float l = il[static_cast<std::size_t>(k)];
                const float r = ir[static_cast<std::size_t>(k)];
                const float mid = 0.5f * (l + r);
                const float side = 0.5f * (l - r);
                ol[static_cast<std::size_t>(k)] = mid + width * side;
                or_[static_cast<std::size_t>(k)] = mid - width * side;
            }
        };
    return t;
}

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

inline CustomNodeType make_phaser_node() {
    CustomNodeType t;
    t.type_id = kPhaserTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Phaser";
    t.lowerable = true;
    t.create = []() -> void* { return new PhaserInstance{}; };
    t.destroy = [](void* p) { delete static_cast<PhaserInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<PhaserInstance*>(p);
        s->sample_rate = sr;
        s->phaser.set_sample_rate(static_cast<float>(sr));
        s->phaser.set_mix(0.5f);  // classic 50/50 → the notches reach full depth
    };
    t.reset = [](void* p) { static_cast<PhaserInstance*>(p)->phaser.reset(); };
    t.baked_params.push_back({kPhaserRateHz, 0.01f, 20.0f, 0.5f});
    t.baked_params.push_back({kPhaserDepth, 0.0f, 1.0f, 0.7f});
    t.baked_params.push_back({kPhaserFeedback, 0.0f, 0.9f, 0.5f});
    t.baked_params.push_back({kPhaserStages, 2.0f, 8.0f, 4.0f});
    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n,
           const BakedParamView& params) {
            auto* s = static_cast<PhaserInstance*>(p);
            const float* x = in.channel_ptr(0);
            float* o = out.channel_ptr(0);
            for (int k = 0; k < n; ++k) {
                const auto off = static_cast<std::int32_t>(k);
                s->phaser.set_rate(params.value_at(kPhaserRateHz, off));
                s->phaser.set_depth(
                    std::clamp(params.value_at(kPhaserDepth, off), 0.0f, 1.0f));
                s->phaser.set_feedback(
                    std::clamp(params.value_at(kPhaserFeedback, off), 0.0f, 0.9f));
                s->phaser.set_stages(static_cast<int>(std::lround(
                    std::clamp(params.value_at(kPhaserStages, off), 2.0f, 8.0f))));
                o[static_cast<std::size_t>(k)] =
                    s->phaser.process(x[static_cast<std::size_t>(k)]);
            }
        };
    return t;
}


}  // namespace pulp::host::forge_lofi
