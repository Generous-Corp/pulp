#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/adsr.hpp>
#include <pulp/signal/bias.hpp>
#include <pulp/signal/convolver_non_uniform.hpp>
#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/dry_wet_mixer.hpp>
#include <pulp/signal/fast_math.hpp>
#include <pulp/signal/filter_design.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/fft_backend.hpp>
#include <pulp/signal/freeze_hold.hpp>
#include <pulp/signal/freeze_loop_sampler.hpp>
#include <pulp/signal/elliptic_halfband_iir.hpp>
#include <pulp/signal/halfband_iir.hpp>
#include <pulp/signal/interpolator.hpp>
#include <pulp/signal/latency_aware_control_smoother.hpp>
#include <pulp/signal/matrix.hpp>
#include <pulp/signal/multichannel_phase_coordinator.hpp>
#include <pulp/signal/noise_morpher.hpp>
#include <pulp/signal/poly_math.hpp>
#include <pulp/signal/pitched_feedback_delay.hpp>
#include <pulp/signal/processor_duplicator.hpp>
#include <pulp/signal/realtime_pitch_time_processor.hpp>
#include <pulp/signal/resampler.hpp>
#include <pulp/signal/simd_buffer.hpp>
#include <pulp/signal/signal.hpp>
#include <pulp/signal/sinc_resampler.hpp>
#include <pulp/signal/spectrogram.hpp>
#include <pulp/signal/spectral_envelope_shifter.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>
#include <pulp/signal/stft.hpp>
#include <pulp/signal/stn_decomposer.hpp>
#include <pulp/signal/special_functions.hpp>
#include <pulp/signal/transient_phase_policy.hpp>
#include <pulp/signal/oversampling.hpp>
#include <pulp/signal/wavetable.hpp>
#include <pulp/signal/windowing.hpp>

#include <array>
#include <complex>
#include <cstddef>
#include <vector>

using namespace pulp::signal;
using pulp::signal::Oversampler;

namespace {

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

struct RtProbeGain {
    float gain = 1.0f;
    float sample_rate = 0.0f;
    int reset_count = 0;

    void set_sample_rate(float sr) { sample_rate = sr; }
    float process(float input) { return input * gain; }
    void reset() { ++reset_count; }
};

class RtProbeLoopProcessor final : public FeedbackLoopProcessor {
public:
    int loop_latency_samples() const override { return 3; }
    bool loop_is_frozen() const override { return frozen; }

    void loop_process(const float* const* in, float* const* out, int num_samples) override {
        for (int ch = 0; ch < channels; ++ch) {
            for (int i = 0; i < num_samples; ++i)
                out[ch][i] = in[ch][i] * 0.5f;
        }
    }

    int channels = 1;
    bool frozen = false;
};

void require_process_allocates_no_memory(Oversampler::Kind kind,
                                         Oversampler::Factor factor) {
    Oversampler os;
    os.set_kind(kind);
    os.set_factor(factor);
    os.set_sample_rate(48000.0f);

    std::array<float, 16> inputs {};
    for (std::size_t i = 0; i < inputs.size(); ++i)
        inputs[i] = static_cast<float>(i + 1) * 0.01f;

    int callback_hits = 0;
    auto saturate = [&](float sample) {
        ++callback_hits;
        return sample / (1.0f + sample * sample);
    };

    pulp::test::RtAllocationProbe probe;
    for (float input : inputs) {
        const float output = os.process(input, saturate);
        (void)output;
    }

    REQUIRE(probe.allocation_count() == 0);
    REQUIRE(callback_hits == static_cast<int>(inputs.size()) * os.factor_value());
}

} // namespace

TEST_CASE("Oversampler process is allocation-free after configuration",
          "[signal][oversampling][rt-safety]") {
    require_process_allocates_no_memory(Oversampler::Kind::fir_biquad,
                                        Oversampler::Factor::x2);
    require_process_allocates_no_memory(Oversampler::Kind::fir_biquad,
                                        Oversampler::Factor::x4);
    require_process_allocates_no_memory(Oversampler::Kind::polyphase_iir,
                                        Oversampler::Factor::x2);
    require_process_allocates_no_memory(Oversampler::Kind::polyphase_iir,
                                        Oversampler::Factor::x4);
    require_process_allocates_no_memory(Oversampler::Kind::polyphase_iir,
                                        Oversampler::Factor::x8);
    require_process_allocates_no_memory(Oversampler::Kind::polyphase_iir,
                                        Oversampler::Factor::x16);
    require_process_allocates_no_memory(Oversampler::Kind::linear_phase_fir,
                                        Oversampler::Factor::x2);
    require_process_allocates_no_memory(Oversampler::Kind::linear_phase_fir,
                                        Oversampler::Factor::x4);
    require_process_allocates_no_memory(Oversampler::Kind::linear_phase_fir,
                                        Oversampler::Factor::x8);
    require_process_allocates_no_memory(Oversampler::Kind::linear_phase_fir,
                                        Oversampler::Factor::x16);
    require_process_allocates_no_memory(Oversampler::Kind::elliptic_polyphase_iir,
                                        Oversampler::Factor::x2);
    require_process_allocates_no_memory(Oversampler::Kind::elliptic_polyphase_iir,
                                        Oversampler::Factor::x4);
    require_process_allocates_no_memory(Oversampler::Kind::elliptic_polyphase_iir,
                                        Oversampler::Factor::x8);
    require_process_allocates_no_memory(Oversampler::Kind::elliptic_polyphase_iir,
                                        Oversampler::Factor::x16);
}

TEST_CASE("Oversampler process_block is allocation-free after configuration",
          "[signal][oversampling][rt-safety]") {
    Oversampler os;
    os.set_kind(Oversampler::Kind::polyphase_iir);
    os.set_factor(Oversampler::Factor::x4);
    os.set_sample_rate(48000.0f);

    std::array<float, 32> input {};
    std::array<float, 32> output {};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(i % 7) * 0.05f;

    int callback_hits = 0;
    auto waveshape = [&](float sample) {
        ++callback_hits;
        return sample - (0.25f * sample * sample * sample);
    };

    pulp::test::RtAllocationProbe probe;
    os.process_block(input.data(), output.data(), input.size(), waveshape);

    REQUIRE(probe.allocation_count() == 0);
    REQUIRE(callback_hits == static_cast<int>(input.size()) * os.factor_value());
}

