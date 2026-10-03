#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <pulp/host/detail/forge_space_catalog_descriptor.hpp>
#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/forge_space_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
#if defined(PULP_HOST_ENABLE_GPU_CONVOLUTION)
#include <pulp/gpu_audio/gpu_convolution_reverb.hpp>
#endif

namespace pulp::host::space {

// ── The convolution reverb (measured spaces) ──────────────────────────────
//
// One node type. The IR and the whole load-time policy are registration
// arguments, for the reasons in the file note; everything left is a gain, a
// filter corner, or a delay, and all of it injects.
namespace convolution {

/// An impulse response, owned by the registered type.
///
/// Held by `shared_ptr` because `CustomNodeType` is copied into the graph's
/// registry and every instance's `prepare` re-ingests it at the session rate —
/// copying a multi-second four-channel IR per instance would be pure waste, and
/// the buffer is immutable once registered.

bool valid_impulse_response(const ImpulseResponse& ir) {
    if (ir.channels.size() != 1u && ir.channels.size() != 2u && ir.channels.size() != 4u)
        return false;
    if (!std::isfinite(ir.sample_rate) || ir.sample_rate <= 0.0 || ir.channels[0].empty())
        return false;
    const std::size_t length = ir.channels[0].size();
    if (length > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !Engine::valid_resample_geometry(static_cast<int>(length), ir.sample_rate, 48000.0,
                                         Engine::kResampTapsPerPhaseDefault))
        return false;
    for (const auto& channel : ir.channels) {
        if (channel.size() != length)
            return false;
        for (float sample : channel)
            if (!std::isfinite(sample))
                return false;
    }
    return true;
}

/// The load-time policy, frozen at registration. See the file note, item 2:
/// each of these is documented by the DSP as taking effect on the next load, so
/// none of them can be a param.

struct Instance {
    Engine engine;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The wet bound is not computed here: the DSP measures `ir_gain * ||h||_1` AT
/// LOAD — the exact ℓ∞ operator norm of the wet path, approached by the
/// sign-matched input — and its own suite asserts it. This composes that
/// measured number up to the node's ceilings:
///
///     dry_max + wet_max * width_max * (ir_gain_max * ||h||_1)
///
/// The `width_max` factor is easy to miss and is real: the DSP's mid/side stage
/// computes `side * width` before the wet gain, so at 200 % a fully
/// out-of-phase wet pair (`mid = 0`) comes out at twice the amplitude.
///
/// IR-DEPENDENT BY CONSTRUCTION. `||h||_1` is a property of the impulse
/// response, so a registry row for this type carries the formula and the IR
/// reference; the number is only meaningful once an IR is named.
float convolution_reverb_worst_case_gain(const ImpulseResponse& ir, const IrPolicy& policy,
                                         double sample_rate, int max_block) {
    Engine probe;
    probe.prepare(sample_rate, max_block, 2);
    probe.set_normalize_mode(policy.normalize);
    probe.set_tail_trim_db(policy.tail_trim_db);
    probe.set_tail_fade_ms(policy.tail_fade_ms);
    probe.set_resample_taps_per_phase(policy.resample_taps_per_phase);
    probe.set_true_stereo(policy.true_stereo);

    std::vector<const float*> ptrs(ir.channels.size());
    for (std::size_t c = 0; c < ir.channels.size(); ++c)
        ptrs[c] = ir.channels[c].data();
    if (ir.channels.empty() ||
        !probe.load_impulse_response(ptrs.data(), static_cast<int>(ir.channels.size()),
                                     static_cast<int>(ir.channels[0].size()), ir.sample_rate))
        return 0.0f;

    probe.set_ir_gain_db(Engine::kIrGainDbMax);
    const double dry_max = 1.0; // kDryPercent ceiling, as a linear gain
    const double wet_max = 1.0; // kWetPercent ceiling
    const double width_max = Engine::kWidthPercentMax / 100.0;
    return static_cast<float>(dry_max + wet_max * width_max * probe.worst_case_gain());
}

/// The measured-space reverb as a lowerable custom node.
///
/// `ir` and `policy` are registration arguments; see the file note. `ir` is
/// copied once into a shared buffer, and every instance re-ingests it in
/// `prepare()` because the ingest resamples to the SESSION rate — which is not
/// known until then.
CustomNodeType make_convolution_reverb_node(ImpulseResponse ir, IrPolicy policy) {
    if (!valid_impulse_response(ir))
        throw std::invalid_argument("convolution IR must have finite, representable rate/length "
                                    "geometry and 1, 2, or 4 equal-length channels");
    auto shared = std::make_shared<ImpulseResponse>(std::move(ir));

    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2; // 0 = left, 1 = right (ONE logical stereo wire)
    t.num_output_ports = 2;
    t.default_name = "Convolution Reverb";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [shared, policy](void* p, double sr, int max_block) {
        auto* s = static_cast<Instance*>(p);
        s->engine.prepare(sr, max_block, 2);
        // Policy before load: every one of these is read BY the load.
        s->engine.set_normalize_mode(policy.normalize);
        s->engine.set_tail_trim_db(policy.tail_trim_db);
        s->engine.set_tail_fade_ms(policy.tail_fade_ms);
        s->engine.set_resample_taps_per_phase(policy.resample_taps_per_phase);
        s->engine.set_true_stereo(policy.true_stereo);
        if (!shared->channels.empty()) {
            std::vector<const float*> ptrs(shared->channels.size());
            for (std::size_t c = 0; c < shared->channels.size(); ++c)
                ptrs[c] = shared->channels[c].data();
            const bool loaded = s->engine.load_impulse_response(
                ptrs.data(), static_cast<int>(shared->channels.size()),
                static_cast<int>(shared->channels[0].size()), shared->sample_rate);
            if (!loaded)
                throw std::runtime_error("validated convolution IR failed to load");
        }
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };

    t.baked_params.push_back({kIrGainDb, static_cast<float>(Engine::kIrGainDbMin),
                              static_cast<float>(Engine::kIrGainDbMax),
                              static_cast<float>(Engine::kIrGainDbDefault)});
    t.baked_params.push_back({kPredelayMs, static_cast<float>(Engine::kPredelayMsMin),
                              static_cast<float>(Engine::kPredelayMsMax),
                              static_cast<float>(Engine::kPredelayMsDefault)});
    t.baked_params.push_back(
        {kWetPercent, 0.0f, 100.0f, static_cast<float>(Engine::kWetPercentDefault)});
    t.baked_params.push_back(
        {kDryPercent, 0.0f, 100.0f, static_cast<float>(Engine::kDryPercentDefault)});
    t.baked_params.push_back({kWidthPercent, static_cast<float>(Engine::kWidthPercentMin),
                              static_cast<float>(Engine::kWidthPercentMax),
                              static_cast<float>(Engine::kWidthPercentDefault)});
    t.baked_params.push_back({kLowcutHz, static_cast<float>(Engine::kLowcutHzMin),
                              static_cast<float>(Engine::kLowcutHzMax),
                              static_cast<float>(Engine::kLowcutHzDefault)});
    t.baked_params.push_back({kHighcutHz, static_cast<float>(Engine::kHighcutHzMin),
                              static_cast<float>(Engine::kHighcutHzMax),
                              static_cast<float>(Engine::kHighcutHzDefault)});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);

