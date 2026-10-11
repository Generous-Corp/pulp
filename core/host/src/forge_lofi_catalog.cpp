// Forge Lo-fi catalog factory implementations.

#include <pulp/host/forge_lofi_catalog.hpp>

namespace pulp::host::forge_lofi {

CustomNodeType make_delay_node() {
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
        s->max_delay_samples = std::max(1, static_cast<int>(std::ceil(kDelayMaxMs * 0.001 * sr)));
        s->line.prepare(s->max_delay_samples);
    };
    t.reset = [](void* p) { static_cast<DelayInstance*>(p)->line.reset(); };
    // time_ms: 1 ms .. 2 s, default 250 ms. feedback: 0 .. 0.95, default 0.5.
    t.baked_params.push_back({kDelayTimeMs, 1.0f, kDelayMaxMs, 250.0f});
    t.baked_params.push_back({kDelayFeedback, 0.0f, 0.95f, 0.5f});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
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
            s->line.push(dry + fb * wet);               // recirculate
            o[static_cast<std::size_t>(k)] = dry + wet; // dry + echo
        }
    };
    return t;
}

CustomNodeType make_filter_node(signal::Svf::Mode mode) {
    CustomNodeType t;
    switch (mode) {
    case signal::Svf::Mode::highpass:
        t.type_id = kFilterHighpassTypeId;
        break;
    case signal::Svf::Mode::bandpass:
        t.type_id = kFilterBandpassTypeId;
        break;
    case signal::Svf::Mode::notch:
        t.type_id = kFilterNotchTypeId;
        break;
    case signal::Svf::Mode::lowpass:
    default:
        t.type_id = kFilterTypeId;
        break;
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
        s->resonance = 0.707f; // Butterworth-ish, no peak
        s->svf.set_resonance(s->resonance);
        s->svf.set_mode(mode);
    };
    t.reset = [](void* p) { static_cast<FilterInstance*>(p)->svf.reset(); };
    // cutoff_hz: 20 Hz .. 20 kHz, default fully open (transparent until turned).
    t.baked_params.push_back({kFilterCutoffHz, 20.0f, 20000.0f, 20000.0f});
    // resonance (Q): 0.5 (gently damped) .. 12 (a singing peak), default 0.707
    // — maximally flat, so an un-bound Resonance is the old fixed behavior.
    t.baked_params.push_back({kFilterResonance, 0.5f, 12.0f, 0.707f});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
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
            s->svf.set_frequency(cutoff); // sample-accurate retune (a tan()/sample)
            o[static_cast<std::size_t>(k)] = s->svf.process(i[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

CustomNodeType make_waveshaper_node() {
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
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<WaveshaperInstance*>(p);
        const float* i = in.channel_ptr(0);
        float* o = out.channel_ptr(0);
        for (int k = 0; k < n; ++k) {
            const float drive = params.value_at(kWaveshaperDrive, static_cast<std::int32_t>(k));
            s->ws.set_drive(drive);
            o[static_cast<std::size_t>(k)] = s->ws.process(i[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

CustomNodeType make_drywet_node() {
    CustomNodeType t;
    t.type_id = kDryWetTypeId;
    t.version = 1;
    t.num_input_ports = 2; // 0 = dry, 1 = wet
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
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<DryWetInstance*>(p);
        const float* dry = in.channel_ptr(0);
        const float* wet = in.channel_ptr(1);
        float* o = out.channel_ptr(0);
        // Block-rate: sample the injected mix at the block's first sample.
        s->dwm.set_mix(params.value_at(kDryWetMix, 0));
        for (int k = 0; k < n; ++k)
            o[static_cast<std::size_t>(k)] = wet[static_cast<std::size_t>(k)];
        s->dwm.push_dry(&dry, 1, n); // stores the dry channel (latency 0 → memcpy)
        s->dwm.mix_wet(&o, 1, n);    // blends dry·(1-mix) + wet·mix in place
    };
    return t;
}

CustomNodeType make_noise_node() {
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
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<NoiseInstance*>(p);
        const float* i = in.channel_ptr(0);
        float* o = out.channel_ptr(0);
        for (int k = 0; k < n; ++k) {
            const float level = params.value_at(kNoiseLevel, static_cast<std::int32_t>(k));
            o[static_cast<std::size_t>(k)] =
                i[static_cast<std::size_t>(k)] + level * forge_white_next(s->rng);
        }
    };
    return t;
}

CustomNodeType make_bitcrush_node(signal::DitherMode dither, signal::NoiseShapingOrder shaping) {
    if (shaping != signal::NoiseShapingOrder::none)
        dither = signal::DitherMode::tpdf;
    CustomNodeType t;
    if (dither == signal::DitherMode::none && shaping == signal::NoiseShapingOrder::none)
        t.type_id = kBitcrushTypeId;
    else if (shaping == signal::NoiseShapingOrder::first)
        t.type_id = kBitcrushTpdfFirstTypeId;
    else if (shaping == signal::NoiseShapingOrder::second)
        t.type_id = kBitcrushTpdfSecondTypeId;
    else
        t.type_id = kBitcrushTpdfTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Crush";
    t.lowerable = true;
    t.create = [dither, shaping]() -> void* {
        auto* s = new BitcrushInstance{};
        s->quantizer.set_dither_mode(dither);
        s->quantizer.set_noise_shaping(shaping);
        return s;
    };
    t.destroy = [](void* p) { delete static_cast<BitcrushInstance*>(p); };
    t.reset = [](void* p) {
        auto* s = static_cast<BitcrushInstance*>(p);
        s->held = 0.0f;
        s->phase = 0.0f;
        s->quantizer.reset();
    };
    // bit_depth: 16 (near-lossless) down to 1 (3-level) — default lossless.
    t.baked_params.push_back({kBitcrushBitDepth, 1.0f, 16.0f, 16.0f});
    // sample_rate_reduction: 1 (full rate) .. 64 — default full rate.
    t.baked_params.push_back({kBitcrushRateDiv, 1.0f, 64.0f, 1.0f});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<BitcrushInstance*>(p);
        const float* i = in.channel_ptr(0);
        float* o = out.channel_ptr(0);
        for (int k = 0; k < n; ++k) {
            const auto off = static_cast<std::int32_t>(k);
            const float bits = params.value_at(kBitcrushBitDepth, off);
            float reduction = params.value_at(kBitcrushRateDiv, off);
            if (reduction < 1.0f)
                reduction = 1.0f;

            // Sample-and-hold decimation: latch a fresh input every
            // `reduction` samples; repeat the held value in between.
            s->phase += 1.0f;
            if (s->phase >= reduction) {
                s->phase -= reduction;
                s->held = i[static_cast<std::size_t>(k)];
            }

            if (s->quantizer.dither_mode() == signal::DitherMode::none &&
                s->quantizer.noise_shaping() == signal::NoiseShapingOrder::none) {
                // Preserve the original realization's arithmetic exactly.
                const float scale = std::exp2(bits - 1.0f); // 2^(bits-1)
                o[static_cast<std::size_t>(k)] = std::round(s->held * scale) / scale;
            } else {
                s->quantizer.set_bits(bits);
                o[static_cast<std::size_t>(k)] = s->quantizer.process(s->held);
            }
        }
    };
    return t;
}

CustomNodeType make_trim_node() {
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
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<TrimInstance*>(p);
        const float* i = in.channel_ptr(0);
        float* o = out.channel_ptr(0);
        for (int k = 0; k < n; ++k) {
            const float db = params.value_at(kTrimGainDb, static_cast<std::int32_t>(k));
            if (db != s->last_db) {
                s->last_db = db;
                s->gain = std::exp2(db * (1.0f / 6.020599913279624f));
            }
            o[static_cast<std::size_t>(k)] = i[static_cast<std::size_t>(k)] * s->gain;
        }
    };
    return t;
}

CustomNodeType make_ping_pong_node() {
    CustomNodeType t;
    t.type_id = kPingPongTypeId;
    t.version = 1;
    t.num_input_ports = 2; // 0 = left, 1 = right (ONE logical stereo wire)
    t.num_output_ports = 2;
    t.default_name = "Ping Pong";
    t.lowerable = true;
    t.create = []() -> void* { return new PingPongInstance{}; };
    t.destroy = [](void* p) { delete static_cast<PingPongInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<PingPongInstance*>(p);
        s->sample_rate = sr;
        s->max_delay_samples = std::max(1, static_cast<int>(std::ceil(kDelayMaxMs * 0.001 * sr)));
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
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
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
            const float fb = std::clamp(params.value_at(kPingPongFeedback, off), 0.0f, 0.95f);
            const float width = std::clamp(params.value_at(kPingPongWidth, off), 0.0f, 1.0f);

            float delay_samples = time_ms * sr_per_ms;
            delay_samples = std::clamp(delay_samples, 1.0f, max_d);

            const float dry_l = il[static_cast<std::size_t>(k)];
            const float dry_r = ir[static_cast<std::size_t>(k)];
            const float wet_l = s->line_l.read(delay_samples);
            const float wet_r = s->line_r.read(delay_samples);

            s->line_l.push(dry_l + fb * wet_r); // right tap feeds the left line
            s->line_r.push(dry_r + fb * wet_l); // and vice versa — the bounce

            const float same = 0.5f + 0.5f * width;
            const float cross = 0.5f - 0.5f * width;
            ol[static_cast<std::size_t>(k)] = dry_l + same * wet_l + cross * wet_r;
            or_[static_cast<std::size_t>(k)] = dry_r + same * wet_r + cross * wet_l;
        }
    };
    return t;
}

CustomNodeType make_reverb_node() {
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
        s->last_decay = -1.0f; // force a setter write on the first sample
        s->last_damping = -1.0f;
    };
    t.reset = [](void* p) { static_cast<ReverbInstance*>(p)->reverb.reset(); };
    // decay: 0.1 .. 10 s (RT60), default 2 s. damping: 0 .. 0.99, default 0.3.
    // mix: 0 .. 1, default 0.3 — a subtle wet blend an untouched reverb sits at.
    t.baked_params.push_back({kReverbDecay, 0.1f, 10.0f, 2.0f});
    t.baked_params.push_back({kReverbDamping, 0.0f, 0.99f, 0.3f});
    t.baked_params.push_back({kReverbMix, 0.0f, 1.0f, 0.3f});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
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
            const float mix = std::clamp(params.value_at(kReverbMix, off), 0.0f, 1.0f);
            const float dry = i[static_cast<std::size_t>(k)];
            const auto st = s->reverb.process(i[static_cast<std::size_t>(k)]);
            const float wet = 0.5f * (st.left + st.right) * kReverbWetNormalization;
            o[static_cast<std::size_t>(k)] = dry * (1.0f - mix) + wet * mix;
        }
    };
    return t;
}