TEST_CASE("Oversampler does not copy a std::function callback per sample",
          "[signal][oversampling][rt-safety]") {
    // The type-erased overload takes its callback by reference. Taking it by value
    // would copy the callable once per sample — and a payload too large for
    // std::function's small-object buffer heap-allocates on every copy, which turns
    // an otherwise RT-safe block into hundreds of allocations.
    Oversampler os;
    os.set_kind(Oversampler::Kind::linear_phase_fir);
    os.set_factor(Oversampler::Factor::x4);

    std::array<float, 64> payload {}; // far past any small-object buffer
    payload.fill(0.5f);
    std::function<float(float)> waveshape = [payload](float sample) {
        return sample * payload[0];
    };

    std::array<float, 32> input {};
    std::array<float, 32> output {};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(i % 7) * 0.05f;

    // A non-const std::function lvalue still binds the templated overload, so probe
    // the type-erased one through a const reference — otherwise its body, which is
    // the code that used to copy, never runs here at all.
    const std::function<float(float)>& erased = waveshape;

    // Settle first: the probe must see the steady state, not first-touch effects.
    os.process_block(input.data(), output.data(), input.size(), waveshape);

    pulp::test::RtAllocationProbe probe;
    os.process_block(input.data(), output.data(), input.size(), waveshape);
    os.process_block(input.data(), output.data(), input.size(), erased);
    for (std::size_t i = 0; i < input.size(); ++i)
        static_cast<void>(os.process(input[i], erased));

    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("Scalar signal helpers are allocation-free after configuration",
          "[signal][rt-safety]") {
    Bias bias;
    bias.set_bias(0.1f);

    DcBlocker<float> dc_blocker;
    dc_blocker.set_pole(0.995f);

    Gain gain;
    gain.set_gain_linear(0.5f);
    Gain64 gain64;
    gain64.set_gain_linear(0.5);

    SimpleMixer mixer;
    mixer.set_mix(0.25f);
    SimpleMixer64 mixer64;
    mixer64.set_mix(0.25);

    Oscillator osc;
    osc.set_sample_rate(48000.0f);
    osc.set_frequency(220.0f);
    osc.set_waveform(Oscillator::Waveform::saw);

    Biquad biquad;
    biquad.set_coefficients(Biquad::Type::lowpass, 1800.0f, 0.707f, 48000.0f);
    Biquad64 biquad64;
    biquad64.set_coefficients(Biquad64::Type::lowpass, 1800.0, 0.707, 48000.0);

    Svf svf;
    svf.set_sample_rate(48000.0f);
    svf.set_frequency(900.0f);
    svf.set_resonance(0.8f);
    svf.set_mode(Svf::Mode::bandpass);

    Phaser phaser;
    phaser.set_sample_rate(48000.0f);
    phaser.set_rate(0.4f);
    phaser.set_depth(0.6f);
    phaser.set_stages(4);

    WaveShaper shaper;
    shaper.set_curve(WaveShaper::Curve::soft_clip);
    shaper.set_drive(2.0f);

    Panner panner;
    panner.set_pan(0.25f);
    panner.set_law(PanLaw::Sin4_5dB);

    BallisticsFilter ballistics;
    ballistics.prepare(48000.0f);
    ballistics.set_attack_ms(2.0f);
    ballistics.set_release_ms(80.0f);

    SmoothedValue<float> smoother(0.0f);
    smoother.set_ramp_time(0.01f, 48000.0f);
    smoother.set_target(1.0f);

    LogRampedValue log_ramp(220.0f);
    log_ramp.set_ramp_time(0.02f, 48000.0f);
    log_ramp.set_target(880.0f);

    require_allocates_no_memory([&] {
        std::array<float, 64> samples {};
        std::array<float, 64> dry {};
        std::array<float, 64> wet {};
        std::array<float, 64> mixed {};
        std::array<double, 64> samples64 {};
        std::array<double, 64> dry64 {};
        std::array<double, 64> wet64 {};
        std::array<double, 64> mixed64 {};
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const float input = static_cast<float>(i % 13) * 0.03f - 0.18f;
            float value = bias.process(input);
            value = dc_blocker.process(value);
            value = gain.process(value);
            value = mixer.process(value, osc.next());
            value = biquad.process(value);
            value = svf.process(value);
            value = phaser.process(value);
            value = shaper.process(value);
            const auto stereo = panner.process(value);
            float left = stereo.left;
            float right = stereo.right;
            panner.process(left, right);
            const float smoothed = smoother.next();
            const float log_smoothed = log_ramp.next();
            osc.set_frequency(log_smoothed);
            samples[i] = ballistics.process((left + right) * (0.5f + smoothed * 0.1f));
            dry[i] = input;
            wet[i] = samples[i];
            const double input64 = static_cast<double>(input);
            samples64[i] = biquad64.process(gain64.process(input64));
            dry64[i] = input64;
            wet64[i] = samples64[i];
        }

        bias.process(samples.data(), static_cast<int>(samples.size()));
        bias.process(samples.data(), mixed.data(), static_cast<int>(samples.size()));
        dc_blocker.process(samples.data(), static_cast<int>(samples.size()));
        gain.process(samples.data(), static_cast<int>(samples.size()));
        mixer.process(dry.data(), wet.data(), mixed.data(), static_cast<int>(mixed.size()));
        gain64.process(samples64.data(), static_cast<int>(samples64.size()));
        mixer64.process(dry64.data(), wet64.data(), mixed64.data(),
                        static_cast<int>(mixed64.size()));
        biquad64.process(samples64.data(), static_cast<int>(samples64.size()));
        svf.process(samples.data(), static_cast<int>(samples.size()));
        phaser.process(samples.data(), static_cast<int>(samples.size()));
        shaper.process(samples.data(), static_cast<int>(samples.size()));
        smoother.skip(16);
        log_ramp.skip(16);
        dc_blocker.reset();
        bias.reset();
        bias.set_sample_rate(48000.0f);
        ballistics.reset();
    });
}

