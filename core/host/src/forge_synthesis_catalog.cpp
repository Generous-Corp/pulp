#include <pulp/host/forge_synthesis_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::host::synthesis {

namespace additive {

float additive_bank_worst_case_gain() {
    return static_cast<float>(signal::AdditiveBank::worst_case_gain());
}

CustomNodeType make_additive_bank_node(Voice voice, int max_partials) {
    const int partials = std::clamp(max_partials, 1, signal::AdditiveBank::kMaxPartialsCeiling);

    CustomNodeType t;
    t.type_id = voice == Voice::organ ? kOrganTypeId : kBellTypeId;
    t.version = 1;
    t.num_input_ports = 1; // port 0 = GATE CV, not audio
    t.num_output_ports = 1;
    t.default_name = voice == Voice::organ ? "Additive Organ" : "Additive Bell";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [voice, partials](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->bank.prepare(sr, partials);
        // After `prepare()`, which is what sized the bank and would otherwise
        // leave the default organ table in place.
        s->bank.load_voice(voice == Voice::organ ? signal::make_organ_voice(partials)
                                                 : signal::make_bell_voice(partials));
        s->gate_high = false;
    };
    t.reset = [](void* p) {
        auto* s = static_cast<Instance*>(p);
        s->bank.reset();
        s->gate_high = false;
    };

    using Bank = signal::AdditiveBank;
    t.baked_params.push_back({kFundamentalHz, static_cast<float>(Bank::kFundamentalMinHz),
                              static_cast<float>(Bank::kFundamentalMaxHz),
                              static_cast<float>(Bank::kFundamentalDefaultHz)});
    t.baked_params.push_back({kPartialCount, 1.0f, static_cast<float>(partials),
                              static_cast<float>(std::min(Bank::kPartialCountDefault, partials))});
    if (voice == Voice::organ) {
        t.baked_params.push_back({kInharmonicity, 0.0f, static_cast<float>(Bank::kInharmonicityMax),
                                  static_cast<float>(Bank::kInharmonicityDefault)});
    }
    t.baked_params.push_back({kSpectralTilt, static_cast<float>(Bank::kSpectralTiltMinDbOct),
                              static_cast<float>(Bank::kSpectralTiltMaxDbOct),
                              static_cast<float>(Bank::kSpectralTiltDefaultDbOct)});
    t.baked_params.push_back({kMasterGainDb, static_cast<float>(Bank::kMasterGainMinDb),
                              static_cast<float>(Bank::kMasterGainMaxDb),
                              static_cast<float>(Bank::kMasterGainDefaultDb)});
    t.baked_params.push_back({kEnvelopeMode, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kRetrigPhase, 0.0f, 2.0f, 0.0f});
    t.baked_params.push_back({kAttackMs, static_cast<float>(Bank::kAttackMinMs),
                              static_cast<float>(Bank::kAttackMaxMs),
                              static_cast<float>(Bank::kAttackDefaultMs)});
    t.baked_params.push_back({kReleaseMs, static_cast<float>(Bank::kReleaseMinMs),
                              static_cast<float>(Bank::kReleaseMaxMs),
                              static_cast<float>(Bank::kReleaseDefaultMs)});
    t.baked_params.push_back({kDetuneCents, 0.0f, static_cast<float>(Bank::kDetuneMaxCents),
                              static_cast<float>(Bank::kDetuneDefaultCents)});

    t.process_instance_baked_param = [voice](void* p, audio::BufferView<float>& out,
                                             const audio::BufferView<const float>& in, int n,
                                             const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* gate = in.channel_ptr(0);
        float* output = out.channel_ptr(0);

        // Every setter here clamps and stores; the bank's real work happens on
        // its own control cadence, so per-sample application is sample-accurate
        // automation rather than a per-sample rebuild.
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->bank.set_fundamental_hz(params.value_at(kFundamentalHz, offset));
            s->bank.set_partial_count(
                static_cast<int>(std::lround(params.value_at(kPartialCount, offset))));
            if (voice == Voice::organ)
                s->bank.set_inharmonicity_b(params.value_at(kInharmonicity, offset));
            s->bank.set_spectral_tilt_db_oct(params.value_at(kSpectralTilt, offset));
            s->bank.set_master_gain_db(params.value_at(kMasterGainDb, offset));
            s->bank.set_envelope_mode(params.value_at(kEnvelopeMode, offset) >= 0.5f
                                          ? signal::AdditiveBank::EnvelopeMode::per_partial_decay
                                          : signal::AdditiveBank::EnvelopeMode::shared_ar);
            const int phase_step = std::clamp(
                static_cast<int>(std::lround(params.value_at(kRetrigPhase, offset))), 0, 2);
            s->bank.set_retrig_phase(static_cast<signal::AdditiveBank::RetrigPhase>(phase_step));
            s->bank.set_attack_ms(params.value_at(kAttackMs, offset));
            s->bank.set_release_ms(params.value_at(kReleaseMs, offset));
            s->bank.set_detune_cents(params.value_at(kDetuneCents, offset));

            // Gate edges, Schmitt-latched. `retrigger()` restarts every decay,
            // so firing it while the gate merely STAYS high would hold the
            // attack open forever and the bell would never ring out.
            const float cv = gate[static_cast<std::size_t>(k)];
            if (!s->gate_high && cv >= kGateOn) {
                s->gate_high = true;
                s->bank.retrigger();
            } else if (s->gate_high && cv <= kGateOff) {
                s->gate_high = false;
                s->bank.release();
            }

            output[static_cast<std::size_t>(k)] = s->bank.next();
        }
    };
    return t;
}