        // Block rate, at offset 0. Not a shortcut — the engine hoists all four
        // mix gains out of its own sample loop, so a mid-block value could not
        // reach the audio even if this read it. See the file note.
        s->engine.set_ir_gain_db(params.value_at(kIrGainDb, 0));
        s->engine.set_predelay_ms(params.value_at(kPredelayMs, 0));
        s->engine.set_wet_percent(params.value_at(kWetPercent, 0));
        s->engine.set_dry_percent(params.value_at(kDryPercent, 0));
        s->engine.set_width_percent(params.value_at(kWidthPercent, 0));
        s->engine.set_lowcut_hz(params.value_at(kLowcutHz, 0));
        s->engine.set_highcut_hz(params.value_at(kHighcutHz, 0));

        const float* in_ptrs[2] = {in.channel_ptr(0), in.channel_ptr(1)};
        float* out_ptrs[2] = {out.channel_ptr(0), out.channel_ptr(1)};
        s->engine.process(in_ptrs, out_ptrs, n);
    };
    return t;
}

#if defined(PULP_HOST_ENABLE_GPU_CONVOLUTION)

int gpu_internal_block_size(int max_block) noexcept {
    if (max_block <= 0)
        return 0;
    std::uint64_t quantum = 1u;
    while (quantum < static_cast<std::uint64_t>(max_block))
        quantum <<= 1u;
    if (quantum > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        return 0;
    return static_cast<int>(quantum);
}

struct GpuInstance {
    std::unique_ptr<gpu_audio::GpuConvolutionReverb> engine;
    std::uint64_t preparation_generation = 0;
};

// Fixed POD schema. Lane counters count transport quanta, not arbitrary host
// callbacks. Both mono lanes stay separate; worker output is not GPU delivery.
CustomNodeDiagnosticsDescriptor gpu_convolution_diagnostics() {
    return {
        kGpuTypeId, 1, kGpuDiagnosticSchema, sizeof(GpuConvolutionDiagnostics),
        [](const void* opaque, std::span<std::byte> output, std::uint64_t& generation) noexcept {
            const auto& instance = *static_cast<const GpuInstance*>(opaque);
            if (!instance.engine || !instance.engine->prepared())
                return CustomNodeDiagnosticAvailability::NotPrepared;
            const GpuConvolutionDiagnostics value{1, 2, instance.engine->report()};
            static_assert(std::is_trivially_copyable_v<GpuConvolutionDiagnostics>);
            if (output.size() != sizeof(value))
                return CustomNodeDiagnosticAvailability::SchemaMismatch;
            std::memcpy(output.data(), &value, sizeof(value));
            generation = instance.preparation_generation;
            return CustomNodeDiagnosticAvailability::Available;
        }};
}

/// Construct the opt-in GPU realization.  It deliberately accepts only the
/// one/two-channel asset shapes admitted by Forge: two concrete authenticated
/// mono lanes preserve dual-mono identity, while a four-cell true-stereo IR
/// remains on the CPU realization until a channel-matrix GPU node exists.
CustomNodeType make_gpu_convolution_reverb_node(ImpulseResponse ir, IrPolicy policy = {},
                                                gpu_audio::GpuConvolverTraceConfig trace) {
    if (!valid_impulse_response(ir) || ir.channels.size() > 2u || policy.true_stereo)
        throw std::invalid_argument("GPU convolution requires a one- or two-channel dual-mono IR");
    auto shared = std::make_shared<ImpulseResponse>(std::move(ir));

    CustomNodeType t;
    t.type_id = kGpuTypeId;
    t.version = 1;
    t.num_input_ports = 2;
    t.num_output_ports = 2;
    t.default_name = "GPU Convolution Reverb";
    // This route owns live authenticated transports and is intentionally not
    // lowerable into a baked artifact.  The CPU realization remains the stable
    // default for offline/baked graphs.
    t.lowerable = false;
    t.create = []() -> void* { return new GpuInstance{}; };
    t.destroy = [](void* p) { delete static_cast<GpuInstance*>(p); };
    t.prepare = [shared, policy, trace](void* p, double sr, int max_block) {
        auto* instance = static_cast<GpuInstance*>(p);
        ++instance->preparation_generation;
        const int internal_block = gpu_internal_block_size(max_block);
        if (!std::isfinite(sr) || sr <= 0.0 || max_block <= 0 ||
            sr > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
            internal_block <= 0)
            throw std::runtime_error("invalid GPU convolution prepare geometry");
        gpu_audio::GpuConvolutionReverbConfig config;
        config.block_size = static_cast<std::uint32_t>(internal_block);
        config.sample_rate = static_cast<std::uint32_t>(std::lround(sr));
        config.impulse_response_sample_rate = shared->sample_rate;
        config.impulse_response = shared->channels;
        config.normalize = policy.normalize;
        config.tail_trim_db = policy.tail_trim_db;
        config.tail_fade_ms = policy.tail_fade_ms;
        config.resample_taps_per_phase = policy.resample_taps_per_phase;
        config.gpu_enabled = true;
        config.trace = trace;
        instance->engine = std::make_unique<gpu_audio::GpuConvolutionReverb>(std::move(config));
        if (!instance->engine->prepare())
            throw std::runtime_error("authenticated GPU convolution provider unavailable");
    };
    t.process_instance = [](void* p, audio::BufferView<float>& out,
                            const audio::BufferView<const float>& in, int n) {
        auto* instance = static_cast<GpuInstance*>(p);
        if (!instance->engine) {
            out.clear();
            return;
        }
        instance->engine->process(in, out, static_cast<std::uint32_t>(std::max(0, n)));
    };
    t.baked_params.push_back({kIrGainDb, static_cast<float>(Engine::kIrGainDbMin),
                              static_cast<float>(Engine::kIrGainDbMax),
                              static_cast<float>(Engine::kIrGainDbDefault)});
    t.baked_params.push_back({kPredelayMs, static_cast<float>(Engine::kPredelayMsMin),
                              static_cast<float>(Engine::kPredelayMsMax),
                              static_cast<float>(Engine::kPredelayMsDefault)});
    t.baked_params.push_back(
        {kWetPercent, 0.0f, 100.0f, static_cast<float>(Engine::kWetPercentDefault)});
    t.baked_params.push_back(
        {kDryPercent, 0.0f, 100.0f, static_cast<float>(Engine::kDryPercentDefault)});
    t.baked_params.push_back({kWidthPercent, static_cast<float>(Engine::kWidthPercentMin),
                              static_cast<float>(Engine::kWidthPercentMax),
                              static_cast<float>(Engine::kWidthPercentDefault)});
    t.baked_params.push_back({kLowcutHz, static_cast<float>(Engine::kLowcutHzMin),
                              static_cast<float>(Engine::kLowcutHzMax),
                              static_cast<float>(Engine::kLowcutHzDefault)});
    t.baked_params.push_back({kHighcutHz, static_cast<float>(Engine::kHighcutHzMin),
                              static_cast<float>(Engine::kHighcutHzMax),
                              static_cast<float>(Engine::kHighcutHzDefault)});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* instance = static_cast<GpuInstance*>(p);
        if (!instance->engine) {
            out.clear();
            return;
        }
        instance->engine->set_ir_gain_db(params.value_at(kIrGainDb, 0));
        instance->engine->set_predelay_ms(params.value_at(kPredelayMs, 0));
        instance->engine->set_wet_percent(params.value_at(kWetPercent, 0));
        instance->engine->set_dry_percent(params.value_at(kDryPercent, 0));
        instance->engine->set_width_percent(params.value_at(kWidthPercent, 0));
        instance->engine->set_lowcut_hz(params.value_at(kLowcutHz, 0));
        instance->engine->set_highcut_hz(params.value_at(kHighcutHz, 0));
        instance->engine->process(in, out, static_cast<std::uint32_t>(std::max(0, n)));
    };
    t.latency_samples_for_block = [](double, int max_block) {
        const int internal_block = gpu_internal_block_size(max_block);
        if (internal_block <= 0 || internal_block > CustomNodeType::kMaxLatencySamples / 3)
            return 0;
        return 3 * internal_block;
    };
    return t;
}

#endif

/// Construct the metadata/audit realization without making the central
/// registry know how to synthesize this asset-backed family's required input.
CustomNodeType catalog_probe_node() {
    return make_convolution_reverb_node({{{1.0f}}, 48000.0});
}

#if defined(PULP_HOST_ENABLE_GPU_CONVOLUTION)
ForgeNodeDescriptor descriptor();
ForgeNodeDescriptor descriptor_with_gpu() {
    auto d = descriptor();
    d.realizations.emplace_back("gpu", kGpuTypeId);
    return d;
}
#endif

ForgeNodeDescriptor descriptor() {
    return {
        "convolution_reverb",
        "Convolution Reverb",
        "Applies a supplied impulse response with stereo wet-path shaping.",
        {},
        {{"default", kTypeId}},
        {{"ir_gain_db", kIrGainDb, "IR Gain", "dB", "Trims the convolved return.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"predelay_ms", kPredelayMs, "Predelay", "ms", "Delays the wet return.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"wet_percent", kWetPercent, "Wet", "%", "Sets the convolved signal level.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"dry_percent", kDryPercent, "Dry", "%", "Sets the direct signal level.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"width_percent", kWidthPercent, "Width", "%", "Shapes stereo width on the wet return.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"lowcut_hz", kLowcutHz, "Low Cut", "Hz", "High-passes the reverb send.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"highcut_hz", kHighcutHz, "High Cut", "Hz", "Low-passes the reverb send.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic}}};
}

} // namespace convolution

// ── The nonlin / gated ambience (designed spaces) ──────────────────────────
//
// One node type, four programs. The program is a PARAM and not a realization,
// which is the one classification here most worth arguing rather than
// asserting:
//
//   * It does not move `latency_samples()` — that is a constant 0 for every
//     program.
//   * The signal path is unchanged. All four programs run the same velvet tap
//     cloud through the same segment filters; what differs is the sequence of
//     tap GAINS, which is a coefficient set, not a topology. (The Reverse
//     program additionally mirrors the segment mapping, which is one flag on
//     the same filters — not a second filter bank.)
//   * The DSP was built for the switch to happen live. It regenerates into a
//     back tap bank and crossfades in over `kSwapFadeMs` precisely so a program
//     change is click-free and allocation-free at run time, and its suite
//     asserts that the swap produces no step larger than the signal already
//     contains.
//   * And it is the front panel. The four names ARE the machine; freezing the
//     program at registration would ship four nodes that a user cannot switch
//     between, for a lineage whose identity is that you turn that one knob.
//
// That is the same reading the FET member's ratio switch gets — a stepped
// front-panel control over an unchanged path with invariant latency — and the
// opposite of the diode bridge's feedback switch, which re-maps a measured
// static curve into a different function.
namespace nonlin_ambience {
using Engine = signal::NonlinAmbience;
namespace cal = signal::nonlin_ambience;
using ::pulp::host::space::nonlin_ambience::kAttackPct;
using ::pulp::host::space::nonlin_ambience::kAttackPctMax;
using ::pulp::host::space::nonlin_ambience::kAttackPctMin;
using ::pulp::host::space::nonlin_ambience::kConverterAmount;
using ::pulp::host::space::nonlin_ambience::kDensityGrowth;
using ::pulp::host::space::nonlin_ambience::kDensityPct;
using ::pulp::host::space::nonlin_ambience::kDensityPctMin;
using ::pulp::host::space::nonlin_ambience::kDiffusion;
using ::pulp::host::space::nonlin_ambience::kGateHoldPct;
using ::pulp::host::space::nonlin_ambience::kGateHoldPctMax;
using ::pulp::host::space::nonlin_ambience::kGateHoldPctMin;
using ::pulp::host::space::nonlin_ambience::kHfDampHz;
using ::pulp::host::space::nonlin_ambience::kLengthMs;
using ::pulp::host::space::nonlin_ambience::kLengthMsDefault;
using ::pulp::host::space::nonlin_ambience::kLengthMsMin;
using ::pulp::host::space::nonlin_ambience::kMixPct;
using ::pulp::host::space::nonlin_ambience::kOutputGainDb;
using ::pulp::host::space::nonlin_ambience::kOutputGainDbMax;
using ::pulp::host::space::nonlin_ambience::kPredelayMs;
using ::pulp::host::space::nonlin_ambience::kPredelayMsMax;
using ::pulp::host::space::nonlin_ambience::kProgram;
using ::pulp::host::space::nonlin_ambience::kProgramSteps;
using ::pulp::host::space::nonlin_ambience::kTone;
using ::pulp::host::space::nonlin_ambience::kTypeId;
using ::pulp::host::space::nonlin_ambience::kWidthPct;

// Topology — read once per block (see the file note on param rate).

/// The program selector's step count. Four documented programs, injected as
/// 0..3 and rounded — the same shape as the FET member's ratio switch.

/// Node-level ceilings for the ranges the DSP does not publish as constants.
/// Each mirrors the module's baked-params table.
/// [design parameter] defaults as shown; ranges are the DSP's own.

/// Forwards `value` to `apply` only when it differs from what was last
/// forwarded.
///
/// This is not a micro-optimisation, it is a CORRECTNESS guard, and it is here
/// rather than in the DSP because the trap is in a shared primitive that this
/// file must not edit. `SmoothedValue::set_target()` has no unchanged-value
/// guard: every call recomputes `increment = (target - current) / ramp_samples`
/// and restarts the ramp. So a node that calls a SmoothedValue-backed setter
/// once per sample with a HELD value converts the documented 20 ms LINEAR ramp
/// into an exponential approach with a 20 ms TIME CONSTANT — measured, it
/// reaches 0.3673 of its journey at the ramp length instead of 1.0, and never
/// reaches the target exactly at all. Four of the ambience's continuous setters
/// are SmoothedValue-backed (`width`, `converter_amount`, `output_gain`,
/// `mix`), so without this guard "mix = 0 %" never becomes the dry wire and
/// "width = 0 %" never becomes exactly mono.
///
/// `last` starts as NaN so the first sample always forwards (`x == NaN` is
/// false for every x, including NaN).
template <typename Fn> void forward_if_changed(float& last, float value, Fn&& apply) {
    if (value == last)
        return;
    last = value;
    apply(value);
}

struct Instance {
    Engine engine;
    /// Last value forwarded for each continuous param. See
    /// `forward_if_changed`.
    float last_diffusion = std::numeric_limits<float>::quiet_NaN();
    float last_tone = std::numeric_limits<float>::quiet_NaN();
    float last_hf_damp = std::numeric_limits<float>::quiet_NaN();
    float last_width = std::numeric_limits<float>::quiet_NaN();
    float last_converter = std::numeric_limits<float>::quiet_NaN();
    float last_output_gain = std::numeric_limits<float>::quiet_NaN();
    float last_mix = std::numeric_limits<float>::quiet_NaN();
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The DSP ships the bound as a closed form of its own shipped constants —
/// `Pi_i(1 + 2*g_i) * G_L1`, an upper bound on the rendered response's
/// `sum|h[n]|` by Young's convolution inequality — and its suite renders the
/// impulse response and asserts the measured sum stays under it. This composes
/// that same closed form at THIS node's ceilings:
///
///   * `diffusion` at its maximum, not its default: 0.85 gives `(1+1.7)^2`
///     rather than the default's `(1+1.4)^2`.
///   * the converter stage engaged, which adds its DC blocker's L1 gain of
///     exactly 2.
///   * `output_gain_db` at +24, the post trim's ceiling.
///
/// `width_pct` and `mix_pct` contribute nothing: this module's width law is a
/// convex mid/side blend capped at 100 % (unlike the convolver's, which reaches
/// 200 %), and the dry/wet law is `(1-m, m)`.
float nonlin_ambience_worst_case_gain() {
    const double core = cal::worst_case_gain(cal::kDiffusionMax, /*converter_on=*/true);
    return static_cast<float>(core * std::pow(10.0, kOutputGainDbMax / 20.0));
}

/// The designed-space ambience as a lowerable custom node.
///
/// `seed` and `max_length_ms` are registration arguments; see the file note,
/// items 4 and 5. The seed selects which velvet realization this node IS — two
/// nodes differing only in seed are two different rooms — and law 2 forbids
/// automating it.
CustomNodeType make_nonlin_ambience_node(std::uint32_t seed, double max_length_ms) {
    const double normalized_max_length_ms = std::isfinite(max_length_ms)
                                                ? std::max(cal::kMinLengthMs, max_length_ms)
                                                : cal::kMaxLengthMs;
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2; // 0 = left, 1 = right (ONE logical stereo wire)
    t.num_output_ports = 2;
    t.default_name = "Nonlin Ambience";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [seed, normalized_max_length_ms](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        // Seed before prepare: prepare builds the first tap table, and building
        // it twice to honour a seed set afterwards would be waste.
        s->engine.set_seed(seed);
        s->engine.prepare(sr, normalized_max_length_ms);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };

    t.baked_params.push_back({kProgram, 0.0f, kProgramSteps, 0.0f});
    t.baked_params.push_back(
        {kLengthMs, kLengthMsMin, static_cast<float>(normalized_max_length_ms),
         std::min(kLengthMsDefault, static_cast<float>(normalized_max_length_ms))});
    t.baked_params.push_back({kPredelayMs, 0.0f, kPredelayMsMax, 0.0f});
    t.baked_params.push_back(
        {kDensityPct, kDensityPctMin, 100.0f, static_cast<float>(cal::kDensityRefPct)});
    t.baked_params.push_back({kDensityGrowth, 0.0f, 2.0f, static_cast<float>(cal::kGammaDefault)});
    t.baked_params.push_back({kGateHoldPct, kGateHoldPctMin, kGateHoldPctMax,
                              static_cast<float>(cal::kGateHold * 100.0)});
    t.baked_params.push_back(
        {kAttackPct, kAttackPctMin, kAttackPctMax, static_cast<float>(cal::kRevRise * 100.0)});
    t.baked_params.push_back({kDiffusion, 0.0f, static_cast<float>(cal::kDiffusionMax),
                              static_cast<float>(cal::kDiffusionDefault)});
    t.baked_params.push_back({kTone, -1.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kHfDampHz, 1000.0f, 18000.0f, static_cast<float>(cal::kFcDark)});
    t.baked_params.push_back({kWidthPct, 0.0f, 100.0f, 100.0f});
    t.baked_params.push_back({kConverterAmount, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kOutputGainDb, -kOutputGainDbMax, kOutputGainDbMax, 0.0f});
    t.baked_params.push_back({kMixPct, 0.0f, 100.0f, 100.0f}); // send-style default

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);

        // Topology is one atomic snapshot per block. Several simultaneous
        // events must regenerate the inactive tap bank once, not once per
        // parameter (seven full 8000-tap/channel walks in one callback).
        const float program_value = params.value_at(kProgram, 0);
        const int program = std::isfinite(program_value)
                                ? std::clamp(static_cast<int>(std::lround(program_value)), 0,
                                             static_cast<int>(kProgramSteps))
                                : static_cast<int>(s->engine.program());
        s->engine.request_topology(
            static_cast<signal::NonlinProgram>(program), params.value_at(kLengthMs, 0),
            params.value_at(kPredelayMs, 0), params.value_at(kDensityPct, 0),
            params.value_at(kDensityGrowth, 0), params.value_at(kGateHoldPct, 0),
            params.value_at(kAttackPct, 0));

        const float* in_left = in.channel_ptr(0);
        const float* in_right = in.channel_ptr(1);
        float* out_left = out.channel_ptr(0);
        float* out_right = out.channel_ptr(1);

        // Continuous: per sample, but only forwarded on CHANGE — see
        // `forward_if_changed`, which is load-bearing rather than thrifty. Each
        // setter is otherwise a clamp, a store, and at most a handful of `exp`
        // calls for the segment corners.
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            forward_if_changed(s->last_diffusion, params.value_at(kDiffusion, offset),
                               [s](float v) { s->engine.set_diffusion(v); });
            forward_if_changed(s->last_tone, params.value_at(kTone, offset),
                               [s](float v) { s->engine.set_tone(v); });
            forward_if_changed(s->last_hf_damp, params.value_at(kHfDampHz, offset),
                               [s](float v) { s->engine.set_hf_damp_hz(v); });
            forward_if_changed(s->last_width, params.value_at(kWidthPct, offset),
                               [s](float v) { s->engine.set_width_pct(v); });
            forward_if_changed(s->last_converter, params.value_at(kConverterAmount, offset),
                               [s](float v) { s->engine.set_converter_amount(v); });
            forward_if_changed(s->last_output_gain, params.value_at(kOutputGainDb, offset),
                               [s](float v) { s->engine.set_output_gain_db(v); });
            forward_if_changed(s->last_mix, params.value_at(kMixPct, offset),
                               [s](float v) { s->engine.set_mix_pct(v); });

            float left = in_left[static_cast<std::size_t>(k)];
            float right = in_right[static_cast<std::size_t>(k)];
            s->engine.process_sample(left, right);
            out_left[static_cast<std::size_t>(k)] = left;
            out_right[static_cast<std::size_t>(k)] = right;
        }
    };
    return t;
}

ForgeNodeDescriptor descriptor() {
    return {"nonlin_ambience",
            "Nonlinear Ambience",
            "A designed stereo ambience with gated, reverse, and nonlinear envelope programs.",
            {},
            {{"default", kTypeId}},
            {{"program",
              kProgram,
              "Program",
              "",
              "Selects the ambience envelope program.",
              ForgeParamKind::stepped,
              ForgeParamCurve::linear,
              {{"nonlinear_one", "Nonlinear One", 0},
               {"gated", "Gated", 1},
               {"reverse", "Reverse", 2},
               {"nonlinear_two", "Nonlinear Two", 3}}},
             {"length_ms", kLengthMs, "Length", "ms", "Sets the ambience duration.",
              ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
             {"predelay_ms", kPredelayMs, "Predelay", "ms", "Delays the wet response.",
              ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"density_pct", kDensityPct, "Density", "%", "Sets the initial reflection density.",
              ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"density_growth", kDensityGrowth, "Density Growth", "",
              "Shapes density across the response.", ForgeParamKind::continuous,
              ForgeParamCurve::linear},
             {"gate_hold_pct", kGateHoldPct, "Gate Hold", "%",
              "Sets the hold portion of the gated program.", ForgeParamKind::continuous,
              ForgeParamCurve::linear},
             {"attack_pct", kAttackPct, "Attack", "%",
              "Sets the rise portion of reverse and nonlinear programs.",
              ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"diffusion", kDiffusion, "Diffusion", "", "Smears reflection detail.",
              ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"tone", kTone, "Tone", "", "Moves the response from dark to bright.",
              ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"hf_damp_hz", kHfDampHz, "HF Damping", "Hz", "Sets high-frequency damping.",
              ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
             {"width_pct", kWidthPct, "Width", "%", "Sets stereo width.",
              ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"converter_amount", kConverterAmount, "Converter", "%", "Adds converter coloration.",
              ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"output_gain_db", kOutputGainDb, "Output Gain", "dB", "Trims the processed output.",
              ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"mix_pct", kMixPct, "Mix", "%", "Blends dry and ambience signals.",
              ForgeParamKind::continuous, ForgeParamCurve::linear}}};
}

} // namespace nonlin_ambience

namespace cabinet {
using Engine = signal::SpeakerModel;
using ::pulp::host::space::cabinet::kBox;
using ::pulp::host::space::cabinet::kBreakupPct;
using ::pulp::host::space::cabinet::kCompressionPct;
using ::pulp::host::space::cabinet::kDiffractionPct;
using ::pulp::host::space::cabinet::kDriveDb;
using ::pulp::host::space::cabinet::kDriver;
using ::pulp::host::space::cabinet::kMicAxisDeg;
using ::pulp::host::space::cabinet::kMicDistanceCm;
using ::pulp::host::space::cabinet::kMicPositionPct;
using ::pulp::host::space::cabinet::kOutputTrimDb;
using ::pulp::host::space::cabinet::kQ;
using ::pulp::host::space::cabinet::kResonanceTrimSt;
using ::pulp::host::space::cabinet::kTrebleHz;
using ::pulp::host::space::cabinet::kTypeId;
using ::pulp::host::space::cabinet::kVolumeL;

struct Instance {
    Engine engine;
    std::array<float, 14> last_params = [] {
        std::array<float, 14> values{};
        values.fill(std::numeric_limits<float>::quiet_NaN());
        return values;
    }();
};

float speaker_cabinet_worst_case_gain() {
    return static_cast<float>(Engine{}.worst_case_gain() *
                              signal::units::db_to_linear(Engine::kOutputTrimDbMax));
}

CustomNodeType make_speaker_cabinet_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Speaker Cabinet";
    t.lowerable = true;
    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sr, int) { static_cast<Instance*>(p)->engine.prepare(sr); };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };
    t.baked_params.push_back(
        {kDriver, 0.0f, static_cast<float>(Engine::kArchetypeCount - 1), 0.0f});
    t.baked_params.push_back({kBox, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kVolumeL, static_cast<float>(Engine::kBoxVolumeLMin),
                              static_cast<float>(Engine::kBoxVolumeLMax),
                              static_cast<float>(Engine::kBoxVolumeLDefault)});
    t.baked_params.push_back({kResonanceTrimSt,
                              static_cast<float>(Engine::kResonanceTrimSemitonesMin),
                              static_cast<float>(Engine::kResonanceTrimSemitonesMax), 0.0f});
    t.baked_params.push_back({kQ, 0.0f, static_cast<float>(Engine::kQResonanceMax), 0.0f});
    t.baked_params.push_back(
        {kBreakupPct, 0.0f, 100.0f, static_cast<float>(Engine::kConeBreakupAmountDefault)});
    t.baked_params.push_back({kTrebleHz, static_cast<float>(Engine::kTrebleRolloffHzMin),
                              static_cast<float>(Engine::kTrebleRolloffHzMax),
                              static_cast<float>(Engine::kTrebleRolloffHzDefault)});
    t.baked_params.push_back({kDriveDb, static_cast<float>(Engine::kDriveDbMin),
                              static_cast<float>(Engine::kDriveDbMax),
                              static_cast<float>(Engine::kDriveDbDefault)});
    t.baked_params.push_back(
        {kCompressionPct, 0.0f, 100.0f, static_cast<float>(Engine::kCompressionAmountDefault)});
    t.baked_params.push_back({kMicDistanceCm, static_cast<float>(Engine::kMicDistanceCmMin),
                              static_cast<float>(Engine::kMicDistanceCmMax),
                              static_cast<float>(Engine::kMicDistanceCmDefault)});
    t.baked_params.push_back(
        {kMicPositionPct, 0.0f, 100.0f, static_cast<float>(Engine::kMicPositionPctDefault)});
    t.baked_params.push_back({kMicAxisDeg, static_cast<float>(Engine::kMicAxisDegMin),
                              static_cast<float>(Engine::kMicAxisDegMax), 0.0f});
    t.baked_params.push_back(
        {kDiffractionPct, 0.0f, 100.0f, static_cast<float>(Engine::kDiffractionAmountDefault)});
    t.baked_params.push_back({kOutputTrimDb, static_cast<float>(Engine::kOutputTrimDbMin),
                              static_cast<float>(Engine::kOutputTrimDbMax), 0.0f});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto& e = static_cast<Instance*>(p)->engine;
        auto& last = static_cast<Instance*>(p)->last_params;
        for (int i = 0; i < n; ++i) {
            const auto o = static_cast<std::int32_t>(i);
            auto forward = [&](std::size_t slot, state::ParamID id, auto&& setter) {
                const float value = params.value_at(id, o);
                if (std::isfinite(value) && value != last[slot]) {
                    last[slot] = value;
                    setter(value);
                }
            };
            forward(0, kDriver,
                    [&](float v) { e.set_driver_archetype(static_cast<int>(std::lround(v))); });
            forward(1, kBox, [&](float v) {
                e.set_box_type(v >= 0.5f ? signal::SpeakerBoxType::open_back
                                         : signal::SpeakerBoxType::sealed);
            });
            forward(2, kVolumeL, [&](float v) { e.set_box_volume_l(v); });
            forward(3, kResonanceTrimSt, [&](float v) { e.set_resonance_trim_semitones(v); });
            forward(4, kQ, [&](float v) { e.set_q_resonance(v); });
            forward(5, kBreakupPct, [&](float v) { e.set_cone_breakup_amount(v); });
            forward(6, kTrebleHz, [&](float v) { e.set_treble_rolloff_hz(v); });
            forward(7, kDriveDb, [&](float v) { e.set_drive_db(v); });
            forward(8, kCompressionPct, [&](float v) { e.set_compression_amount(v); });
            forward(9, kMicDistanceCm, [&](float v) { e.set_mic_distance_cm(v); });
            forward(10, kMicPositionPct, [&](float v) { e.set_mic_position_pct(v); });
            forward(11, kMicAxisDeg, [&](float v) { e.set_mic_axis_deg(v); });
            forward(12, kDiffractionPct, [&](float v) { e.set_diffraction_amount(v); });
            forward(13, kOutputTrimDb, [&](float v) { e.set_output_trim_db(v); });
            out.channel_ptr(0)[i] = e.process(in.channel_ptr(0)[i]);
        }
    };
    return t;
}