CustomNodeType make_compressor_node() {
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
        static_cast<CompressorInstance*>(p)->comp.set_sample_rate(static_cast<float>(sr));
    };
    t.reset = [](void* p) { static_cast<CompressorInstance*>(p)->comp.reset(); };
    // threshold_db: -60 .. 0, default -18. ratio: 1 (transparent) .. 20, default 4.
    // attack_ms: 0.1 .. 100, default 10. release_ms: 10 .. 1000, default 150.
    t.baked_params.push_back({kCompThresholdDb, -60.0f, 0.0f, -18.0f});
    t.baked_params.push_back({kCompRatio, 1.0f, 20.0f, 4.0f});
    t.baked_params.push_back({kCompAttackMs, 0.1f, 100.0f, 10.0f});
    t.baked_params.push_back({kCompReleaseMs, 10.0f, 1000.0f, 150.0f});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
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
        cp.knee_db = 6.0f;   // musical soft knee
        cp.makeup_db = 0.0f; // reduce-only: never restores level
        s->comp.set_params(cp);
        for (int k = 0; k < n; ++k)
            o[static_cast<std::size_t>(k)] = s->comp.process(i[static_cast<std::size_t>(k)]);
    };
    return t;
}

CustomNodeType make_gate_node() {
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
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
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
            attack_ms > 0.0f ? 1.0f - std::exp(-1.0f / (attack_ms * 0.001f * sr)) : 1.0f;
        const float release_coeff =
            release_ms > 0.0f ? 1.0f - std::exp(-1.0f / (release_ms * 0.001f * sr)) : 1.0f;

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
                target_db =
                    std::max(-below * (GateInstance::kRatio - 1.0f), GateInstance::kRangeDb);
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

CustomNodeType make_lfo_node() {
    CustomNodeType t;
    t.type_id = kLfoTypeId;
    t.version = 1;
    t.num_input_ports = 0;  // pure control source
    t.num_output_ports = 1; // CV out
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
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
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
            o[static_cast<std::size_t>(k)] = std::clamp(0.5f + 0.5f * depth * osc, 0.0f, 1.0f);
            s->phase += rate * inv_sr;
            if (s->phase >= 1.0f)
                s->phase -= std::floor(s->phase);
        }
    };
    return t;
}