ForgeNodeDescriptor descriptor() {
    return {
        "additive_bank",
        "Additive Bank",
        "A sinusoidal partial bank for harmonic organ and inharmonic bell voices.",
        {{"voice",
          "Voice",
          "Selects the harmonic or modal voice construction.",
          {{"organ", "Organ", 0}, {"bell", "Bell", 1}}}},
        {{"organ", kOrganTypeId, {{"voice", "organ"}}}, {"bell", kBellTypeId, {{"voice", "bell"}}}},
        {{"fundamental_hz", kFundamentalHz, "Fundamental", "Hz",
          "Sets the bank's fundamental pitch.", ForgeParamKind::continuous,
          ForgeParamCurve::logarithmic},
         {"partial_count", kPartialCount, "Partials", "", "Sets the active oscillator count.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"inharmonicity",
          kInharmonicity,
          "Inharmonicity",
          "",
          "Stretches organ partials away from the harmonic series.",
          ForgeParamKind::continuous,
          ForgeParamCurve::linear,
          {},
          {"organ"}},
         {"spectral_tilt", kSpectralTilt, "Spectral Tilt", "dB/oct",
          "Tilts level across the partial spectrum.", ForgeParamKind::continuous,
          ForgeParamCurve::linear},
         {"master_gain_db", kMasterGainDb, "Master Gain", "dB", "Sets bank output level.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"envelope_mode",
          kEnvelopeMode,
          "Envelope Mode",
          "",
          "Selects shared or per-partial envelopes.",
          ForgeParamKind::stepped,
          ForgeParamCurve::linear,
          {{"shared", "Shared", 0}, {"per_partial", "Per Partial", 1}}},
         {"retrigger_phase",
          kRetrigPhase,
          "Retrigger Phase",
          "",
          "Selects phase behavior when the voice retriggers.",
          ForgeParamKind::stepped,
          ForgeParamCurve::linear,
          {{"free", "Free Running", 0}, {"reset", "Reset", 1}, {"alternating", "Alternating", 2}}},
         {"attack_ms", kAttackMs, "Attack", "ms", "Sets voice attack time.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"release_ms", kReleaseMs, "Release", "ms", "Sets voice release time.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"detune_cents", kDetuneCents, "Detune", "cent", "Spreads partial tuning.",
          ForgeParamKind::continuous, ForgeParamCurve::linear}}};
}

} // namespace additive