TEST_CASE("Prepared DryWetMixer push and mix are allocation-free within capacity",
          "[signal][rt-safety]") {
    DryWetMixer mixer;
    mixer.prepare(2, 64);
    mixer.set_mix(0.35f);
    mixer.set_curve(MixCurve::EqualPower);

    DryWetMixer latency_mixer;
    latency_mixer.set_wet_latency(3);
    latency_mixer.prepare(2, 64);
    latency_mixer.set_mix(0.25f);
    latency_mixer.set_curve(MixCurve::Linear);

    std::array<float, 64> dry_l {};
    std::array<float, 64> dry_r {};
    std::array<float, 64> wet_l {};
    std::array<float, 64> wet_r {};
    std::array<float, 64> latency_wet_l {};
    std::array<float, 64> latency_wet_r {};
    for (std::size_t i = 0; i < dry_l.size(); ++i) {
        dry_l[i] = static_cast<float>(i + 1) * 0.01f;
        dry_r[i] = static_cast<float>(i + 1) * 0.02f;
        wet_l[i] = 0.5f;
        wet_r[i] = -0.25f;
        latency_wet_l[i] = 0.2f;
        latency_wet_r[i] = -0.1f;
    }

    const float* dry_channels[] = {dry_l.data(), dry_r.data()};
    float* wet_channels[] = {wet_l.data(), wet_r.data()};
    float* latency_wet_channels[] = {latency_wet_l.data(), latency_wet_r.data()};

    require_allocates_no_memory([&] {
        mixer.push_dry(dry_channels, 2, 64);
        mixer.mix_wet(wet_channels, 2, 64);
        mixer.set_mix(0.5f);
        mixer.set_curve(MixCurve::Sqrt4_5dB);
        mixer.push_dry(dry_channels, 1, 16);
        mixer.mix_wet(wet_channels, 2, 64);
        mixer.reset();

        latency_mixer.push_dry(dry_channels, 2, 64);
        latency_mixer.mix_wet(latency_wet_channels, 2, 64);
        latency_mixer.reset();
        latency_mixer.push_dry(nullptr, 0, 0);
        latency_mixer.mix_wet(latency_wet_channels, 2, 64);
    });
}

TEST_CASE("Envelope smoothing and resampler hot paths are allocation-free after setup",
          "[signal][rt-safety]") {
    Adsr envelope;
    envelope.set_sample_rate(48000.0f);
    envelope.set_params({0.001f, 0.005f, 0.6f, 0.01f});

    LatencyAwareControlSmoother::Config smoother_config;
    smoother_config.domain = LatencyAwareControlSmoother::Domain::semitone;
    smoother_config.attack_seconds = 0.01f;
    smoother_config.release_seconds = 0.02f;
    LatencyAwareControlSmoother latency_smoother;
    latency_smoother.prepare(48000.0, smoother_config);
    latency_smoother.set_immediate(0.0f);

    SincResampler sinc;
    sinc.build(8, 64, 8.0);
    std::array<float, 16> sinc_samples {};
    for (std::size_t i = 0; i < sinc_samples.size(); ++i)
        sinc_samples[i] = std::sin(static_cast<float>(i) * 0.1f);

    Resampler resampler;
    ResamplerQuality quality;
    quality.stopband_db = 40.0;
    quality.transition_fraction = 0.25;
    quality.cutoff_fraction = 0.85;
    quality.phases = 8;
    resampler.prepare(48000.0, 44100.0, 1, 16, quality);
    std::array<float, 16> resampler_input {};
    std::array<float, 32> resampler_output {};
    for (std::size_t i = 0; i < resampler_input.size(); ++i)
        resampler_input[i] = static_cast<float>(i % 5) * 0.05f;
    const float* resampler_inputs[] = {resampler_input.data()};
    float* resampler_outputs[] = {resampler_output.data()};

    require_allocates_no_memory([&] {
        std::array<float, 16> mono {};
        std::array<float, 16> left {};
        std::array<float, 16> right {};
        for (std::size_t i = 0; i < mono.size(); ++i) {
            mono[i] = 1.0f;
            left[i] = 0.5f;
            right[i] = -0.5f;
        }
        float* envelope_channels[] = {left.data(), right.data()};

        envelope.note_on();
        (void)envelope.next();
        envelope.apply_to_buffer(mono.data(), 0, static_cast<int>(mono.size()));
        envelope.apply_to_buffer(envelope_channels, 2, 0, static_cast<int>(left.size()));
        envelope.note_off();
        (void)envelope.is_active();
        (void)envelope.stage();
        envelope.reset();

        latency_smoother.set_target(7.0f);
        (void)latency_smoother.value_at(12);
        (void)latency_smoother.value_at(-4);
        (void)latency_smoother.ratio_at(8);
        (void)latency_smoother.advance(16);
        (void)latency_smoother.target();
        (void)latency_smoother.current();
        (void)latency_smoother.is_settled(0.001f);
        latency_smoother.set_immediate(-2.0f);

        (void)sinc.half_width();
        (void)sinc.taps();
        (void)sinc.ready();
        (void)sinc.apply(sinc_samples.data(), 0.25);
        (void)sinc.read(sinc_samples.data(), static_cast<int>(sinc_samples.size()), 4.5);

        resampler.set_ratio(48000.0, 44000.0);
        (void)resampler.process_block_detailed(
            resampler_inputs, resampler_input.size(),
            resampler_outputs, resampler_output.size());
        (void)resampler.process_block(
            resampler_inputs, 4,
            resampler_outputs, 8);
        (void)resampler.process_block_mono_detailed(
            resampler_input.data(), 4,
            resampler_output.data(), 8);
        (void)resampler.process_block_mono(
            resampler_input.data(), 4,
            resampler_output.data(), 8);
        (void)resampler.max_output_for(16);
        (void)resampler.taps_per_phase();
        (void)resampler.phases();
        (void)resampler.prototype_length();
        (void)resampler.input_rate();
        (void)resampler.output_rate();
        (void)resampler.channels();
        resampler.reset();
    });
}