CustomNodeType make_vca_node() {
    CustomNodeType t;
    t.type_id = kVcaTypeId;
    t.version = 1;
    t.num_input_ports = 2; // 0 = signal, 1 = gain CV
    t.num_output_ports = 1;
    t.default_name = "VCA";
    t.lowerable = true;
    t.create = []() -> void* { return new int{0}; }; // trivial keepalive instance
    t.destroy = [](void* p) { delete static_cast<int*>(p); };
    t.baked_params.push_back({kVcaGain, 0.0f, 1.0f, 1.0f});
    t.process_instance_baked_param = [](void* /*p*/, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        const float* sig = in.channel_ptr(0);
        const float* cv = in.channel_ptr(1);
        float* o = out.channel_ptr(0);
        for (int k = 0; k < n; ++k) {
            const float gain =
                std::clamp(params.value_at(kVcaGain, static_cast<std::int32_t>(k)), 0.0f, 1.0f);
            const float c = std::clamp(cv[static_cast<std::size_t>(k)], 0.0f, 1.0f);
            o[static_cast<std::size_t>(k)] = sig[static_cast<std::size_t>(k)] * gain * c;
        }
    };
    return t;
}

CustomNodeType make_env_follower_node() {
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
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<EnvFollowerInstance*>(p);
        const float* i = in.channel_ptr(0);
        float* o = out.channel_ptr(0);
        // Ballistics are block-rate: a setter recomputes an exp coefficient,
        // so retune only when the injected value actually moved.
        const float atk = params.value_at(kEnvAttackMs, 0);
        const float rel = params.value_at(kEnvReleaseMs, 0);
        if (atk != s->last_attack_ms) {
            s->env.set_attack_ms(atk);
            s->last_attack_ms = atk;
        }
        if (rel != s->last_release_ms) {
            s->env.set_release_ms(rel);
            s->last_release_ms = rel;
        }
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

CustomNodeType make_filter_cv_node() {
    CustomNodeType t;
    t.type_id = kFilterCvTypeId;
    t.version = 1;
    t.num_input_ports = 2; // 0 = signal, 1 = cutoff CV
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
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<FilterCvInstance*>(p);
        const float* sig = in.channel_ptr(0);
        const float* cv = in.channel_ptr(1);
        float* o = out.channel_ptr(0);
        const float nyq_guard = 0.45f * static_cast<float>(s->sample_rate);
        const float q = params.value_at(kFilterCvResonance, 0);
        if (q != s->last_resonance) {
            s->svf.set_resonance(q);
            s->last_resonance = q;
        }
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

CustomNodeType make_delay_cv_node() {
    CustomNodeType t;
    t.type_id = kDelayCvTypeId;
    t.version = 1;
    t.num_input_ports = 2; // 0 = signal, 1 = time CV
    t.num_output_ports = 1;
    t.default_name = "DelayCV";
    t.lowerable = true;
    t.create = []() -> void* { return new DelayCvInstance{}; };
    t.destroy = [](void* p) { delete static_cast<DelayCvInstance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<DelayCvInstance*>(p);
        s->sample_rate = sr;
        s->max_delay_samples = std::max(1, static_cast<int>(std::ceil(kDelayMaxMs * 0.001 * sr)));
        s->line.prepare(s->max_delay_samples);
    };
    t.reset = [](void* p) { static_cast<DelayCvInstance*>(p)->line.reset(); };
    // base 0.1..50 ms (chorus range) default 15; depth 0..25 ms default 5; feedback
    // 0..0.95 default 0 (chorus); mix 0..1 default 0.5.
    t.baked_params.push_back({kDelayCvBaseMs, 0.1f, 50.0f, 15.0f});
    t.baked_params.push_back({kDelayCvDepthMs, 0.0f, 25.0f, 5.0f});
    t.baked_params.push_back({kDelayCvFeedback, 0.0f, 0.95f, 0.0f});
    t.baked_params.push_back({kDelayCvMix, 0.0f, 1.0f, 0.5f});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
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

CustomNodeType make_auto_pan_node() {
    CustomNodeType t;
    t.type_id = kAutoPanTypeId;
    t.version = 1;
    t.num_input_ports = 2; // 0 = left, 1 = right (ONE logical stereo wire)
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
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
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
            const float depth = std::clamp(params.value_at(kAutoPanDepth, off), 0.0f, 1.0f);
            const int shape = static_cast<int>(
                std::lround(std::clamp(params.value_at(kAutoPanShape, off), 0.0f, 3.0f)));

            const float osc = forge_lfo_osc(s->phase, shape); // [-1, 1]
            const float pan = std::clamp(depth * osc, -1.0f, 1.0f);
            const float position = (pan + 1.0f) * 0.5f; // [0, 1]
            const float theta = position * kHalfPi;
            const float lg = std::cos(theta);
            const float rg = std::sin(theta);
            ol[static_cast<std::size_t>(k)] = il[static_cast<std::size_t>(k)] * lg;
            or_[static_cast<std::size_t>(k)] = ir[static_cast<std::size_t>(k)] * rg;

            s->phase += rate * inv_sr;
            if (s->phase >= 1.0f)
                s->phase -= std::floor(s->phase);
        }
    };
    return t;
}

CustomNodeType make_width_node() {
    CustomNodeType t;
    t.type_id = kWidthTypeId;
    t.version = 1;
    t.num_input_ports = 2; // 0 = left, 1 = right (ONE logical stereo wire)
    t.num_output_ports = 2;
    t.default_name = "Width";
    t.lowerable = true;
    t.create = []() -> void* { return new WidthInstance{}; };
    t.destroy = [](void* p) { delete static_cast<WidthInstance*>(p); };
    t.prepare = [](void* /*p*/, double /*sr*/, int /*max_block*/) {};
    t.reset = [](void* /*p*/) {};
    t.baked_params.push_back({kWidthAmount, 0.0f, 2.0f, 1.0f});
    t.process_instance_baked_param = [](void* /*p*/, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        const float* il = in.channel_ptr(0);
        const float* ir = in.channel_ptr(1);
        float* ol = out.channel_ptr(0);
        float* or_ = out.channel_ptr(1);
        for (int k = 0; k < n; ++k) {
            const auto off = static_cast<std::int32_t>(k);
            const float l = il[static_cast<std::size_t>(k)];
            const float r = ir[static_cast<std::size_t>(k)];
            signal::stereo_width(l, r, params.value_at(kWidthAmount, off),
                                 ol[static_cast<std::size_t>(k)], or_[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

CustomNodeType make_phaser_node() {
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
        s->phaser.set_mix(0.5f); // classic 50/50 → the notches reach full depth
    };
    t.reset = [](void* p) { static_cast<PhaserInstance*>(p)->phaser.reset(); };
    t.baked_params.push_back({kPhaserRateHz, 0.01f, 20.0f, 0.5f});
    t.baked_params.push_back({kPhaserDepth, 0.0f, 1.0f, 0.7f});
    t.baked_params.push_back({kPhaserFeedback, 0.0f, 0.9f, 0.5f});
    t.baked_params.push_back({kPhaserStages, 2.0f, 8.0f, 4.0f});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<PhaserInstance*>(p);
        const float* x = in.channel_ptr(0);
        float* o = out.channel_ptr(0);
        for (int k = 0; k < n; ++k) {
            const auto off = static_cast<std::int32_t>(k);
            s->phaser.set_rate(params.value_at(kPhaserRateHz, off));
            s->phaser.set_depth(std::clamp(params.value_at(kPhaserDepth, off), 0.0f, 1.0f));
            s->phaser.set_feedback(std::clamp(params.value_at(kPhaserFeedback, off), 0.0f, 0.9f));
            s->phaser.set_stages(static_cast<int>(
                std::lround(std::clamp(params.value_at(kPhaserStages, off), 2.0f, 8.0f))));
            o[static_cast<std::size_t>(k)] = s->phaser.process(x[static_cast<std::size_t>(k)]);
        }
    };
    return t;
}

} // namespace pulp::host::forge_lofi