namespace vocoder {

float vocoder_worst_case_gain(double sample_rate) {
    constexpr double kPi = 3.14159265358979323846;
    const double highpass_l1 = 2.0 / (1.0 + std::tan(kPi * Voc::kSibilanceCornerHz / sample_rate));
    const double bank = Voc::kOutputHeadroomTrim * Voc::kWorstCaseGain;
    const double trim = std::pow(10.0, Voc::kOutputTrimMaxDb / 20.0);
    return static_cast<float>((bank + highpass_l1) * trim);
}

float vocoder_all_sample_rates_worst_case_gain() {
    const double bank = Voc::kOutputHeadroomTrim * Voc::kWorstCaseGain;
    const double trim = std::pow(10.0, Voc::kOutputTrimMaxDb / 20.0);
    return static_cast<float>((bank + 2.0) * trim);
}

CustomNodeType make_vocoder_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2; // 0 = modulator (the voice), 1 = carrier (the thing that speaks)
    t.num_output_ports = 1;
    t.default_name = "Vocoder";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->vocoder.prepare(sr);
        // Force the change-guarded setters to apply on the first block: a
        // guard that starts equal to the incoming value would skip the
        // configuration the graph asked for.
        s->band_count = -1.0f;
        s->freq_lo = -1.0f;
        s->freq_hi = -1.0f;
        s->attack_ms = -1.0f;
        s->release_ms = -1.0f;
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->vocoder.reset(); };

    t.baked_params.push_back({kBandCount, static_cast<float>(Voc::kMinBands),
                              static_cast<float>(Voc::kMaxBands), 16.0f});
    t.baked_params.push_back({kFreqLoHz, static_cast<float>(Voc::kFreqLoMinHz),
                              static_cast<float>(Voc::kFreqLoMaxHz), 120.0f});
    t.baked_params.push_back({kFreqHiHz, static_cast<float>(Voc::kFreqHiMinHz),
                              static_cast<float>(Voc::kFreqHiMaxHz), 7000.0f});
    t.baked_params.push_back({kCarrierSource, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kInternalWave, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kInternalPw, static_cast<float>(Voc::kPulseWidthMin),
                              static_cast<float>(Voc::kPulseWidthMax), 0.5f});
    t.baked_params.push_back({kCarrierPitchHz, static_cast<float>(Voc::kMinPitchHz),
                              static_cast<float>(Voc::kMaxPitchHz),
                              static_cast<float>(Voc::kDefaultPitchHz)});
    t.baked_params.push_back({kNoiseMix, 0.0f, 1.0f, 0.15f});
    t.baked_params.push_back({kAttackMs, static_cast<float>(Voc::kAttackMinMs),
                              static_cast<float>(Voc::kAttackMaxMs), 1.5f});
    t.baked_params.push_back({kReleaseMs, static_cast<float>(Voc::kReleaseMinMs),
                              static_cast<float>(Voc::kReleaseMaxMs), 15.0f});
    t.baked_params.push_back({kUnvoicedSens, 0.0f, 1.0f, 0.5f});
    t.baked_params.push_back({kSibilanceMix, 0.0f, 1.0f, 0.35f});
    t.baked_params.push_back({kFormantShiftSt, static_cast<float>(Voc::kFormantShiftMinSt),
                              static_cast<float>(Voc::kFormantShiftMaxSt), 0.0f});
    t.baked_params.push_back({kFormantFreeze, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kOutputTrimDb, static_cast<float>(Voc::kOutputTrimMinDb),
                              static_cast<float>(Voc::kOutputTrimMaxDb), 0.0f});
    t.baked_params.push_back({kDryWet, 0.0f, 1.0f, 1.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* modulator = in.channel_ptr(kModulatorPort);
        const float* carrier = in.channel_ptr(kCarrierPort);
        float* output = out.channel_ptr(0);

        // ── Block rate, and only on change ─────────────────────────────────
        // Each of these loops every band computing transcendentals. Sampled at
        // the block's first sample, following the dry/wet mixer's precedent for
        // a setter that is not sample-cheap.
        const float bands = params.value_at(kBandCount, 0);
        if (bands != s->band_count) {
            s->band_count = bands;
            s->vocoder.set_band_count(static_cast<int>(std::lround(bands)));
        }
        const float lo = params.value_at(kFreqLoHz, 0);
        const float hi = params.value_at(kFreqHiHz, 0);
        if (lo != s->freq_lo || hi != s->freq_hi) {
            s->freq_lo = lo;
            s->freq_hi = hi;
            s->vocoder.set_band_range_hz(lo, hi);
        }
        const float attack = params.value_at(kAttackMs, 0);
        if (attack != s->attack_ms) {
            s->attack_ms = attack;
            s->vocoder.set_attack_ms(attack);
        }
        const float release = params.value_at(kReleaseMs, 0);
        if (release != s->release_ms) {
            s->release_ms = release;
            s->vocoder.set_release_ms(release);
        }

        // ── Per sample ─────────────────────────────────────────────────────
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->vocoder.set_carrier_source(params.value_at(kCarrierSource, offset) >= 0.5f
                                              ? Voc::CarrierSource::internal
                                              : Voc::CarrierSource::external);
            s->vocoder.set_internal_wave(params.value_at(kInternalWave, offset) >= 0.5f
                                             ? Voc::InternalWave::pulse
                                             : Voc::InternalWave::saw);
            s->vocoder.set_internal_pulse_width(params.value_at(kInternalPw, offset));
            s->vocoder.set_internal_pitch_hz(params.value_at(kCarrierPitchHz, offset));
            s->vocoder.set_noise_mix(params.value_at(kNoiseMix, offset));
            s->vocoder.set_unvoiced_sensitivity(params.value_at(kUnvoicedSens, offset));
            s->vocoder.set_sibilance_mix(params.value_at(kSibilanceMix, offset));
            s->vocoder.set_formant_shift_semitones(params.value_at(kFormantShiftSt, offset));
            s->vocoder.set_formant_freeze(params.value_at(kFormantFreeze, offset) >= 0.5f);
            s->vocoder.set_output_trim_db(params.value_at(kOutputTrimDb, offset));
            s->vocoder.set_dry_wet(params.value_at(kDryWet, offset));

            float y = 0.0f;
            s->vocoder.process(modulator[static_cast<std::size_t>(k)],
                               carrier[static_cast<std::size_t>(k)], y);
            output[static_cast<std::size_t>(k)] = y;
        }
    };
    return t;
}

