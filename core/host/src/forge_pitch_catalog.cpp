#include <pulp/host/detail/forge_realization_identity.hpp>
#include <pulp/host/forge_pitch_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::host::pitch {
namespace whammy {

/// Decodes the stepped `pedal_mode` value. Rounds rather than truncates so a
/// host that sends 0.9999 for "harmony" gets harmony instead of whammy.
signal::PedalMode decode_pedal_mode(float v) {
    const int i = std::clamp(static_cast<int>(std::lround(v)), 0, 3);
    switch (i) {
    case 1:
        return signal::PedalMode::harmony;
    case 2:
        return signal::PedalMode::detune;
    case 3:
        return signal::PedalMode::dive;
    default:
        return signal::PedalMode::whammy;
    }
}

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// Cited from the invariant the DSP suite asserts, not re-derived here.
///
/// This used to be `kWorstCaseGain * dc_blocker_peak_gain()` with
/// `kWorstCaseGain = √2` and the DC blocker contributing `2/(1+p)` at Nyquist —
/// 1.000327 at 48 kHz, so the product was 1.4147. Measured, the node reaches
/// 2.2117 on sustained near-DC content: the registry was understating the
/// headroom a downstream stage plans against by 3.9 dB.
///
/// The error was using the DC blocker's MAGNITUDE-response peak as a bound on
/// its worst single SAMPLE. Those are different quantities; the sample bound is
/// the impulse response's L1 norm, which is exactly 2 at every pole position.
/// `Shifter::kWorstCaseGain` now folds that in — an equal-power mix maximises
/// `cos θ · 1 + sin θ · 2` at `√5` — so it is the whole bound and multiplying
/// by the blocker again would double-count it.
///
/// No longer sample-rate dependent, and that is a consequence rather than a
/// simplification: the L1 norm of `y[n] = x[n] − x[n−1] + p·y[n−1]` is 2 for
/// every `p`, so the pole's position (and hence the rate) drops out.
float whammy_worst_case_gain(double /*sample_rate*/) {
    return static_cast<float>(Shifter::kWorstCaseGain);
}

/// The node's reported latency at a given window and sample rate, in samples.
///
/// Exposed because `CustomNodeType` cannot declare latency and the graph's PDC
/// therefore sees zero (see the gap note at the top of this file). A host that
/// needs to align a whammy branch against a dry one has to read this and
/// compensate itself. Computed by the shipped block rather than by repeating
/// its expression here.
int whammy_latency_samples(double sample_rate, double window_ms) {
    Shifter probe;
    probe.prepare(sample_rate);
    probe.set_window_ms(window_ms);
    return probe.latency_samples();
}

/// The conventional dry/wet default for a mode, for the PRESET layer.
/// Deliberately not applied when the `pedal_mode` param moves — see the note at
/// the top of this file.
float whammy_default_mix_for_mode(signal::PedalMode mode) {
    return static_cast<float>(Shifter::default_mix_for(mode));
}

/// The expression-pedal pitch shifter as a lowerable custom node.
///
/// `window_ms` is a construction-time argument, not a param: it IS this
/// member's `latency_samples()`, and it sets the warble rate. The DSP sizes its
/// line for the whole declared range at `prepare()`, so any value in range is
/// allocation-free — it is frozen here for the host's sake.
CustomNodeType make_whammy_node(double window_ms) {
    const double fixed_window_ms =
        std::clamp(std::isfinite(window_ms) ? window_ms : Shifter::kWindowMsDefault,
                   Shifter::kWindowMsMin, Shifter::kWindowMsMax);
    CustomNodeType t;
    t.type_id = kTypeId;
    if (fixed_window_ms != Shifter::kWindowMsDefault)
        t.type_id += ".window_" + detail::realization_real_token(fixed_window_ms);
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Whammy";
    t.lowerable = true;
    t.latency_samples = [fixed_window_ms](double sample_rate) {
        Shifter probe;
        probe.prepare(sample_rate);
        probe.set_window_ms(fixed_window_ms);
        return probe.latency_samples();
    };

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [fixed_window_ms](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->shifter.prepare(sr);
        // After `prepare()`, which is what sized the line for the whole window
        // range. Setting it here rather than before also means the value
        // survives a re-prepare at a new sample rate, which is where a
        // construction-time realization would otherwise quietly revert.
        s->shifter.set_window_ms(fixed_window_ms);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->shifter.reset(); };

    // Ranges and defaults are the module's canonical contract, in REAL units.
    // Each row is a design-parameter declaration per the series contract; the
    // `set_*` comments in the DSP header mirror these same numbers for
    // readability at the call site and are not a second declaration.
    using S = Shifter;
    t.baked_params.push_back({kPedal, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kPedalMode, kModeWhammy, kModeDive, kModeWhammy});
    t.baked_params.push_back({kShiftSource, kSourcePedal, kSourceDirect, kSourcePedal});
    t.baked_params.push_back({kShiftSemitones, static_cast<float>(S::kShiftSemisMin),
                              static_cast<float>(S::kShiftSemisMax), 0.0f});
    t.baked_params.push_back({kHeelSemis, static_cast<float>(S::kShiftSemisMin),
                              static_cast<float>(S::kShiftSemisMax),
                              static_cast<float>(S::kHeelSemisDefault)});
    t.baked_params.push_back({kToeSemis, static_cast<float>(S::kShiftSemisMin),
                              static_cast<float>(S::kShiftSemisMax),
                              static_cast<float>(S::kToeSemisDefault)});
    // Harmony intervals clamp to ±24 in the DSP rather than to the shift range;
    // the node's row mirrors the DSP's clamp so a host's knob cannot travel
    // somewhere the block will silently pull it back from.
    t.baked_params.push_back(
        {kIntervalASemis, -24.0f, 24.0f, static_cast<float>(S::kIntervalASemisDefault)});
    t.baked_params.push_back(
        {kIntervalBSemis, -24.0f, 24.0f, static_cast<float>(S::kIntervalBSemisDefault)});
    t.baked_params.push_back({kDetuneCents, -static_cast<float>(S::kDetuneCentsMax),
                              static_cast<float>(S::kDetuneCentsMax),
                              static_cast<float>(S::kDetuneCentsDefault)});
    t.baked_params.push_back({kDiveFloorSemis, static_cast<float>(S::kDiveFloorSemisMin),
                              static_cast<float>(S::kDiveFloorSemisMax),
                              static_cast<float>(S::kDiveFloorSemisDefault)});
    t.baked_params.push_back({kGlideUpMs, static_cast<float>(S::kGlideMsMin),
                              static_cast<float>(S::kGlideMsMax),
                              static_cast<float>(S::kGlideMsDefault)});
    t.baked_params.push_back({kGlideDownMs, static_cast<float>(S::kGlideMsMin),
                              static_cast<float>(S::kGlideMsMax),
                              static_cast<float>(S::kGlideMsDefault)});
    // The whammy mode's conventional default (dry muted). Other modes' defaults
    // are a preset concern, not a live consequence of the mode param.
    t.baked_params.push_back(
        {kMix, 0.0f, 1.0f, whammy_default_mix_for_mode(signal::PedalMode::whammy)});
    t.baked_params.push_back({kDetents, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kInterp, kInterpLinear, kInterpCubic, kInterpLinear});
    t.baked_params.push_back({kDriftDepth, 0.0f, 1.0f, 0.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);

        // Sample at a time so every control is sample-accurate — a pedal sweep
        // is the point of this node, and a block-rate pedal would stair-step
        // the one gesture it exists to perform. The setters are a clamp and a
        // store apiece; the slew and the ratio are recomputed inside `process`
        // regardless, so this costs a handful of inlined calls per sample
        // rather than any re-preparation.
        //
        // NOT set here: `set_window_ms`, which is the registration-time
        // realization above. Adding it to this loop would silently make the
        // node's latency automatable.
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->shifter.set_shift_source(params.value_at(kShiftSource, offset) >= 0.5f
                                            ? signal::ShiftSource::direct
                                            : signal::ShiftSource::pedal);
            s->shifter.set_pedal_mode(decode_pedal_mode(params.value_at(kPedalMode, offset)));
            s->shifter.set_pedal(params.value_at(kPedal, offset));
            s->shifter.set_shift_semitones(params.value_at(kShiftSemitones, offset));
            s->shifter.set_targets(params.value_at(kHeelSemis, offset),
                                   params.value_at(kToeSemis, offset));
            s->shifter.set_harmony(params.value_at(kIntervalASemis, offset),
                                   params.value_at(kIntervalBSemis, offset));
            s->shifter.set_detune_cents(params.value_at(kDetuneCents, offset));
            s->shifter.set_dive_floor_semis(params.value_at(kDiveFloorSemis, offset));
            s->shifter.set_glide_ms(params.value_at(kGlideUpMs, offset),
                                    params.value_at(kGlideDownMs, offset));
            s->shifter.set_mix(params.value_at(kMix, offset));
            s->shifter.set_detents(params.value_at(kDetents, offset) >= 0.5f);
            s->shifter.set_interp(params.value_at(kInterp, offset) >= 0.5f
                                      ? signal::PitchInterp::cubic
                                      : signal::PitchInterp::linear);
            s->shifter.set_drift_depth(params.value_at(kDriftDepth, offset));

            output[static_cast<std::size_t>(k)] =
                s->shifter.process(input[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

ForgeNodeDescriptor descriptor() {
    return {
        "whammy",
        "Whammy",
        "An expression-controlled pitch shifter for bends, harmony, detune, and dives.",
        {},
        {{"default", kTypeId}},
        {
            {"pedal", kPedal, "Pedal", "%", "Moves between the heel and toe targets.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"pedal_mode",
             kPedalMode,
             "Pedal Mode",
             "",
             "Selects the pedal's pitch behavior.",
             ForgeParamKind::stepped,
             ForgeParamCurve::linear,
             {{"whammy", "Whammy", kModeWhammy},
              {"harmony", "Harmony", kModeHarmony},
              {"detune", "Detune", kModeDetune},
              {"dive", "Dive", kModeDive}}},
            {"shift_source",
             kShiftSource,
             "Shift Source",
             "",
             "Selects pedal mapping or a direct pitch target.",
             ForgeParamKind::stepped,
             ForgeParamCurve::linear,
             {{"pedal", "Pedal", kSourcePedal}, {"direct", "Direct", kSourceDirect}}},
            {"shift_semitones", kShiftSemitones, "Shift", "st", "Sets the direct pitch target.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"heel_semitones", kHeelSemis, "Heel", "st", "Sets pitch at the heel position.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"toe_semitones", kToeSemis, "Toe", "st", "Sets pitch at the toe position.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"interval_a_semitones", kIntervalASemis, "Harmony A", "st",
             "Sets the harmony interval at heel.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"interval_b_semitones", kIntervalBSemis, "Harmony B", "st",
             "Sets the harmony interval at toe.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"detune_cents", kDetuneCents, "Detune", "cent", "Sets the detune-mode pitch spread.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"dive_floor_semitones", kDiveFloorSemis, "Dive Floor", "st",
             "Sets the deepest dive target.", ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"glide_up_ms", kGlideUpMs, "Glide Up", "ms", "Sets upward pitch slew.",
             ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
            {"glide_down_ms", kGlideDownMs, "Glide Down", "ms", "Sets downward pitch slew.",
             ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
            {"mix", kMix, "Mix", "%", "Blends dry and shifted signals.", ForgeParamKind::continuous,
             ForgeParamCurve::linear},
            {"detents",
             kDetents,
             "Detents",
             "",
             "Snaps pedal targets to musical detents.",
             ForgeParamKind::stepped,
             ForgeParamCurve::linear,
             {{"off", "Off", 0}, {"on", "On", 1}}},
            {"interpolation",
             kInterp,
             "Interpolation",
             "",
             "Selects the delay-line interpolation quality.",
             ForgeParamKind::stepped,
             ForgeParamCurve::linear,
             {{"linear", "Linear", kInterpLinear}, {"cubic", "Cubic", kInterpCubic}}},
            {"drift_depth", kDriftDepth, "Drift", "%", "Adds slow pitch instability.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
        }};
}

} // namespace whammy

namespace harmony {
CustomNodeType make_harmony_engine_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Harmony Engine";
    t.lowerable = true;
    t.latency_samples = [](double sample_rate) {
        Engine probe;
        probe.prepare(sample_rate);
        return probe.latency_samples();
    };
    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        static_cast<Instance*>(p)->engine.prepare(sr);
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };
    t.baked_params = {
        {kKey, 0.0f, 11.0f, 0.0f},
        {kScale, 0.0f, static_cast<float>(signal::kScaleCount - 1), 0.0f},
        {kV1Interval, -14.0f, 14.0f, static_cast<float>(Engine::kV1IntervalDefault)},
        {kV1Detune, static_cast<float>(-Engine::kDetuneMaxCents),
         static_cast<float>(Engine::kDetuneMaxCents), 0.0f},
        {kV1Level, static_cast<float>(Engine::kLevelMinDb), static_cast<float>(Engine::kLevelMaxDb),
         0.0f},
        {kV2Enable, 0.0f, 1.0f, 0.0f},
        {kV2Interval, -14.0f, 14.0f, static_cast<float>(Engine::kV2IntervalDefault)},
        {kV2Detune, static_cast<float>(-Engine::kDetuneMaxCents),
         static_cast<float>(Engine::kDetuneMaxCents), 0.0f},
        {kV2Level, static_cast<float>(Engine::kLevelMinDb), static_cast<float>(Engine::kLevelMaxDb),
         0.0f},
        {kGlideMs, 0.0f, static_cast<float>(Engine::kGlideMsMax),
         static_cast<float>(Engine::kGlideMsDefault)},
        {kDryLevel, static_cast<float>(Engine::kLevelMinDb),
         static_cast<float>(Engine::kLevelMaxDb), 0.0f},
        {kHumanize, 0.0f, static_cast<float>(Engine::kHumanizeMaxCents),
         static_cast<float>(Engine::kHumanizeDefault)},
    };
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto& engine = static_cast<Instance*>(p)->engine;
        const float* input = in.channel_ptr(0);
        float* output = out.channel_ptr(0);
        for (int i = 0; i < n; ++i) {
            const auto offset = static_cast<std::int32_t>(i);
            engine.set_key(static_cast<int>(std::lround(params.value_at(kKey, offset))));
            const int scale =
                std::clamp(static_cast<int>(std::lround(params.value_at(kScale, offset))), 0,
                           signal::kScaleCount - 1);
            engine.set_scale(static_cast<signal::ScaleType>(scale));
            engine.set_voice_interval(
                0, static_cast<int>(std::lround(params.value_at(kV1Interval, offset))));
            engine.set_voice_detune_cents(0, params.value_at(kV1Detune, offset));
            engine.set_voice_level_db(0, params.value_at(kV1Level, offset));
            engine.set_voice_enabled(1, params.value_at(kV2Enable, offset) >= 0.5f);
            engine.set_voice_interval(
                1, static_cast<int>(std::lround(params.value_at(kV2Interval, offset))));
            engine.set_voice_detune_cents(1, params.value_at(kV2Detune, offset));
            engine.set_voice_level_db(1, params.value_at(kV2Level, offset));
            engine.set_glide_ms(params.value_at(kGlideMs, offset));
            engine.set_dry_level_db(params.value_at(kDryLevel, offset));
            engine.set_humanize_cents(params.value_at(kHumanize, offset));
            output[i] = engine.process(input[i]);
        }
    };
    return t;
}

float worst_case_gain() {
    return static_cast<float>(Engine::kWorstCaseGain);
}

int latency_samples(double sample_rate) {
    Engine engine;
    engine.prepare(sample_rate);
    return engine.latency_samples();
}

ForgeNodeDescriptor descriptor() {
    return {"harmony_engine",
            "Harmony Engine",
            "A scale-aware dual-voice pitch harmonizer with glide and humanization.",
            {},
            {{"default", kTypeId}},
            {
                {"key",
                 kKey,
                 "Key",
                 "",
                 "Selects the tonic pitch class.",
                 ForgeParamKind::stepped,
                 ForgeParamCurve::linear,
                 {{"c", "C", 0},
                  {"c_sharp", "C sharp", 1},
                  {"d", "D", 2},
                  {"d_sharp", "D sharp", 3},
                  {"e", "E", 4},
                  {"f", "F", 5},
                  {"f_sharp", "F sharp", 6},
                  {"g", "G", 7},
                  {"g_sharp", "G sharp", 8},
                  {"a", "A", 9},
                  {"a_sharp", "A sharp", 10},
                  {"b", "B", 11}}},
                {"scale",
                 kScale,
                 "Scale",
                 "",
                 "Selects the pitch-quantization scale.",
                 ForgeParamKind::stepped,
                 ForgeParamCurve::linear,
                 {{"major", "Major", 0},
                  {"natural_minor", "Natural Minor", 1},
                  {"dorian", "Dorian", 2},
                  {"phrygian", "Phrygian", 3},
                  {"lydian", "Lydian", 4},
                  {"mixolydian", "Mixolydian", 5},
                  {"harmonic_minor", "Harmonic Minor", 6},
                  {"melodic_minor", "Melodic Minor", 7},
                  {"major_pentatonic", "Major Pentatonic", 8},
                  {"minor_pentatonic", "Minor Pentatonic", 9}}},
                {"voice_one_interval", kV1Interval, "Voice One Interval", "degree",
                 "Offsets the first harmony voice by scale degrees.", ForgeParamKind::continuous,
                 ForgeParamCurve::linear},
                {"voice_one_detune", kV1Detune, "Voice One Detune", "cent",
                 "Fine-tunes the first harmony voice.", ForgeParamKind::continuous,
                 ForgeParamCurve::linear},
                {"voice_one_level", kV1Level, "Voice One Level", "dB",
                 "Sets the first harmony voice level.", ForgeParamKind::continuous,
                 ForgeParamCurve::linear},
                {"voice_two_enabled",
                 kV2Enable,
                 "Voice Two",
                 "",
                 "Enables the second harmony voice.",
                 ForgeParamKind::stepped,
                 ForgeParamCurve::linear,
                 {{"off", "Off", 0}, {"on", "On", 1}}},
                {"voice_two_interval", kV2Interval, "Voice Two Interval", "degree",
                 "Offsets the second harmony voice by scale degrees.", ForgeParamKind::continuous,
                 ForgeParamCurve::linear},
                {"voice_two_detune", kV2Detune, "Voice Two Detune", "cent",
                 "Fine-tunes the second harmony voice.", ForgeParamKind::continuous,
                 ForgeParamCurve::linear},
                {"voice_two_level", kV2Level, "Voice Two Level", "dB",
                 "Sets the second harmony voice level.", ForgeParamKind::continuous,
                 ForgeParamCurve::linear},
                {"glide_ms", kGlideMs, "Glide", "ms", "Smooths interval changes.",
                 ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
                {"dry_level", kDryLevel, "Dry Level", "dB", "Sets the unshifted signal level.",
                 ForgeParamKind::continuous, ForgeParamCurve::linear},
                {"humanize", kHumanize, "Humanize", "cent",
                 "Adds deterministic pitch variation to harmony voices.",
                 ForgeParamKind::continuous, ForgeParamCurve::linear},
            }};
}

} // namespace harmony

} // namespace pulp::host::pitch