TEST_CASE("FFT and simple convolver hot paths are allocation-free after setup",
          "[signal][fft][rt-safety]") {
    Fft fft(16);
    std::array<std::complex<float>, 16> complex_samples {};
    std::array<float, 16> real_samples {};
    std::array<float, 16> magnitudes {};
    std::array<float, 16> magnitudes_db {};
    for (std::size_t i = 0; i < complex_samples.size(); ++i) {
        real_samples[i] = std::sin(static_cast<float>(i) * 0.2f);
        complex_samples[i] = {real_samples[i], static_cast<float>(i % 3) * 0.1f};
    }

    Convolver convolver;
    const std::array<float, 4> impulse {0.5f, 0.25f, 0.125f, 0.0f};
    convolver.load_ir(impulse.data(), static_cast<int>(impulse.size()), 8);
    std::array<float, 16> convolver_input {};
    std::array<float, 16> convolver_output {};
    for (std::size_t i = 0; i < convolver_input.size(); ++i)
        convolver_input[i] = (i == 0) ? 1.0f : 0.0f;

    require_allocates_no_memory([&] {
        fft.forward(complex_samples.data());
        fft.inverse(complex_samples.data());
        fft.forward_real(real_samples.data(), complex_samples.data());
        fft.magnitude(complex_samples.data(), magnitudes.data(), static_cast<int>(magnitudes.size()));
        fft.magnitude_db(complex_samples.data(), magnitudes_db.data(), static_cast<int>(magnitudes_db.size()));
        (void)fft.size();

        for (float sample : convolver_input)
            (void)convolver.process(sample);
        convolver.process(convolver_input.data(), convolver_output.data(), static_cast<int>(convolver_input.size()));
        convolver.reset();
    });
}

TEST_CASE("Multi-backend FFT and non-uniform convolver hot paths are allocation-free after setup",
          "[signal][fft][rt-safety]") {
    MultiBackendFft multi_fft(16, FftBackend::kissfft);
    std::array<std::complex<float>, 16> fft_samples {};
    for (std::size_t i = 0; i < fft_samples.size(); ++i)
        fft_samples[i] = {std::sin(static_cast<float>(i) * 0.25f),
                          std::cos(static_cast<float>(i) * 0.125f)};

    NonUniformPartitionedConvolver convolver;
    std::array<float, 32> impulse {};
    impulse[0] = 0.5f;
    impulse[1] = 0.25f;
    impulse[8] = 0.125f;
    impulse[16] = 0.0625f;
    convolver.load_ir(impulse.data(), impulse.size(), 8, 2);
    std::array<float, 8> input {};
    std::array<float, 8> output {};
    input[0] = 1.0f;

    require_allocates_no_memory([&] {
        multi_fft.forward(fft_samples.data());
        multi_fft.inverse(fft_samples.data());
        (void)multi_fft.size();
        (void)multi_fft.backend();

        convolver.process(input.data(), output.data(), input.size());
        convolver.process(input.data(), output.data(), input.size());
        convolver.reset();
        (void)convolver.latency();
        (void)convolver.block_size();
        (void)convolver.head_samples();
        (void)convolver.tail_block();
        (void)convolver.tail_multiplier();
        (void)convolver.is_loaded();
    });
}

TEST_CASE("Prepared freeze and pitched delay helpers are allocation-free while processing",
          "[signal][freeze][rt-safety]") {
    FreezeHold freeze_hold;
    FreezeHold::Config freeze_config;
    freeze_config.fft_size = 256;
    freeze_config.channels = 2;
    freeze_config.analysis_hop = 64;
    freeze_config.capture_frames = 3;
    freeze_config.crossfade_frames = 2;
    freeze_hold.prepare(freeze_config);

    std::array<std::complex<float>, 129> freeze_left {};
    std::array<std::complex<float>, 129> freeze_right {};
    for (std::size_t i = 0; i < freeze_left.size(); ++i) {
        freeze_left[i] = {1.0f + static_cast<float>(i) * 0.001f,
                          static_cast<float>(i % 5) * 0.01f};
        freeze_right[i] = {0.5f + static_cast<float>(i) * 0.001f,
                           static_cast<float>(i % 7) * 0.01f};
    }
    std::complex<float>* freeze_frames[] = {freeze_left.data(), freeze_right.data()};

    FreezeLoopSampler loop_sampler;
    loop_sampler.prepare(2, 64, 8);
    std::array<float, 32> loop_in_l {};
    std::array<float, 32> loop_in_r {};
    std::array<float, 32> loop_out_l {};
    std::array<float, 32> loop_out_r {};
    for (std::size_t i = 0; i < loop_in_l.size(); ++i) {
        loop_in_l[i] = std::sin(static_cast<float>(i) * 0.1f);
        loop_in_r[i] = std::cos(static_cast<float>(i) * 0.1f);
    }
    const float* loop_inputs[] = {loop_in_l.data(), loop_in_r.data()};
    float* loop_outputs[] = {loop_out_l.data(), loop_out_r.data()};

    PitchedFeedbackDelay delay;
    PitchedFeedbackDelay::Config delay_config;
    delay_config.channels = 2;
    delay_config.max_delay_seconds = 0.1f;
    delay_config.max_block = 64;
    delay.prepare(48000.0, delay_config);
    RtProbeLoopProcessor loop_processor;
    loop_processor.channels = 2;
    delay.set_loop_processor(&loop_processor);
    delay.set_delay_ms(10.0f);
    delay.set_feedback(0.5f);
    std::array<float, 64> delay_in_l {};
    std::array<float, 64> delay_in_r {};
    std::array<float, 64> delay_out_l {};
    std::array<float, 64> delay_out_r {};
    delay_in_l[0] = 1.0f;
    delay_in_r[0] = -1.0f;
    const float* delay_inputs[] = {delay_in_l.data(), delay_in_r.data()};
    float* delay_outputs[] = {delay_out_l.data(), delay_out_r.data()};

    require_allocates_no_memory([&] {
        for (int i = 0; i < 4; ++i)
            freeze_hold.process_group(freeze_frames, 2, 129);
        freeze_hold.set_frozen(true);
        for (int i = 0; i < 4; ++i)
            freeze_hold.process_group(freeze_frames, 2, 129);
        freeze_hold.set_frozen(false);
        freeze_hold.process_group(freeze_frames, 2, 129);
        (void)freeze_hold.is_engaged();
        (void)freeze_hold.is_latched();
        freeze_hold.reset();

        loop_sampler.write(loop_inputs, 32);
        loop_sampler.freeze(16);
        loop_sampler.read(loop_outputs, 16);
        loop_sampler.release();
        loop_sampler.read(loop_outputs, 16);
        loop_sampler.reset();
        (void)loop_sampler.channels();
        (void)loop_sampler.frozen();
        (void)loop_sampler.loop_length();

        delay.process(delay_inputs, delay_outputs, 64);
        loop_processor.frozen = true;
        delay.process(delay_inputs, delay_outputs, 32);
        delay.set_delay_sync(120.0, 0.25);
        delay.set_feedback(0.25f);
        (void)delay.min_delay_samples();
        delay.reset();
    });
}