ForgeNodeDescriptor descriptor() {
    return {
        "vocoder",
        "Vocoder",
        "A multiband vocoder with external or internal carrier and formant controls.",
        {},
        {{"default", kTypeId}},
        {{"band_count", kBandCount, "Bands", "", "Sets the analysis and synthesis band count.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"freq_lo_hz", kFreqLoHz, "Low Frequency", "Hz", "Sets the lowest vocoder band.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"freq_hi_hz", kFreqHiHz, "High Frequency", "Hz", "Sets the highest vocoder band.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"carrier_source",
          kCarrierSource,
          "Carrier Source",
          "",
          "Selects external input or the internal oscillator.",
          ForgeParamKind::stepped,
          ForgeParamCurve::linear,
          {{"external", "External", 0}, {"internal", "Internal", 1}}},
         {"internal_wave",
          kInternalWave,
          "Internal Wave",
          "",
          "Selects the internal carrier waveform.",
          ForgeParamKind::stepped,
          ForgeParamCurve::linear,
          {{"saw", "Saw", 0}, {"pulse", "Pulse", 1}}},
         {"internal_pulse_width", kInternalPw, "Pulse Width", "%",
          "Sets internal pulse-carrier duty.", ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"carrier_pitch_hz", kCarrierPitchHz, "Carrier Pitch", "Hz",
          "Sets internal carrier pitch.", ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"noise_mix", kNoiseMix, "Noise", "%", "Adds noise to the carrier.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"attack_ms", kAttackMs, "Attack", "ms", "Sets band-envelope attack.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"release_ms", kReleaseMs, "Release", "ms", "Sets band-envelope release.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"unvoiced_sensitivity", kUnvoicedSens, "Unvoiced Sensitivity", "",
          "Detects unvoiced consonants.", ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"sibilance_mix", kSibilanceMix, "Sibilance", "%",
          "Restores high-frequency consonant energy.", ForgeParamKind::continuous,
          ForgeParamCurve::linear},
         {"formant_shift_st", kFormantShiftSt, "Formant Shift", "st",
          "Moves the analysis formants.", ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"formant_freeze",
          kFormantFreeze,
          "Formant Freeze",
          "",
          "Holds the current analysis envelopes.",
          ForgeParamKind::stepped,
          ForgeParamCurve::linear,
          {{"off", "Off", 0}, {"on", "On", 1}}},
         {"output_trim_db", kOutputTrimDb, "Output Trim", "dB", "Trims vocoder output.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"dry_wet", kDryWet, "Mix", "%", "Blends dry and vocoded signals.",
          ForgeParamKind::continuous, ForgeParamCurve::linear}}};
}

} // namespace vocoder

