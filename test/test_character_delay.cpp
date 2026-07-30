// Multi-character delay — audio-domain acceptance suite.
//
// Every case measures rendered output rather than implementation detail. Shared
// deterministic stimuli and measurements live in support/character_delay_fixture.hpp.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/character_delay_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace pulp::test::character_delay;

#include "harness/rt_allocation_probe.hpp"


// ═══════════════════════════════════════════════════════════════════════════
// 1 — Engine-time accuracy
// ═══════════════════════════════════════════════════════════════════════════
TEST_CASE("every character places its repeat at the requested time",
          "[character-delay][time]") {
    // R1, onset form. Delay time is measured from the ONSET of a short windowed
    // burst, referenced to the stimulus's own onset so the burst's rise time
    // cancels — one recipe for every character, with no character-conditional
    // test logic.
    //
    // Onset rather than peak because the peak does not measure the delay for
    // three of the five. Through a compander the peak measures the expander's
    // ~10 ms attack: a bare impulse comes back with its peak displaced a fixed
    // ~4.6 ms whatever the delay is set to, which at a 10 ms delay reads as a
    // 46% error that has nothing to do with the clock grid. Through the
    // diffuser the peak sits somewhere inside a smear that moves with the
    // character macro. Where a repeat BEGINS is what the delay time sets, and
    // for the linear characters onset and peak agree anyway.
    struct Case {
        Character character;
        double amount;
        bool relative_tolerance;  // clocked characters get +-2%, the rest +-1 ms
    };
    const Case cases[] = {
        {Character::clean, 0.0, false},
        {Character::diffusion, 0.5, false},
        {Character::tape, 0.0, false},
        {Character::bbd, 0.5, true},
        {Character::vintage_digital, 0.5, true},
    };

    for (const auto& c : cases) {
        for (double time_ms : {10.0, 100.0, 350.0, 1000.0, 2000.0}) {
            Engine delay;
            configure(delay, c.character, time_ms, 0.0, c.amount);
            settle(delay, 4.0 * slew_seconds(c.character) + 0.4);

            const int frames = static_cast<int>(kSr * (time_ms * 0.002 + 0.3));
            auto buffers = burst_left(frames, 1000.0, 0.002);
            const int stimulus_onset = onset_index(buffers.left, 0.05);
            render(delay, buffers);
            const int measured = onset_index(buffers.left, 0.05) - stimulus_onset;
            const double expected = time_ms * 0.001 * kSr;
            const double tolerance = c.relative_tolerance ? 0.02 * expected : 0.001 * kSr;

            INFO("character index " << static_cast<int>(c.character) << ", time " << time_ms
                                    << " ms, onset at " << measured << ", expected " << expected);
            REQUIRE(measured > 0);
            CHECK(std::abs(measured - expected) <= tolerance);
        }
    }
}