TEST_CASE("Prepared spectral analysis helpers are allocation-free while processing",
          "[signal][spectral][rt-safety]") {
    Stft stft;
    StftConfig stft_config;
    stft_config.fft_size = 256;
    stft_config.hop_size = 64;
    stft.configure(stft_config);
    std::array<float, 256> stft_input {};
    for (std::size_t i = 0; i < stft_input.size(); ++i)
        stft_input[i] = std::sin(static_cast<float>(i) * 0.05f);

    SpectralFrameEngine engine;
    SpectralFrameEngineConfig engine_config;
    engine_config.fft_size = 256;
    engine_config.analysis_hop = 64;
    engine_config.channels = 2;
    engine_config.max_block = 128;
    engine_config.max_synthesis_hop = 128;
    engine.prepare(engine_config);
    std::array<float, 128> engine_in_l {};
    std::array<float, 128> engine_in_r {};
    std::array<float, 128> engine_out_l {};
    std::array<float, 128> engine_out_r {};
    for (std::size_t i = 0; i < engine_in_l.size(); ++i) {
        engine_in_l[i] = std::sin(static_cast<float>(i) * 0.03f);
        engine_in_r[i] = std::cos(static_cast<float>(i) * 0.04f);
    }
    const float* engine_inputs[] = {engine_in_l.data(), engine_in_r.data()};
    float* engine_outputs[] = {engine_out_l.data(), engine_out_r.data()};

    StnDecomposer stn;
    StnConfig stn_config;
    stn_config.num_bins = 129;
    stn_config.time_median = 3;
    stn_config.freq_median = 3;
    stn.prepare(stn_config);
    std::array<float, 129> magnitudes {};
    for (std::size_t i = 0; i < magnitudes.size(); ++i)
        magnitudes[i] = 1.0f + static_cast<float>(i % 11) * 0.1f;

    TransientPhasePolicy transient;
    TransientPhasePolicy::Config transient_config;
    transient_config.fft_size = 256;
    transient.prepare(transient_config);
    std::array<std::complex<float>, 129> transient_l {};
    std::array<std::complex<float>, 129> transient_r {};
    std::complex<float>* mutable_transient_frames[] = {transient_l.data(), transient_r.data()};
    const std::complex<float>* transient_frames[] = {transient_l.data(), transient_r.data()};
    for (std::size_t i = 0; i < transient_l.size(); ++i) {
        transient_l[i] = {1.0f + static_cast<float>(i % 7) * 0.01f, 0.0f};
        transient_r[i] = {0.5f + static_cast<float>(i % 5) * 0.01f, 0.0f};
    }

    require_allocates_no_memory([&] {
        (void)stft.push_samples(stft_input.data(), static_cast<int>(stft_input.size()));
        (void)stft.latest_frame();
        (void)stft.frame_ready();
        (void)stft.num_bins();
        (void)stft.fft_size();
        (void)stft.hop_size();
        Stft::to_db(magnitudes.data(), static_cast<int>(magnitudes.size()));
        stft.reset();

        engine.process(engine_inputs, engine_outputs, 128,
                       [](std::complex<float>* const* frames, int bins) {
                           frames[0][0] *= 0.99f;
                           (void)bins;
                       });
        engine.analyze(engine_inputs, 128,
                       [&](std::complex<float>* const* frames, int bins) {
                           engine.synthesize_frame(frames, 64);
                           (void)bins;
                       });
        const int available = engine.available_output();
        engine.read_output(engine_outputs, std::min(available, 64));
        (void)engine.latency_samples();
        (void)engine.fft_size();
        (void)engine.analysis_hop();
        (void)engine.num_bins();
        (void)engine.channels();
        engine.reset();

        for (int i = 0; i < 4; ++i) {
            magnitudes[static_cast<std::size_t>(i)] += 0.5f;
            const auto& masks = stn.process(magnitudes.data());
            (void)masks.sines.data();
            (void)masks.transients.data();
            (void)masks.noise.data();
        }
        (void)stn.center_magnitude();
        (void)stn.latency_frames();
        (void)stn.num_bins();
        stn.reset();

        for (int i = 0; i < 12; ++i) {
            mutable_transient_frames[0][static_cast<std::size_t>(i % 129)].real(2.0f);
            (void)transient.analyze(transient_frames, 2, 129);
        }
        transient.reset();
    });
}

TEST_CASE("Prepared spectral envelope shifter is allocation-free while processing",
          "[signal][spectral][rt-safety]") {
    SpectralEnvelopeShifter shifter;
    SpectralEnvelopeShifterConfig config;
    config.fft_size = 256;
    config.true_envelope_iterations = 2;
    shifter.prepare(config);

    std::array<std::complex<float>, 129> left {};
    std::array<std::complex<float>, 129> right {};
    std::complex<float>* frames[] = {left.data(), right.data()};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = {1.0f + static_cast<float>(i % 9) * 0.02f, 0.0f};
        right[i] = {0.8f + static_cast<float>(i % 7) * 0.02f, 0.0f};
    }

    require_allocates_no_memory([&] {
        shifter.process_group(frames, 2, shifter.num_bins(), 1.15f);
        (void)shifter.num_bins();
        (void)shifter.order();
    });
}