namespace cyclic {

float cyclic_stretch_worst_case_gain() {
    return static_cast<float>(Engine::kWorstCaseGain *
                              signal::units::db_to_linear(Engine::kOutputDbMax));
}

CustomNodeType make_cyclic_stretch_node(Regime regime) {
    const auto settings = regime == Regime::short_frame ? signal::kCyclicStretchShortFrame
                                                        : signal::kCyclicStretchLongFrame;
    CustomNodeType t;
    t.type_id = regime == Regime::short_frame ? kShortTypeId : kLongTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name =
        regime == Regime::short_frame ? "Cyclic Stretch (Short)" : "Cyclic Stretch (Long)";
    t.lowerable = true;
    t.latency_samples = [settings](double sample_rate) {
        Engine probe;
        probe.prepare(sample_rate);
        probe.set_regime(settings);
        return probe.latency_samples();
    };
    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [settings](void* p, double sr, int) {
        auto& e = static_cast<Instance*>(p)->engine;
        e.prepare(sr);
        e.set_regime(settings);
        e.reset();
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };
    t.baked_params.push_back({kStretch, static_cast<float>(Engine::kStretchRatioMin),
                              static_cast<float>(Engine::kStretchRatioMax),
                              static_cast<float>(Engine::kStretchRatioDefault)});
    t.baked_params.push_back({kCaptureMs, static_cast<float>(Engine::kCaptureMsMin),
                              static_cast<float>(Engine::kCaptureMsMax),
                              static_cast<float>(Engine::kCaptureMsDefault)});
    t.baked_params.push_back(
        {kCrossfadeShape, 0.0f, 1.0f, static_cast<float>(settings.crossfade_shape)});
    t.baked_params.push_back({kMixPct, 0.0f, 100.0f, static_cast<float>(Engine::kMixDefault)});
    t.baked_params.push_back({kOutputDb, static_cast<float>(Engine::kOutputDbMin),
                              static_cast<float>(Engine::kOutputDbMax),
                              static_cast<float>(Engine::kOutputDbDefault)});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto& e = static_cast<Instance*>(p)->engine;
        e.set_stretch_ratio(params.value_at(kStretch, 0));
        e.set_capture_ms(params.value_at(kCaptureMs, 0));
        e.set_crossfade_shape(params.value_at(kCrossfadeShape, 0));
        e.set_mix(params.value_at(kMixPct, 0));
        e.set_output_db(params.value_at(kOutputDb, 0));
        e.process(in.channel_ptr(0), out.channel_ptr(0), n);
    };
    return t;
}