TEST_CASE("reverse keeps the requested delay time on the clocked characters",
          "[character-delay][time][reverse]") {
    // BBD and Vintage own a line the segmenter cannot replace, so in reverse the
    // two delays add. If the line kept the requested time the total would be
    // double it — and the layer above multiplies milliseconds trusting the
    // module, so every tempo-synced patch would land in the wrong place. The
    // line is therefore pinned short (kReverseLineMs) and the segment carries
    // the requested time.
    //
    // Measured DIFFERENTIALLY against Clean, which has no line of its own in
    // reverse. An absolute measurement would be meaningless here: a reversed
    // segment plays back-to-front, so a transient's delay sweeps the whole range
    // [1, 2L] depending where in the segment cycle it lands, and a burst at the
    // start of a segment comes back at ~2L however the line is configured. The
    // difference between two characters driven identically cancels that.
    const double time_ms = 400.0;
    // One settle duration for every character, long enough for the slowest
    // transport. It has to be IDENTICAL across characters, not merely
    // sufficient: a reversed segment is a cycle, so a different settle length
    // starts the burst at a different phase within it, and phase alone moves the
    // measured onset by up to a full segment — which would swamp the effect
    // under test.
    const double kSettleSeconds = 4.0 * (cd::kTimeSlewTapeMs * 0.001) + 0.5;
    auto onset_for = [&](Character character) {
        Engine delay;
        configure(delay, character, time_ms, 0.0, 0.5);
        delay.set_reverse(true);
        delay.reset();
        settle(delay, kSettleSeconds);

        auto buffers = burst_left(static_cast<int>(kSr * 3.0), 700.0, 0.01, 0.7f);
        render(delay, buffers);
        REQUIRE(all_finite(buffers.left));
        return onset_index(buffers.left, 0.05);
    };

    const int reference = onset_for(Character::clean);
    REQUIRE(reference > 0);

    for (auto character : {Character::bbd, Character::vintage_digital}) {
        const int measured = onset_for(character);
        REQUIRE(measured > 0);
        const double added_ms =
            1000.0 * static_cast<double>(measured - reference) / kSr;
        INFO("character index " << static_cast<int>(character) << " adds " << added_ms
                                << " ms over Clean; the pinned line is " << cd::kReverseLineMs
                                << " ms and the requested time is " << time_ms << " ms");
        // The character's own line adds about kReverseLineMs, not the requested
        // time. The generous upper bound is what distinguishes "short pinned
        // line" from "line still running at the requested time".
        CHECK(added_ms < 0.25 * time_ms);
    }
}