TEST_CASE("Prepared realtime pitch/time processor is allocation-free while processing",
          "[signal][spectral][rt-safety]") {
    RealtimePitchTimeConfig pitch_config;
    pitch_config.mode = PitchTimeMode::realtime_pitch;
    pitch_config.quality = PitchTimeQuality::low_latency;
    pitch_config.channels = 2;
    pitch_config.max_block = 256;
    pitch_config.max_pitch_semitones = 12.0f;
    pitch_config.formant_mode = FormantMode::preserve;
    pitch_config.noise_morphing = true;
    pitch_config.sinc_resampling = true;

    RealtimePitchTimeProcessor pitch;
    pitch.prepare(48000.0, pitch_config);
    pitch.set_pitch_semitones(3.0f);
    pitch.set_formant_semitones(-2.0f);
    pitch.set_frozen(false);

    std::array<float, 256> in_l {};
    std::array<float, 256> in_r {};
    std::array<float, 256> out_l {};
    std::array<float, 256> out_r {};
    for (std::size_t i = 0; i < in_l.size(); ++i) {
        in_l[i] = std::sin(static_cast<float>(i) * 0.025f);
        in_r[i] = std::cos(static_cast<float>(i) * 0.031f);
    }
    const float* inputs[] = {in_l.data(), in_r.data()};
    float* outputs[] = {out_l.data(), out_r.data()};

    RealtimePitchTimeConfig stretch_config = pitch_config;
    stretch_config.mode = PitchTimeMode::time_stretch;
    stretch_config.max_time_ratio = 1.5f;
    stretch_config.noise_morphing = false;
    stretch_config.sinc_resampling = false;

    RealtimePitchTimeProcessor stretch;
    stretch.prepare(48000.0, stretch_config);
    stretch.set_time_ratio(1.25f);
    stretch.set_formant_mode(FormantMode::follow);

    require_allocates_no_memory([&] {
        for (int block = 0; block < 10; ++block)
            pitch.process(inputs, outputs, static_cast<int>(in_l.size()));
        (void)pitch.latency_samples();
        (void)pitch.fft_size();
        (void)pitch.is_frozen();
        pitch.reset();

        for (int block = 0; block < 6; ++block)
            stretch.feed(inputs, static_cast<int>(in_l.size()));
        const int available = stretch.available_stretched();
        stretch.read_stretched(outputs, std::min(available, static_cast<int>(out_l.size())));
        (void)stretch.achieved_time_ratio();
        stretch.reset();
    });
}

TEST_CASE("Prepared storage-backed signal helpers are allocation-free while processing",
          "[signal][rt-safety]") {
    DelayLine delay;
    delay.prepare(64);

    FirFilter fir;
    fir.set_coefficients(std::vector<float>{0.25f, 0.5f, 0.25f});

    LookupTable table(64, -1.0f, 1.0f, [](float x) { return x * x * x; });

    ProcessorDuplicator<RtProbeGain> duplicator;
    duplicator.prepare(2, 48000.0f);
    duplicator.for_each([](RtProbeGain& g) { g.gain = 0.75f; });

    require_allocates_no_memory([&] {
        std::array<float, 32> left {};
        std::array<float, 32> right {};
        for (std::size_t i = 0; i < left.size(); ++i) {
            const float input = static_cast<float>(i + 1) * 0.01f;
            delay.push(input);
            left[i] = fir.process(delay.read(3.5f));
            right[i] = table.process(input);
        }

        float* channels[] = {left.data(), right.data()};
        duplicator.process(channels, 2, static_cast<int>(left.size()));
        duplicator.process_channel(right.data(), 1, static_cast<int>(right.size()));
        duplicator.reset();
        delay.reset();
        fir.reset();
    });
}

TEST_CASE("Dynamics and filter signal helpers are allocation-free after configuration",
          "[signal][rt-safety]") {
    Compressor compressor;
    Compressor::Params compressor_params;
    compressor_params.threshold_db = -18.0f;
    compressor_params.ratio = 3.0f;
    compressor_params.attack_ms = 1.0f;
    compressor_params.release_ms = 80.0f;
    compressor.set_params(compressor_params);
    compressor.set_sample_rate(48000.0f);
    compressor.set_sidechain_hpf_hz(120.0f);
    compressor.set_lookahead_ms(1.0f);

    Limiter limiter;
    limiter.set_sample_rate(48000.0f);
    limiter.set_threshold_db(-1.0f);
    limiter.set_release_ms(25.0f);

    NoiseGate gate;
    NoiseGate::Params gate_params;
    gate_params.threshold_db = -45.0f;
    gate_params.ratio = 8.0f;
    gate_params.attack_ms = 0.2f;
    gate_params.release_ms = 60.0f;
    gate.set_params(gate_params);
    gate.set_sample_rate(48000.0f);

    LadderFilter ladder;
    ladder.set_sample_rate(48000.0f);
    ladder.set_frequency(1400.0f);
    ladder.set_resonance(0.45f);

    LinkwitzRiley crossover;
    crossover.set_frequency(1200.0f, 48000.0f);

    TptFilter tpt;
    tpt.prepare(48000.0f);
    tpt.set_cutoff(800.0f);

    require_allocates_no_memory([&] {
        std::array<float, 64> signal {};
        std::array<float, 64> sidechain {};
        for (std::size_t i = 0; i < signal.size(); ++i) {
            signal[i] = (static_cast<float>(i % 17) - 8.0f) * 0.05f;
            sidechain[i] = (static_cast<float>(i % 11) - 5.0f) * 0.07f;
        }

        for (std::size_t i = 0; i < signal.size(); ++i) {
            float value = compressor.process_with_sidechain(signal[i], sidechain[i]);
            value = limiter.process(value);
            value = gate.process(value);
            value = ladder.process(value);
            const auto split = crossover.process(value);
            tpt.set_cutoff(500.0f + static_cast<float>(i) * 10.0f);
            const auto tpt_out = tpt.process(split.low + split.high);
            signal[i] = tpt_out.lowpass + tpt_out.highpass + tpt_out.allpass * 0.1f;
        }

        compressor.process(signal.data(), static_cast<int>(signal.size()));
        compressor.process_with_sidechain(signal.data(), sidechain.data(), static_cast<int>(signal.size()));
        limiter.process(signal.data(), static_cast<int>(signal.size()));
        gate.process(signal.data(), static_cast<int>(signal.size()));
        ladder.process(signal.data(), static_cast<int>(signal.size()));
        (void)compressor.latency_samples();
        (void)compressor.gain_reduction_db();
        (void)tpt.cutoff();
        (void)tpt.process_lowpass(0.1f);
        (void)tpt.process_highpass(0.1f);
        (void)tpt.process_allpass(0.1f);
        compressor.reset();
        limiter.reset();
        gate.reset();
        ladder.reset();
        crossover.reset();
        tpt.reset();
    });
}

