#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/bias.hpp>
#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fast_math.hpp>
#include <pulp/signal/interpolator.hpp>
#include <pulp/signal/matrix.hpp>
#include <pulp/signal/processor_duplicator.hpp>
#include <pulp/signal/signal.hpp>
#include <pulp/signal/special_functions.hpp>
#include <pulp/signal/oversampling.hpp>
#include <pulp/signal/wavetable.hpp>

#include <array>
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

TEST_CASE("Scalar signal helpers are allocation-free after configuration",
          "[signal][rt-safety]") {
    Bias bias;
    bias.set_bias(0.1f);

    DcBlocker<float> dc_blocker;
    dc_blocker.set_pole(0.995f);

    Gain gain;
    gain.set_gain_linear(0.5f);

    SimpleMixer mixer;
    mixer.set_mix(0.25f);

    Oscillator osc;
    osc.set_sample_rate(48000.0f);
    osc.set_frequency(220.0f);
    osc.set_waveform(Oscillator::Waveform::saw);

    Biquad biquad;
    biquad.set_coefficients(Biquad::Type::lowpass, 1800.0f, 0.707f, 48000.0f);

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
        }

        bias.process(samples.data(), static_cast<int>(samples.size()));
        bias.process(samples.data(), mixed.data(), static_cast<int>(samples.size()));
        dc_blocker.process(samples.data(), static_cast<int>(samples.size()));
        gain.process(samples.data(), static_cast<int>(samples.size()));
        mixer.process(dry.data(), wet.data(), mixed.data(), static_cast<int>(mixed.size()));
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
