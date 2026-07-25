// Allocation-probe roster for the modulation and utility toolkit. Extracted
// from test_signal_rt_safety.cpp to keep that file under ~1,200 lines; the
// harness and the contract are identical.
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/chaos.hpp>
#include <pulp/signal/envelope.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/lpg.hpp>
#include <pulp/signal/mod_matrix.hpp>
#include <pulp/signal/mod_tools.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/trigger.hpp>
#include <pulp/signal/units.hpp>
#include <pulp/signal/vca.hpp>

#include <array>
#include <span>

using namespace pulp::signal;

namespace {

constexpr double kSampleRate = 48000.0;

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

} // namespace

TEST_CASE("Deterministic randomness is allocation-free", "[signal][mod][rt-safety]") {
    Xorshift32 rng(2024u);
    OuWalk walk;
    walk.prepare(kSampleRate);
    walk.set_theta(1.0);
    walk.set_sigma(0.2);
    walk.seed(7u);
    walk.reset();

    Drift drift;
    drift.prepare(kSampleRate);
    drift.set_cents(6.0);
    drift.seed(9u);
    drift.reset();

    require_allocates_no_memory([&] {
        float accumulator = 0.0f;
        for (int i = 0; i < 512; ++i) {
            accumulator += rng.next_bipolar() + rng.gaussian() + walk.next();
            drift.next();
            accumulator += drift.pitch_factor() + drift.fraction();
            accumulator += unit_from(rng_key(3ull, static_cast<std::uint64_t>(i)));
        }
        rng.reset();
        walk.reset();
        drift.reset();
        (void)accumulator;
    });
}

TEST_CASE("Lfo output paths are allocation-free", "[signal][mod][rt-safety]") {
    Lfo lfo;
    lfo.prepare(kSampleRate);
    lfo.set_rate_hz(4.0);
    lfo.set_delay_ms(5.0);
    lfo.set_fade_in_ms(10.0);
    lfo.set_fade_out_ms(10.0, true);
    lfo.set_repeat_count(8);
    lfo.set_mode(Lfo::Mode::retrig);
    lfo.set_stereo_offset(0.25f);
    lfo.set_random_segments(Lfo::kDefaultRandomSegments);
    lfo.reset();

    require_allocates_no_memory([&] {
        float accumulator = 0.0f;
        for (auto wave : {Lfo::Wave::sine, Lfo::Wave::triangle, Lfo::Wave::saw_up,
                          Lfo::Wave::saw_down, Lfo::Wave::square,
                          Lfo::Wave::sh_random, Lfo::Wave::smooth_random}) {
            lfo.set_wave(wave);
            for (int i = 0; i < 256; ++i) {
                accumulator += lfo.next() + lfo.next_unipolar();
                float left = 0.0f;
                float right = 0.0f;
                lfo.next_stereo(left, right);
                float s = 0.0f;
                float c = 0.0f;
                lfo.next_quadrature(s, c);
                accumulator += left + right + s + c;
            }
            lfo.retrigger();
        }
        lfo.set_shape_morph(1.7f);
        for (int i = 0; i < 256; ++i) accumulator += lfo.next();
        lfo.reset();
        (void)accumulator;
    });
}

TEST_CASE("Control-signal tools are allocation-free", "[signal][mod][rt-safety]") {
    SlewLimiter slew;
    slew.prepare(static_cast<float>(kSampleRate));
    slew.set_times_ms(5.0f, 50.0f);

    SampleHold hold;
    hold.prepare(static_cast<float>(kSampleRate));
    hold.set_glide_ms(10.0f);

    Attenuverter attenuverter;
    attenuverter.set_gain(-0.5f);
    attenuverter.set_offset(0.5f);

    Rectifier rectifier;
    Comparator comparator;
    comparator.set_threshold(0.25f);

    Quantizer quantizer;
    quantizer.set_range(-1.0f, 1.0f);
    quantizer.set_steps(12);

    Curve curve;
    curve.set_curve(0.6f);

    require_allocates_no_memory([&] {
        float accumulator = 0.0f;
        for (int i = 0; i < 1024; ++i) {
            const float x = static_cast<float>(i % 97) / 97.0f - 0.5f;
            accumulator += slew.process(x);
            accumulator += hold.process(x, i % 32 == 0);
            accumulator += attenuverter.process(x);
            accumulator += rectifier.process(x);
            accumulator += comparator.process(x) ? 1.0f : 0.0f;
            accumulator += quantizer.process(x);
            accumulator += curve.process(std::abs(x));
            accumulator += stage_curve(std::abs(x), 0.5f) + smoothstep(std::abs(x));
        }
        slew.reset();
        hold.reset();
        comparator.reset();
        (void)accumulator;
    });
}