TEST_CASE("Stateless math signal helpers are allocation-free",
          "[signal][rt-safety]") {
    require_allocates_no_memory([&] {
        std::array<float, 16> samples {};
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const float x = static_cast<float>(i) * 0.125f - 1.0f;
            float value = FastMath::tanh(x);
            value += FastMath::sin(x) + FastMath::cos(x);
            value += FastMath::exp2(x) + FastMath::log2(static_cast<float>(i + 1));
            value += FastMath::pow(static_cast<float>(i + 1) * 0.25f, 0.75f);
            value += FastMath::db_to_gain(-6.0f) + FastMath::gain_to_db(0.5f);
            value += FastMath::rcp(static_cast<float>(i + 1));
            value += FastMath::rsqrt(static_cast<float>(i + 1));
            value += FastMath::clamp_unit(x) + FastMath::soft_clip(x);

            value += Interpolator::linear(0.25f, x, x + 1.0f);
            value += Interpolator::hermite(0.5f, x - 1.0f, x, x + 1.0f, x + 2.0f);
            value += Interpolator::lagrange(0.5f, x - 1.0f, x, x + 1.0f, x + 2.0f);
            value += Interpolator::sinc6(0.25f, x - 2.0f, x - 1.0f, x, x + 1.0f, x + 2.0f, x + 3.0f);

            value += sinc(x) + bessel_i0(std::abs(x));
            value += gamma_fn(static_cast<float>(i + 1) * 0.25f + 1.0f);
            value += erf_fn(x) + erfc_fn(x);
            value += lanczos(x, 3);
            value += db_to_linear(-12.0f) + linear_to_db(0.25f);
            value += freq_to_midi(440.0f) + midi_to_freq(69.0f);
            value += static_cast<float>(special::elliptic_K(0.25));
            value += static_cast<float>(special::elliptic_E(0.25));
            value += static_cast<float>(special::jacobi_nome(0.25));
            double sn = 0.0;
            double cn = 0.0;
            double dn = 0.0;
            special::jacobi_sncndn(0.2, 0.25, &sn, &cn, &dn);
            value += static_cast<float>(sn + cn + dn);

            samples[i] = snap_to_zero(value);
        }

        snap_to_zero(samples.data(), static_cast<int>(samples.size()));
        (void)snap_threshold<float>();
        (void)is_denormal(std::numeric_limits<float>::denorm_min());

        Matrix4 transform_matrix =
            translation_matrix(1.0f, 2.0f, 3.0f)
            * rotation_z(0.25f)
            * rotation_y(0.5f)
            * rotation_x(0.75f)
            * scale_matrix(2.0f, 3.0f, 4.0f);
        transform_matrix = transform_matrix + Matrix4::zero();
        transform_matrix = transform_matrix.transposed().transposed();
        const auto scaled = transform_matrix * 0.5f;
        const Vec3 transformed = transform(scaled, Vec3{1.0f, 2.0f, 3.0f});
        (void)transformed;
        (void)(scaled == scaled);

        Matrix2 m2 = Matrix2::identity();
        m2(0, 1) = 2.0f;
        (void)determinant(m2);

        Matrix3 m3 = Matrix3::identity();
        m3(0, 1) = 2.0f;
        m3(1, 2) = -1.0f;
        (void)determinant(m3);
    });
}

TEST_CASE("Spectral design and SIMD utility hot paths are allocation-free after setup",
          "[signal][rt-safety]") {
    std::vector<float> window = WindowFunction::generate(16, WindowFunction::Type::hann);
    std::array<float, 16> samples {};
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(i + 1) * 0.01f;

    ColorMapper mapper(ColorRamp::inferno);

    FrequencyAxis axis;
    axis.configure(1024, 48000.0f, FrequencyScale::logarithmic);

    SpectrogramBuffer spectrogram;
    spectrogram.configure(8, 4);
    std::array<float, 8> magnitudes_db {-80.0f, -72.0f, -48.0f, -36.0f,
                                        -24.0f, -18.0f, -6.0f, 0.0f};

    AlignedBuffer aligned(8);
    std::array<float, 8> aligned_source {};
    for (std::size_t i = 0; i < aligned_source.size(); ++i)
        aligned_source[i] = static_cast<float>(i) * 0.125f;

    const std::vector<float> polynomial {1.0f, -2.0f, 0.5f, 0.25f};

    require_allocates_no_memory([&] {
        WindowFunction::apply(samples.data(), window);

        auto color = mapper.map(0.5f);
        mapper.set_ramp(ColorRamp::viridis);
        color = mapper.map(static_cast<float>(color.r) / 255.0f);
        (void)mapper.ramp();
        (void)color;

        const int display_bin = axis.display_to_bin(axis.bin_to_display(8));
        const float hz = axis.display_to_hz(axis.hz_to_display(axis.bin_to_hz(display_bin)));
        (void)axis.num_bins();
        (void)axis.nyquist();
        (void)axis.scale();
        (void)hz;

        spectrogram.push_column(magnitudes_db.data(), static_cast<int>(magnitudes_db.size()), mapper);
        (void)spectrogram.pixels();
        (void)spectrogram.write_column();
        (void)spectrogram.frames_written();

        aligned.copy_from(aligned_source.data(), aligned_source.size());
        aligned.clear();
        float sum = 0.0f;
        for (float value : aligned)
            sum += value;
        aligned.resize(aligned.size());
        (void)sum;

        float poly = Polynomial::eval(polynomial, 0.25f);
        poly += Polynomial::eval_complex(polynomial, {0.25f, 0.5f}).real();
        const auto roots = Polynomial::roots_quadratic(1.0f, -3.0f, 2.0f);
        poly += roots.first.real() + roots.second.real();

        Mat2 m2;
        m2.m[0][1] = 0.5f;
        m2.m[1][0] = -0.25f;
        const Mat2 m2_inv = m2.inverse();
        poly += m2.determinant() + m2_inv.determinant();

        Mat3 m3;
        m3.m[0][1] = 0.25f;
        m3.m[1][2] = -0.5f;
        const Mat3 m3_product = m3 * Mat3::identity();
        poly += m3_product.determinant();

        auto low = FilterDesign::lowpass(1000.0f, 0.707f, 48000.0f);
        auto high = FilterDesign::highpass(1000.0f, 0.707f, 48000.0f);
        auto band = FilterDesign::bandpass(1200.0f, 1.0f, 48000.0f);
        auto notch = FilterDesign::notch(1200.0f, 1.0f, 48000.0f);
        auto allpass = FilterDesign::allpass(1400.0f, 0.8f, 48000.0f);
        auto peak = FilterDesign::peaking_eq(900.0f, 1.1f, 3.0f, 48000.0f);
        auto low_shelf = FilterDesign::low_shelf(250.0f, 2.0f, 48000.0f);
        auto high_shelf = FilterDesign::high_shelf(6000.0f, -2.0f, 48000.0f);
        poly += low.b0 + high.b0 + band.b0 + notch.b0 + allpass.b0
              + peak.b0 + low_shelf.b0 + high_shelf.b0;
        (void)poly;
    });
}