ForgeNodeDescriptor descriptor() {
    return {"cyclic_stretch",
            "Cyclic Stretch",
            "A cyclic capture-and-repeat stretcher with short and long latency regimes.",
            {{"regime",
              "Regime",
              "Selects the fixed capture and latency regime.",
              {{"short", "Short", 0}, {"long", "Long", 1}}}},
            {{"short", kShortTypeId, {{"regime", "short"}}},
             {"long", kLongTypeId, {{"regime", "long"}}}},
            {{"stretch", kStretch, "Stretch", "x", "Sets the cyclic time-stretch ratio.",
              ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
             {"capture_ms", kCaptureMs, "Capture", "ms", "Sets captured slice duration.",
              ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
             {"crossfade_shape", kCrossfadeShape, "Crossfade Shape", "",
              "Shapes overlap between cyclic slices.", ForgeParamKind::continuous,
              ForgeParamCurve::linear},
             {"mix_pct", kMixPct, "Mix", "%", "Blends dry and stretched signals.",
              ForgeParamKind::continuous, ForgeParamCurve::linear},
             {"output_db", kOutputDb, "Output", "dB", "Trims output level.",
              ForgeParamKind::continuous, ForgeParamCurve::linear}}};
}

} // namespace cyclic

namespace granular {

float granular_worst_case_gain() {
    // At most kMaxGrainBudget grains overlap. Each grain, its window, and its
    // pan gain are individually bounded by one; the shipped four-point cubic
    // interpolator has a peak kernel L1 norm of 1.25.
    return static_cast<float>(Engine::kMaxGrainBudget) * 1.25f *
           static_cast<float>(signal::units::db_to_linear(Engine::kMaxLevelDb));
}

CustomNodeType make_granular_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 2;
    t.default_name = "Granular Live";
    t.lowerable = true;
    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sr, int) {
        auto& e = static_cast<Instance*>(p)->engine;
        e.prepare(sr);
        e.set_source(signal::GrainSource::live_ring);
        e.reset();
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };
    t.baked_params.push_back({kDensityHz, static_cast<float>(Engine::kMinDensityHz),
                              static_cast<float>(Engine::kMaxDensityHz), 20.0f});
    t.baked_params.push_back({kGrainMs, static_cast<float>(Engine::kMinGrainMs),
                              static_cast<float>(Engine::kMaxGrainMs), 80.0f});
    t.baked_params.push_back({kPosition, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back(
        {kPositionSprayMs, 0.0f, static_cast<float>(Engine::kMaxPositionSprayMs), 0.0f});
    t.baked_params.push_back({kPitchSt, static_cast<float>(-Engine::kMaxPitchSemitones),
                              static_cast<float>(Engine::kMaxPitchSemitones), 0.0f});
    t.baked_params.push_back(
        {kPitchSpraySt, 0.0f, static_cast<float>(Engine::kMaxPitchSpraySemitones), 0.0f});
    t.baked_params.push_back({kPanSpray, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kJitter, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kLevelDb, static_cast<float>(Engine::kMinLevelDb),
                              static_cast<float>(Engine::kMaxLevelDb), 0.0f});
    t.baked_params.push_back({kMix, 0.0f, 1.0f, 1.0f});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto& e = static_cast<Instance*>(p)->engine;
        e.set_density_hz(params.value_at(kDensityHz, 0));
        e.set_grain_ms(params.value_at(kGrainMs, 0));
        e.set_position(params.value_at(kPosition, 0));
        e.set_position_spray_ms(params.value_at(kPositionSprayMs, 0));
        e.set_pitch_semitones(params.value_at(kPitchSt, 0));
        e.set_pitch_spray_semitones(params.value_at(kPitchSpraySt, 0));
        e.set_pan_spray(params.value_at(kPanSpray, 0));
        e.set_async_jitter(params.value_at(kJitter, 0));
        e.set_level_db(params.value_at(kLevelDb, 0));
        e.set_mix(params.value_at(kMix, 0));
        e.process(in.channel_ptr(0), out.channel_ptr(0), out.channel_ptr(1), n);
    };
    return t;
}

ForgeNodeDescriptor descriptor() {
    return {
        "granular_live",
        "Granular Live",
        "A live-input granular processor with density, position, pitch, pan, and timing spray.",
        {},
        {{"default", kTypeId}},
        {{"density_hz", kDensityHz, "Density", "Hz", "Sets grain launch rate.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"grain_ms", kGrainMs, "Grain Size", "ms", "Sets grain duration.",
          ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
         {"position", kPosition, "Position", "%", "Sets read position in the live history.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"position_spray_ms", kPositionSprayMs, "Position Spray", "ms",
          "Randomizes grain read position.", ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"pitch_st", kPitchSt, "Pitch", "st", "Transposes grains.", ForgeParamKind::continuous,
          ForgeParamCurve::linear},
         {"pitch_spray_st", kPitchSpraySt, "Pitch Spray", "st", "Randomizes grain pitch.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"pan_spray", kPanSpray, "Pan Spray", "%", "Randomizes stereo placement.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"jitter", kJitter, "Jitter", "%", "Randomizes grain launch timing.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"level_db", kLevelDb, "Level", "dB", "Sets grain output level.",
          ForgeParamKind::continuous, ForgeParamCurve::linear},
         {"mix", kMix, "Mix", "%", "Blends dry and granular signals.", ForgeParamKind::continuous,
          ForgeParamCurve::linear}}};
}

} // namespace granular

} // namespace pulp::host::synthesis