TEST_CASE("Trigger and gate kit is allocation-free", "[signal][mod][rt-safety]") {
    TriggerDetect detect;
    detect.prepare(kSampleRate);

    GateGen gate;
    gate.prepare(kSampleRate);
    gate.set_length_ms(20.0);
    gate.set_retrigger(GateGen::Retrigger::extend);

    ClockDivider divider;
    divider.set_division(3);

    ClockMult multiplier;
    multiplier.set_multiplier(4);

    BurstGen burst;
    burst.prepare(kSampleRate);
    burst.set_count(BurstGen::kMaxCount);
    burst.set_spacing_ms(15.0);
    burst.set_spacing_curve(-0.5f);

    TrigDelay delay;
    delay.prepare(kSampleRate);
    delay.set_delay_ms(12.0);

    require_allocates_no_memory([&] {
        float accumulator = 0.0f;
        for (int i = 0; i < 4096; ++i) {
            const bool source = (i % 480) == 0;
            const bool trigger = detect.process(source);
            accumulator += gate.process(trigger) ? 1.0f : 0.0f;
            accumulator += divider.process(trigger) ? 1.0f : 0.0f;
            accumulator += multiplier.process(trigger) ? 1.0f : 0.0f;
            accumulator += burst.process(trigger).level;
            accumulator += delay.process(trigger) ? 1.0f : 0.0f;
        }
        detect.reset();
        gate.reset();
        divider.reset();
        multiplier.reset();
        burst.reset();
        delay.reset();
        (void)accumulator;
    });
}

TEST_CASE("Envelope family is allocation-free", "[signal][mod][rt-safety]") {
    Ar ar;
    Ad ad;
    Ahd ahd;
    Dahdsr dahdsr;
    ModEnv mod_env;
    TransientDetector transient;

    ar.prepare(kSampleRate);
    ad.prepare(kSampleRate);
    ahd.prepare(kSampleRate);
    dahdsr.prepare(kSampleRate);
    mod_env.prepare(kSampleRate);
    transient.prepare(static_cast<float>(kSampleRate));

    ad.set_loop(true, 0);
    ahd.set_loop(true, 4);
    dahdsr.set_loop(true, 0);
    dahdsr.set_delay_ms(2.0);
    dahdsr.set_hold_ms(2.0);
    mod_env.set_depth(-0.5f);

    require_allocates_no_memory([&] {
        float accumulator = 0.0f;
        for (int i = 0; i < 4096; ++i) {
            if (i % 1024 == 0) {
                ar.gate(true);
                ad.trigger(0.8f);
                ahd.trigger(0.6f);
                dahdsr.note_on(0.9f);
                mod_env.trigger();
            }
            if (i % 1024 == 512) {
                ar.gate(false);
                dahdsr.note_off();
            }
            accumulator += ar.next() + ad.next() + ahd.next() + dahdsr.next()
                           + mod_env.next() + mod_env.modulation();
            accumulator += transient.process(accumulator * 0.001f);
        }
        ar.reset();
        ad.reset();
        ahd.reset();
        dahdsr.reset();
        mod_env.reset();
        transient.reset();
        (void)accumulator;
    });
}

TEST_CASE("Vca, Lpg, matrix, and chaos are allocation-free",
          "[signal][mod][rt-safety]") {
    Vca vca;
    vca.prepare(static_cast<float>(kSampleRate));
    vca.set_response(Vca::Response::exponential);
    vca.set_lag_ms(2.0);

    Lpg lpg;
    lpg.prepare(static_cast<float>(kSampleRate));
    lpg.set_decay_ms(220.0);
    lpg.set_colour(0.6f);
    lpg.reset();

    ModMatrix matrix;
    for (int i = 0; i < ModMatrix::kMaxSlots; ++i)
        matrix.set_slot(i, i % 4, i % 2, 0.1f, (i % 3) - 1);

    LogisticMap chaos;
    chaos.set_r(3.87);
    chaos.seed(0.37);
    chaos.reset();

    std::array<float, 4> sources{};
    std::array<float, 2> dests{};

    require_allocates_no_memory([&] {
        float accumulator = 0.0f;
        for (int i = 0; i < 2048; ++i) {
            if (i % 1440 == 0) lpg.strike(Lpg::velocity_to_strike(0.8f));
            const float excitation = static_cast<float>(i % 31) / 31.0f - 0.5f;
            accumulator += lpg.process(excitation);
            accumulator += vca.process(excitation, 0.5f);
            accumulator += chaos.next() + chaos.next_bipolar();

            sources.fill(accumulator * 0.0001f);
            dests.fill(0.0f);
            matrix.evaluate(std::span<const float>(sources), std::span<float>(dests));
            accumulator += dests[0] + dests[1];
        }
        vca.reset();
        lpg.reset();
        chaos.reset();
        (void)accumulator;
    });
}

TEST_CASE("Unit conversions are allocation-free", "[signal][mod][rt-safety]") {
    using namespace pulp::signal::units;
    require_allocates_no_memory([&] {
        float accumulator = 0.0f;
        for (int i = 0; i < 1024; ++i) {
            const float t = static_cast<float>(i) / 1024.0f;
            accumulator += db_to_linear(-t * 60.0f) + linear_to_db(t + 0.001f);
            accumulator += midi_to_hz(21.0f + t * 87.0f) + hz_to_midi(100.0f + t);
            accumulator += semitones_to_ratio(t * 12.0f) + cents_to_ratio(t * 50.0f);
            accumulator += ms_to_onepole_coef(1.0f + t, 48000.0f);
            accumulator += t60_to_per_sample_gain(0.5f + t, 48000.0f);
            accumulator += taper_log(t, 0.01f, 20.0f);
            accumulator += division_to_beats(i % kDivisionCount);
            accumulator += static_cast<float>(
                division_to_samples(static_cast<Division>(i % kDivisionCount), 120.0f, 48000.0));
        }
        (void)accumulator;
    });
}