TEST_CASE("Prepared phase and half-band helpers are allocation-free while processing",
          "[signal][rt-safety]") {
    HalfBandAllpassSection section(0.5f);
    HalfBandUpsampler2x upsampler;
    HalfBandDownsampler2x downsampler;
    EllipticHalfBandUpsampler2x elliptic_upsampler;
    EllipticHalfBandDownsampler2x elliptic_downsampler;

    NoiseMorpher noise_morpher;
    noise_morpher.prepare(8, 0x1234ull);
    noise_morpher.prepare_masked_scratch();

    MultichannelPhaseCoordinator coordinator;
    coordinator.prepare(256, 2);

    std::array<float, 8> envelope_a {};
    std::array<float, 8> envelope_b {};
    std::array<float, 8> mask {};
    for (std::size_t i = 0; i < envelope_a.size(); ++i) {
        envelope_a[i] = 0.1f + static_cast<float>(i) * 0.01f;
        envelope_b[i] = 0.2f + static_cast<float>(i) * 0.02f;
        mask[i] = (i % 2 == 0) ? 0.25f : 0.75f;
    }
    noise_morpher.push_envelope(envelope_a.data());

    std::array<std::complex<float>, 8> noise_bins {};
    std::array<float, 16> halfband_input {};
    std::array<float, 32> up_output {};
    std::array<float, 16> down_output {};
    for (std::size_t i = 0; i < halfband_input.size(); ++i)
        halfband_input[i] = static_cast<float>(i + 1) * 0.01f;

    std::array<std::complex<float>, 129> left {};
    std::array<std::complex<float>, 129> right {};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = {static_cast<float>(i) * 0.001f, static_cast<float>(i % 5) * 0.01f};
        right[i] = {static_cast<float>(i) * 0.002f, static_cast<float>(i % 7) * 0.01f};
    }
    std::complex<float>* frames[] = {left.data(), right.data()};

    require_allocates_no_memory([&] {
        float lo = 0.0f;
        float hi = 0.0f;
        (void)section.process(0.25f);
        section.set_coefficient(0.25f);
        (void)section.coefficient();
        section.reset();

        (void)upsampler.sections_a();
        (void)upsampler.sections_b();
        upsampler.process(0.125f, lo, hi);
        upsampler.process_block(halfband_input, up_output);
        upsampler.reset();

        (void)downsampler.sections_a();
        (void)downsampler.sections_b();
        (void)downsampler.process(lo, hi);
        downsampler.process_block(up_output, down_output);
        downsampler.reset();

        (void)elliptic_upsampler.direct_sections();
        (void)elliptic_upsampler.delayed_sections();
        elliptic_upsampler.process(0.125f, lo, hi);
        elliptic_upsampler.reset();

        (void)elliptic_downsampler.direct_sections();
        (void)elliptic_downsampler.delayed_sections();
        (void)elliptic_downsampler.process(lo, hi);
        elliptic_downsampler.reset();

        (void)noise_morpher.num_bins();
        noise_morpher.push_envelope(envelope_b.data());
        noise_morpher.synthesize(0.5f, noise_bins.data());
        noise_morpher.push_masked_envelope(envelope_b.data(), mask.data());
        noise_morpher.advance();
        noise_morpher.reset();

        coordinator.process_group(frames, 129, 64, 64);
        (void)coordinator.reference_magnitudes();
        (void)coordinator.num_bins();
        coordinator.reset();
    });
}

TEST_CASE("Prepared delay effect helpers are allocation-free while processing",
          "[signal][rt-safety]") {
    Chorus chorus;
    chorus.prepare(48000.0f);
    chorus.set_rate(0.8f);
    chorus.set_depth(0.4f);
    chorus.set_mix(0.35f);
    chorus.set_delay_ms(12.0f);

    Reverb reverb;
    reverb.prepare(48000.0f);
    reverb.set_decay(1.5f);
    reverb.set_damping(0.45f);
    reverb.set_mix(0.2f);

    bool output_stayed_finite = false;
    require_allocates_no_memory([&] {
        float accumulator = 0.0f;
        for (int i = 0; i < 96; ++i) {
            const float input = i == 0 ? 1.0f : 0.0f;
            const auto chorus_out = chorus.process(input);
            const auto reverb_out = reverb.process(chorus_out.left + chorus_out.right);
            accumulator += reverb_out.left + reverb_out.right;
        }
        output_stayed_finite = std::isfinite(accumulator);
        chorus.reset();
        reverb.reset();
    });
    REQUIRE(output_stayed_finite);
}

TEST_CASE("ProcessorChain and wavetable playback are allocation-free after setup",
          "[signal][rt-safety]") {
    ProcessorChain<Gain, Biquad, WaveShaper> chain;
    chain.get<0>().set_gain_linear(0.8f);
    chain.get<1>().set_coefficients(Biquad::Type::highpass, 120.0f, 0.707f, 48000.0f);
    chain.get<2>().set_curve(WaveShaper::Curve::tanh_clip);

    Wavetable table({
        WavetableEntry{{0.0f, 1.0f, 0.0f, -1.0f}, 12000.0f},
        WavetableEntry{{0.0f, 0.5f, 0.0f, -0.5f}, 24000.0f},
    });
    table.set_sample_rate(48000.0f);
    table.set_frequency(440.0f);

    WavetableBank bank({Wavetable::make_sine(64), Wavetable::make_triangle(2, 64)});
    bank.set_sample_rate(48000.0f);
    bank.set_frequency(220.0f);
    bank.set_position(0.4f);

    require_allocates_no_memory([&] {
        std::array<float, 64> buffer {};
        for (auto& sample : buffer) {
            sample = chain.process(table.next() + bank.next());
        }
        chain.process(buffer.data(), static_cast<int>(buffer.size()));
        chain.reset();
        table.reset();
    });
}