TEST_CASE("time offset scales every character's right-channel delay",
          "[character-delay][time]") {
    struct Case {
        Character character;
        double amount;
        bool relative_tolerance;
    };
    const Case cases[] = {
        {Character::clean, 0.0, false},
        {Character::diffusion, 0.5, false},
        {Character::tape, 0.0, false},
        {Character::bbd, 0.5, true},
        {Character::vintage_digital, 0.5, true},
    };

    for (const auto& c : cases) {
        for (double time_ms : {400.0, cd::kMaxDelayMs}) {
          for (double offset : {cd::kTimeOffsetMin, 1.0, cd::kTimeOffsetMax}) {
            Engine delay;
            configure(delay, c.character, time_ms, 0.0, c.amount);
            delay.set_time_offset(static_cast<float>(offset));
            delay.reset();
            settle(delay, 4.0 * slew_seconds(c.character) + 0.4);

            const double expected_ms = time_ms * offset;
            const int frames = static_cast<int>(kSr * (expected_ms * 0.001 + 0.3));
            auto buffers = burst_left(frames, 1000.0, 0.004, 0.5f);
            buffers.right = buffers.left;
            const int stimulus_onset = onset_index(buffers.right, 0.05);
            render(delay, buffers);

            const int measured = onset_index(buffers.right, 0.05) - stimulus_onset;
            const double expected = expected_ms * 0.001 * kSr;
            const double tolerance = c.relative_tolerance ? 0.02 * expected : 0.001 * kSr;
            INFO("character index " << static_cast<int>(c.character) << ", time " << time_ms
                                    << " ms, offset " << offset << ", onset " << measured
                                    << ", expected " << expected);
            REQUIRE(measured > 0);
            CHECK(std::abs(measured - expected) <= tolerance);
          }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 2 — Repeat decay
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("repeats decay at the feedback amount", "[character-delay][feedback]") {
    struct Case {
        Character character;
        double amount;
        double time_ms;
        double burst_seconds;
        double low;
        double high;
    };
    // The BBD's compander has a ~10 ms time constant, so its repeats have to be
    // long enough — and far enough apart — for the envelopes to reach the
    // operating region the device is designed around. Measuring its decay with
    // a 10 ms blip measures the compander's attack instead.
    const Case cases[] = {
        {Character::clean, 0.0, 100.0, 0.01, 0.48, 0.52},
        {Character::tape, 0.0, 100.0, 0.01, 0.40, 0.60},
        {Character::bbd, 0.5, 250.0, 0.08, 0.40, 0.60},
    };

    for (const auto& c : cases) {
        Engine delay;
        configure(delay, c.character, c.time_ms, 0.5, c.amount);
        settle(delay, 4.0 * slew_seconds(c.character) + 0.4);

        // A short burst rather than a bare impulse: the tape saturator and the
        // BBD compander are level-dependent, and an impulse's crest factor puts
        // both far outside the operating region a repeat actually sits in.
        // A moderate level, not a hot one. Both coloured characters compress
        // at high level by design (that IS the coloration), so measuring the
        // FEEDBACK law at the top of their range would be measuring the
        // saturator instead.
        auto buffers = burst_left(static_cast<int>(kSr * (c.time_ms * 0.001 * 6.0)), 1000.0,
                                  c.burst_seconds, 0.2f);
        render(delay, buffers);

        double previous = 0.0;
        for (int k = 1; k <= 5; ++k) {
            const int centre = static_cast<int>(k * c.time_ms * 0.001 * kSr);
            const int half = static_cast<int>(0.6 * c.burst_seconds * kSr) + 200;
            const double p = rms(buffers.left, centre - half, centre + half);
            if (k > 1) {
                const double ratio = p / std::max(previous, 1e-12);
                INFO("character index " << static_cast<int>(c.character) << ", repeat "
                                        << k << " ratio " << ratio);
                CHECK(ratio > c.low);
                CHECK(ratio < c.high);
            }
            previous = p;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 3 — Self-oscillation contract
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("saturating characters self-oscillate bounded at maximum feedback",
          "[character-delay][feedback][slow]") {
    struct Config {
        Character character;
        double amount;
        TapeTier tier;
    };
    std::vector<Config> configs{
        {Character::tape, 0.7, TapeTier::standard},
        {Character::bbd, 0.7, TapeTier::standard},
        {Character::vintage_digital, 0.7, TapeTier::standard},
    };
    for (double age : cd::kTapeAxis)
        configs.push_back({Character::tape, age, TapeTier::physical});
    for (const auto& config : configs) {
        Engine delay;
        configure(delay, config.character, 250.0, 1.1, config.amount, config.tier);

        // R4's protocol: a single small impulse, then a long zero-input settle.
        // The settle is 25 s rather than 10 because the BBD's clocked
        // band-limiting sheds energy on every pass, so its oscillation builds
        // far more slowly than tape's — which is the character behaving
        // correctly, not failing to oscillate.
        auto seed = make_stereo(static_cast<int>(kSr * 25.0));
        seed.left[0] = 0.01f;
        seed.right[0] = 0.01f;
        render(delay, seed);

        auto tail = make_stereo(static_cast<int>(kSr));
        render(delay, tail);

        const double level = rms(tail.left, 0, static_cast<int>(tail.left.size()));
        INFO("character index " << static_cast<int>(config.character) << ", tier "
                                << static_cast<int>(config.tier) << " rms " << level);
        CHECK(all_finite(tail.left));
        CHECK(level > 1e-3);
        CHECK(peak(tail.left, 0, static_cast<int>(tail.left.size())) < 4.0);
    }
}

TEST_CASE("physical tape keeps sub-unity feedback below oscillation",
          "[character-delay][feedback][tape][slow]") {
    const auto verify_decay = [](double feedback, double age) {
        Engine delay;
        configure(delay, Character::tape, 375.0, feedback, age, TapeTier::physical);

        // The worn physical loop can take more than 12 seconds to cross from
        // a quiet tail into oscillation, so observe the same 20-second horizon
        // used by the acceptance sweep rather than accepting an early lull.
        auto seed = make_stereo(static_cast<int>(kSr * 20.0));
        seed.left[0] = 0.3f;
        seed.right[0] = 0.3f;
        render(delay, seed);

        const double level = rms(seed.left, static_cast<int>(kSr * 18.0),
                                 static_cast<int>(seed.left.size()));
        INFO("feedback " << feedback << ", age " << age << " tail rms " << level);
        CHECK(all_finite(seed.left));
        CHECK(level < 2e-3);
    };

    // Acceptance points plus a no-feedback floor control.
    for (double feedback : {0.0, 0.72})
        for (double age : {0.72, 1.0})
            verify_decay(feedback, age);

    // The measured compensation is tabulated on this axis; pin every knot so
    // an intermediate calibration cannot drift while only the worn endpoint
    // remains green.
    for (double age : cd::kTapeAxis)
        verify_decay(0.9, age);
}

TEST_CASE("unsaturated characters always decay", "[character-delay][feedback][slow]") {
    // Clean and Diffusion have no in-loop saturator, so their feedback is
    // clamped below unity and the tail must die whatever the knob says.
    for (auto character : {Character::clean, Character::diffusion}) {
        Engine delay;
        configure(delay, character, 250.0, 1.1, 0.7);

        auto seed = make_stereo(static_cast<int>(kSr * 20.0));
        seed.left[0] = 0.01f;
        seed.right[0] = 0.01f;
        render(delay, seed);

        auto tail = make_stereo(static_cast<int>(kSr));
        render(delay, tail);

        const double level = rms(tail.left, 0, static_cast<int>(tail.left.size()));
        INFO("character index " << static_cast<int>(character) << " rms " << level);
        CHECK(all_finite(tail.left));
        CHECK(level < 1e-4);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 4 — Ping-pong
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("full crossfeed bounces repeats between channels", "[character-delay][stereo]") {
    Engine delay;
    configure(delay, Character::clean, 100.0, 0.7, 0.0);
    delay.set_crossfeed(1.0f);
    delay.reset();
    settle(delay, 0.2);

    auto buffers = impulse_left(static_cast<int>(kSr * 0.6));
    render(delay, buffers);

    for (int k = 1; k <= 4; ++k) {
        const int centre = static_cast<int>(k * 0.1 * kSr);
        const double left = peak(buffers.left, centre - 400, centre + 400);
        const double right = peak(buffers.right, centre - 400, centre + 400);
        const double dominant = (k % 2 == 1) ? left : right;
        const double quiet = (k % 2 == 1) ? right : left;
        INFO("repeat " << k << " L " << left << " R " << right);
        CHECK(dominant > 0.0);
        CHECK(20.0 * std::log10((quiet + 1e-15) / dominant) < -20.0);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 10 — Reverse
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("reverse plays each segment backwards without a splice click",
          "[character-delay][reverse]") {
    Engine delay;
    configure(delay, Character::clean, 500.0, 0.0, 0.0);
    delay.set_reverse(true);
    delay.reset();

    // A linear AMPLITUDE ramp on a tone: the envelope is what reversal negates,
    // and a bare DC ramp would be removed by the loop's 20 Hz DC blocker.
    const int n = static_cast<int>(kSr * 3.0);
    auto buffers = make_stereo(n);
    const int ramp_length = static_cast<int>(kSr);
    for (int i = 0; i < ramp_length; ++i) {
        const double envelope = static_cast<double>(i) / ramp_length;
        buffers.left[static_cast<std::size_t>(i)] = static_cast<float>(
            envelope * std::sin(2.0 * cd::kPi * 500.0 * static_cast<double>(i) / kSr));
    }
    render(delay, buffers);
    REQUIRE(all_finite(buffers.left));

    const int segment = static_cast<int>(0.5 * kSr);
    for (int index = 1; index <= 2; ++index) {
        const int start = index * segment;
        const double early = rms(buffers.left, start + 2000, start + 8000);
        const double late = rms(buffers.left, start + segment - 8000, start + segment - 2000);
        INFO("segment " << index << " early " << early << " late " << late);
        CHECK(late < early);  // the input's rising envelope comes back falling
    }

    // No splice click: the step across a boundary must stay within the range of
    // steps the signal itself produces inside a segment.
    const int boundary = 2 * segment;
    const double in_segment = max_step(buffers.left, boundary + 500, boundary + segment - 500);
    const double across = max_step(buffers.left, boundary - 40, boundary + 40);
    INFO("in-segment max step " << in_segment << ", across boundary " << across);
    CHECK(across <= 2.0 * in_segment);
}

TEST_CASE("reverse with feedback alternates direction", "[character-delay][reverse]") {
    Engine delay;
    configure(delay, Character::clean, 250.0, 0.7, 0.0);
    delay.set_reverse(true);
    delay.reset();

    auto buffers = burst_left(static_cast<int>(kSr * 3.0), 700.0, 0.05, 0.9f);
    render(delay, buffers);
    CHECK(all_finite(buffers.left));
    // Energy keeps circulating rather than stopping after one segment.
    CHECK(rms(buffers.left, static_cast<int>(kSr * 1.5), static_cast<int>(kSr * 2.5)) > 1e-4);
}

// ═══════════════════════════════════════════════════════════════════════════
// 11 — Freeze
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("freeze holds the loop and rejects new input", "[character-delay][freeze][slow]") {
    // The tone is pinned at 900 Hz on purpose. A frozen Clean loop's decay is
    // set by the in-loop 20 Hz DC-removal highpass, so the result depends
    // entirely on the test frequency: at a 250 ms delay that is ~120
    // recirculations in 30 s, over which a 220 Hz tone loses 4.2 dB and a 900 Hz
    // tone loses 0.3 dB — from identical, correct code. The criterion is only
    // reproducible with the tone well above the highpass corner.
    Engine delay;
    configure(delay, Character::clean, 250.0, 0.4, 0.0);

    auto priming = sine_both(static_cast<int>(kSr), 900.0, 0.5f);
    render(delay, priming);
    delay.set_freeze(true);

    auto first = make_stereo(static_cast<int>(kSr));
    render(delay, first);
    const double initial = rms(first.left, 0, static_cast<int>(first.left.size()));
    REQUIRE(initial > 1e-3);

    // (a) input rejection
    {
        Engine probe;
        configure(probe, Character::clean, 250.0, 0.4, 0.0);
        auto prime = sine_both(static_cast<int>(kSr), 900.0, 0.5f);
        render(probe, prime);
        probe.set_freeze(true);
        auto quiet = make_stereo(static_cast<int>(kSr * 2.0));
        render(probe, quiet);
        const double frozen = rms(quiet.left, static_cast<int>(kSr), static_cast<int>(kSr * 2.0));

        auto injected = sine_both(static_cast<int>(kSr * 2.0), 3100.0, 0.5f);
        render(probe, injected);
        const double injected_energy = magnitude_at(injected.left, static_cast<int>(kSr),
                                                    static_cast<int>(kSr * 2.0), 3100.0);
        INFO("frozen level " << frozen << ", injected tone " << injected_energy);
        CHECK(20.0 * std::log10((injected_energy + 1e-18) / frozen) < -60.0);
    }

    // (b) a frozen clean loop holds
    for (int second = 0; second < 29; ++second) {
        auto quiet = make_stereo(static_cast<int>(kSr));
        render(delay, quiet);
    }
    auto last = make_stereo(static_cast<int>(kSr));
    render(delay, last);
    const double held = rms(last.left, 0, static_cast<int>(last.left.size()));
    INFO("frozen clean loop " << initial << " -> " << held);
    CHECK(20.0 * std::log10((held + 1e-15) / initial) > -3.0);

    // (d) release is click free
    delay.set_freeze(false);
    auto release = make_stereo(static_cast<int>(kSr * 0.5));
    render(delay, release);
    const double inside = max_step(last.left, 1000, static_cast<int>(last.left.size()));
    INFO("release max step " << max_step(release.left, 0, 512) << " vs in-loop " << inside);
    CHECK(max_step(release.left, 0, 512) <= 2.0 * inside);
}

TEST_CASE("frozen coloured loops evolve but stay bounded",
          "[character-delay][freeze][slow]") {
    // Keeping the character INSIDE the frozen loop is the design decision: a
    // frozen tape or BBD loop degrades per pass into texture, which is the
    // feature. It must degrade without diverging.
    for (auto character : {Character::tape, Character::bbd}) {
        Engine delay;
        configure(delay, character, 250.0, 0.5, 0.6);
        auto priming = sine_both(static_cast<int>(kSr), 500.0, 0.5f);
        render(delay, priming);
        delay.set_freeze(true);

        for (int second = 0; second < 59; ++second) {
            auto quiet = make_stereo(static_cast<int>(kSr));
            render(delay, quiet);
        }
        auto last = make_stereo(static_cast<int>(kSr));
        render(delay, last);
        INFO("character index " << static_cast<int>(character));
        CHECK(all_finite(last.left));
        CHECK(peak(last.left, 0, static_cast<int>(last.left.size())) < 4.0);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 12 — Ducking
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("ducking pushes the wet path down under a hot input",
          "[character-delay][duck]") {
    auto measure = [](float duck, bool trailing_silence) {
        Engine delay;
        configure(delay, Character::clean, 300.0, 0.6, 0.0);
        delay.set_duck(duck);
        delay.reset();

        const int n = static_cast<int>(kSr * 3.0);
        auto buffers = sine_both(n, 300.0, 0.7f);
        if (trailing_silence)
            for (int i = static_cast<int>(kSr * 2.0); i < n; ++i) {
                buffers.left[static_cast<std::size_t>(i)] = 0.0f;
                buffers.right[static_cast<std::size_t>(i)] = 0.0f;
            }
        render(delay, buffers);
        return buffers;
    };

    const auto open = measure(0.0f, false);
    const auto ducked = measure(1.0f, false);
    const double open_level = rms(open.left, static_cast<int>(kSr), static_cast<int>(kSr * 2.0));
    const double ducked_level =
        rms(ducked.left, static_cast<int>(kSr), static_cast<int>(kSr * 2.0));
    INFO("open " << open_level << " ducked " << ducked_level);
    CHECK(20.0 * std::log10((ducked_level + 1e-18) / open_level) < -12.0);

    // Recovery means the FIRST point after which the wet path stays within
    // 1 dB, not merely a long average that can hide a late bad interval.
    const auto released = measure(1.0f, true);
    const auto undocked = measure(0.0f, true);
    constexpr double kWindowS = 0.02;
    const int window = static_cast<int>(kWindowS * kSr);
    const int stop = static_cast<int>(2.0 * kSr);
    const int end = static_cast<int>(2.9 * kSr);
    int last_bad_end = stop;
    for (int begin = stop; begin + window <= end; begin += window) {
        const double reference = rms(undocked.left, begin, begin + window);
        REQUIRE(reference > 1e-6);
        const double recovered = rms(released.left, begin, begin + window);
        const double difference_db =
            20.0 * std::log10((recovered + 1e-18) / reference);
        if (std::abs(difference_db) >= 1.0)
            last_bad_end = begin + window;
    }
    const double recovery_seconds = static_cast<double>(last_bad_end - stop) / kSr;
    INFO("stable duck recovery at " << recovery_seconds << " s; three tau is "
                                    << 3.0 * cd::kDuckReleaseS << " s");
    CHECK(recovery_seconds <= 3.0 * cd::kDuckReleaseS);
}

TEST_CASE("non-finite parameter writes preserve the last valid state",
          "[character-delay][api]") {
    auto set_valid_state = [](Engine& delay) {
        configure(delay, Character::tape, 437.0, 0.72, 0.83,
                  TapeTier::physical);
        delay.set_sample_rate(kSr);
        delay.set_tape_speed_ips(15.0f);
        delay.set_right_time_ms(611.0f);
        delay.set_crossfeed(0.41f);
        delay.set_diffusion_amount(0.29f);
        delay.set_mod(0.31f, 0.46f);
        delay.set_duck(0.37f);
        delay.set_loop_low_cut_hz(180.0f);
        delay.set_loop_low_cut_resonance(1.3f);
        delay.set_loop_high_cut_hz(7200.0f);
        delay.set_loop_high_cut_resonance(1.1f);
    };

    for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity()}) {
        Engine candidate;
        Engine reference;
        set_valid_state(candidate);
        set_valid_state(reference);

        auto before_bad_rate = sine_both(static_cast<int>(0.25 * kSr), 521.0, 0.19f);
        auto reference_before = before_bad_rate;
        render(candidate, before_bad_rate);
        render(reference, reference_before);
        REQUIRE(before_bad_rate.left == reference_before.left);
        candidate.set_sample_rate(bad);
        auto after_bad_rate = sine_both(static_cast<int>(0.25 * kSr), 521.0, 0.19f);
        auto reference_after = after_bad_rate;
        render(candidate, after_bad_rate);
        render(reference, reference_after);
        CHECK(after_bad_rate.left == reference_after.left);
        CHECK(after_bad_rate.right == reference_after.right);

        candidate.set_tape_speed_ips(static_cast<float>(bad));
        candidate.set_time_ms(static_cast<float>(bad));
        candidate.set_time_offset(static_cast<float>(bad));
        candidate.set_right_time_ms(static_cast<float>(bad));
        candidate.set_feedback(static_cast<float>(bad));
        candidate.set_crossfeed(static_cast<float>(bad));
        candidate.set_character_amount(static_cast<float>(bad));
        candidate.set_diffusion_amount(static_cast<float>(bad));
        candidate.set_duck(static_cast<float>(bad));
        candidate.set_loop_low_cut_hz(static_cast<float>(bad));
        candidate.set_loop_low_cut_resonance(static_cast<float>(bad));
        candidate.set_loop_high_cut_hz(static_cast<float>(bad));
        candidate.set_loop_high_cut_resonance(static_cast<float>(bad));

        // A bad component must not discard the valid component beside it.
        candidate.set_mod(static_cast<float>(bad), 0.73f);
        reference.set_mod(0.31f, 0.73f);
        candidate.set_mod(0.62f, static_cast<float>(bad));
        reference.set_mod(0.62f, 0.73f);

        candidate.reset();
        reference.reset();
        auto actual = sine_both(static_cast<int>(kSr * 1.5), 733.0, 0.27f);
        auto expected = actual;
        render(candidate, actual);
        render(reference, expected);
        INFO("invalid value " << bad);
        CHECK(all_finite(actual.left));
        CHECK(all_finite(actual.right));
        CHECK(actual.left == expected.left);
        CHECK(actual.right == expected.right);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 13 — Determinism
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("renders are bit-identical after reset", "[character-delay][determinism]") {
    for (auto character : {Character::tape, Character::bbd}) {
        Engine delay;
        configure(delay, character, 300.0, 0.5, 0.8);

        auto first = sine_both(static_cast<int>(kSr * 10.0), 440.0, 0.3f);
        render(delay, first);
        delay.reset();
        auto second = sine_both(static_cast<int>(kSr * 10.0), 440.0, 0.3f);
        render(delay, second);

        INFO("character index " << static_cast<int>(character));
        CHECK(first.left == second.left);
        CHECK(first.right == second.right);
    }
}

TEST_CASE("the physical tier's chew sequence repeats exactly",
          "[character-delay][determinism][tape]") {
    Engine delay;
    configure(delay, Character::tape, 300.0, 0.4, 1.0, TapeTier::physical);
    auto first = sine_both(static_cast<int>(kSr * 6.0), 440.0, 0.3f);
    render(delay, first);
    const std::size_t states = delay.chew_state_index(0);

    delay.reset();
    auto second = sine_both(static_cast<int>(kSr * 6.0), 440.0, 0.3f);
    render(delay, second);

    INFO("chew states " << states << " vs " << delay.chew_state_index(0));
    CHECK(delay.chew_state_index(0) == states);
    CHECK(first.left == second.left);
}

// ═══════════════════════════════════════════════════════════════════════════
// 14 — Real-time safety
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("process allocates nothing in any configuration",
          "[character-delay][rt-safety]") {
    for (auto character : {Character::clean, Character::vintage_digital, Character::tape,
                           Character::bbd, Character::diffusion}) {
        const int tier_count = character == Character::tape ? 2 : 1;
        for (int tier_index = 0; tier_index < tier_count; ++tier_index) {
            const auto tier = tier_index == 0 ? TapeTier::standard : TapeTier::physical;
            for (bool reverse : {false, true}) {
                for (bool freeze : {false, true}) {
                    Engine delay;
                    configure(delay, character, 250.0, 0.6, 0.7, tier);
                    delay.set_reverse(reverse);
                    delay.set_freeze(freeze);
                    delay.set_mod(0.4f, 0.5f);
                    delay.set_duck(0.5f);
                    delay.set_crossfeed(0.3f);
                    delay.set_diffusion_amount(0.6f);
                    delay.set_loop_low_cut_hz(120.0f);
                    delay.set_loop_low_cut_resonance(1.2f);
                    delay.set_loop_high_cut_hz(6000.0f);
                    delay.set_loop_high_cut_resonance(1.1f);
                    delay.reset();

                    auto warmup = sine_both(kBlock * 8, 500.0, 0.4f);
                    render(delay, warmup);

                    auto buffers = sine_both(kBlock * 8, 500.0, 0.4f);
                    {
                        pulp::test::RtAllocationProbe probe;
                        render(delay, buffers);
                        // reset() is on the allocation-free path too.
                        delay.reset();
                        // Snapshot before INFO: building Catch2's message stream allocates,
                        // and the probe is still in scope.
                        const auto count = probe.allocation_count();
                        const auto bytes = probe.allocated_bytes();
                        INFO("character index " << static_cast<int>(character) << " tier "
                                                << static_cast<int>(tier) << " reverse "
                                                << reverse << " freeze " << freeze);
                        CHECK(count == 0);
                        CHECK(bytes == 0);
                    }
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
TEST_CASE("the wet path stays inside a stated gain bound",
          "[character-delay][gain]") {
    // The bound a host or graph layer needs in order to size headroom, and the
    // reason it is NOT the usual 1/(1-feedback) geometric series:
    //
    //   * Tape, BBD and Vintage carry an in-loop saturator whose output is hard
    //     bounded to +-1 REGARDLESS of the feedback setting, so the loop
    //     contributes at most unity to the line input however far past 1.0 the
    //     feedback knob goes. The geometric series does not apply and would be
    //     unbounded at feedback 1.1.
    //   * Clean and Diffusion have no saturator, so their feedback IS clamped
    //     below unity and the geometric bound is the right one: at the 0.98
    //     ceiling a comb-resonant input can build to about 1/(1-0.98) = 50x.
    //     That is inherent to any feedback delay, not specific to this module.
    //
    // This measures the first case, which is the one a bound derived from the
    // feedback range alone would get badly wrong.
    for (auto character : {Character::tape, Character::bbd, Character::vintage_digital}) {
        Engine delay;
        configure(delay, character, 120.0, 1.1, 1.0);
        delay.set_crossfeed(0.5f);

        // Full-scale input, worst case, long enough for the loop to fill.
        auto buffers = sine_both(static_cast<int>(kSr * 8.0), 220.0, 1.0f);
        render(delay, buffers);

        const double top = peak(buffers.left, static_cast<int>(kSr * 2.0),
                                static_cast<int>(buffers.left.size()));
        INFO("character index " << static_cast<int>(character) << " peak " << top);
        CHECK(all_finite(buffers.left));
        CHECK(top < 4.0);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 17 — Latency
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("the module reports zero latency in every configuration",
          "[character-delay][latency]") {
    for (auto character : {Character::clean, Character::vintage_digital, Character::tape,
                           Character::bbd, Character::diffusion}) {
        for (auto tier : {TapeTier::standard, TapeTier::physical}) {
            Engine delay;
            configure(delay, character, 250.0, 0.3, 0.5, tier);
            CHECK(delay.latency_samples() == 0);
        }
    }

    // And it is a real zero, not a claim: the physical tier's in-loop
    // oversampler and loss FIR are folded out of the line, so the repeat still
    // lands on the requested time.
    Engine delay;
    configure(delay, Character::tape, 350.0, 0.0, 0.0, TapeTier::physical);
    settle(delay, 4.0 * slew_seconds(Character::tape) + 0.6);
    auto buffers = impulse_left(static_cast<int>(kSr * 0.9));
    render(delay, buffers);
    const int index = peak_index(buffers.left, 1, static_cast<int>(buffers.left.size()));
    INFO("physical-tier repeat at " << index << ", expected " << 0.35 * kSr);
    CHECK(std::abs(index - 0.35 * kSr) <= 0.001 * kSr);
}