/// Compatibility spelling retained for the original public design contract.
/// Both names return the same stable node type and parameter surface.
CustomNodeType make_speaker_emulation_node() {
    return make_speaker_cabinet_node();
}

ForgeNodeDescriptor descriptor() {
    return {
        "speaker_cabinet",
        "Speaker Cabinet",
        "Models a driven loudspeaker, enclosure, microphone, and diffraction path.",
        {},
        {{"default", kTypeId}},
        {{"driver",
          kDriver,
          "Driver",
          "",
          "Selects the loudspeaker driver archetype.",
          ForgeParamKind::stepped,
          ForgeParamCurve::linear,
          {{"brit_twelve_ceramic", "British Twelve Ceramic", 0},
           {"amer_twelve_ceramic", "American Twelve Ceramic", 1},
           {"alnico_twelve", "Alnico Twelve", 2},
           {"brit_ten", "British Ten", 3},
           {"bass_fifteen", "Bass Fifteen", 4}}},
         {"box",
          kBox,
          "Box",
          "",
          "Selects a sealed or open-back enclosure.",
          ForgeParamKind::stepped,
          ForgeParamCurve::linear,
          {{"sealed", "Sealed", 0}, {"open_back", "Open Back", 1}}},
         {"volume_l", kVolumeL, "Box Volume", "L", "Sets enclosure volume.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"resonance_trim_st", kResonanceTrimSt, "Resonance Trim", "st",
          "Retunes the enclosure resonance.", ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"q", kQ, "Resonance Q", "", "Sets resonance emphasis.", ForgeParamKind::continuous,
          ForgeParamCurve::linear},
         {"breakup_pct", kBreakupPct, "Cone Breakup", "%", "Adds cone-breakup coloration.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"treble_hz", kTrebleHz, "Treble Rolloff", "Hz", "Sets the high-frequency rolloff.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"drive_db", kDriveDb, "Drive", "dB", "Drives the speaker nonlinearity.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"compression_pct", kCompressionPct, "Compression", "%", "Adds power compression.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"mic_distance_cm", kMicDistanceCm, "Mic Distance", "cm", "Sets microphone distance.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"mic_position_pct", kMicPositionPct, "Mic Position", "%",
          "Moves the microphone from center toward edge.", ForgeParamKind::continuous,
          ForgeParamCurve::linear},
         {"mic_axis_deg", kMicAxisDeg, "Mic Axis", "deg", "Turns the microphone off axis.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"diffraction_pct", kDiffractionPct, "Diffraction", "%", "Adds cabinet-edge diffraction.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"output_trim_db", kOutputTrimDb, "Output Trim", "dB", "Trims the modeled output.",
          ForgeParamKind::continuous, ForgeParamCurve::linear}}};
}

} // namespace cabinet

} // namespace pulp::host::space
