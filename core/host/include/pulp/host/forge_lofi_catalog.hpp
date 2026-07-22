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
//
// Port arity: every node here is one logical mono in → one logical mono out,
// EXCEPT the dry/wet mixer (two logical inputs, dry and wet) and the ping-pong
// delay, whose two ports are the LEFT and RIGHT halves of one logical stereo
// wire. A host that runs the catalog dual-mono (one instance per channel)
// instantiates a true-stereo node once, spanning both channel rails.
//
// DryWetMixer is block-rate on purpose: DryWetMixerT's public API is block-oriented
// (a scalar set_mix() plus a block mix_wet() over an internal per-channel dry
// buffer) and exposes no per-sample gain hook, so a faithful wrapping applies the
// injected mix value at the block's first sample for the whole block. A mix macro
// is not audio-rate-critical, so block granularity is the right, honest tradeoff —
// unlike a filter cutoff sweep, which this catalog keeps per-sample.

#include <pulp/host/signal_graph.hpp>

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
inline constexpr const char* kTrimTypeId       = "forge_lofi_trim";
inline constexpr const char* kPingPongTypeId   = "forge_lofi_ping_pong";

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

}  // namespace pulp::host::forge_lofi
