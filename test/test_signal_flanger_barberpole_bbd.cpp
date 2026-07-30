#include "test_signal_flanger_support.hpp"

#include <numbers>

TEST_CASE("R14 the float and double instantiations place the comb identically",
          "[signal][flanger]") {
    // The comb math is a single-tap feedback loop, not a cascade, so there is
    // no precision-sensitive recursion depth for the two to diverge over. A
    // divergence beyond this tolerance would mean a clamp-ordering or
    // interpolation bug, not expected drift.
    const double delay = kCenterMs;
    const double notch = Fl::notch_hz(0, delay, Polarity::positive);
    const double peak = 1.0 / (delay * 0.001);

    FlangerT<float> single;
    single.prepare(kSr);
    single.set_mode(Mode::classic);
    single.set_center_delay_ms(delay);
    single.set_depth_ms(0.0);
    single.set_feedback(0.5);
    single.set_mix(0.5);
    single.reset();

    const int n = kSettle + kAnalysisLen;
    std::vector<float> in(static_cast<std::size_t>(n)), out(static_cast<std::size_t>(n));
    auto measure = [&](double hz) {
        for (int k = 0; k < n; ++k)
            in[static_cast<std::size_t>(k)] = static_cast<float>(
                kProbeAmplitude * std::sin(2.0 * std::numbers::pi * hz * k / kSr));
        single.reset();
        single.process(in.data(), out.data(), n);
        std::vector<double> segment(out.begin() + kSettle, out.end());
        return magnitude_at(segment, hz) / kProbeAmplitude;
    };

    auto notch_double = held(delay, 0.5, Polarity::positive);
    auto peak_double = held(delay, 0.5, Polarity::positive);
    const double float_notch = measure(notch);
    const double float_peak = measure(peak);

    REQUIRE_THAT(float_peak, WithinRel(transfer(peak_double, peak), 0.01));
    REQUIRE_THAT(float_notch, WithinRel(transfer(notch_double, notch), 0.05));
    REQUIRE_THAT(float_peak, WithinRel(comb_magnitude(peak, delay, 0.5, Polarity::positive), 0.02));
}

TEST_CASE("barberpole shifts the wet path instead of sweeping it",
          "[signal][flanger]") {
    // The mechanism, measured directly rather than through the illusion it
    // produces. With the wet path frequency-shifted by Δf, a single input tone
    // comes out as TWO tones — the dry at f and the wet at f + Δf — and the
    // comb the pair forms drifts because their relative phase advances at Δf.
    // A delay-swept flanger cannot produce a second tone at all.
    constexpr double kTone = 500.0;
    constexpr double kShift = 3.0;
    constexpr int kLen = 48000;  // 1 s: 1 Hz bins, so every tone here is exact

    Fl f;
    f.prepare(kSr);
    f.set_mode(Mode::barberpole);
    f.set_center_delay_ms(kCenterMs);
    f.set_barberpole_shift_hz(kShift);
    f.set_feedback(0.0);
    f.set_mix(0.5);
    f.reset();

    std::vector<double> in(static_cast<std::size_t>(kLen + kSettle));
    std::vector<double> out(in.size());
    for (std::size_t k = 0; k < in.size(); ++k)
        in[k] = kProbeAmplitude *
                std::sin(2.0 * std::numbers::pi * kTone * static_cast<double>(k) / kSr);
    f.process(in.data(), out.data(), static_cast<int>(in.size()));
    const std::vector<double> segment(out.begin() + kSettle, out.end());

    double dry_gain = 0.0, wet_gain = 0.0;
    Fl::mix_gains(0.5, dry_gain, wet_gain);
    REQUIRE_THAT(magnitude_at(segment, kTone) / kProbeAmplitude, WithinRel(dry_gain, 0.02));
    REQUIRE_THAT(magnitude_at(segment, kTone + kShift) / kProbeAmplitude,
                 WithinRel(wet_gain, 0.02));
    // Single sideband: the shift goes one way only, which is what makes the
    // drift monotonic rather than a symmetric warble.
    REQUIRE(magnitude_at(segment, kTone - kShift) / kProbeAmplitude < 0.02 * wet_gain);
}

TEST_CASE("barberpole feedback stacks the shift into an endless spiral",
          "[signal][flanger]") {
    // Why the shifter sits INSIDE the loop: every recirculation adds another
    // Δf, so energy appears at f + 2Δf, f + 3Δf … and climbs without ever
    // landing back where it started. That staircase is the barberpole illusion,
    // and it is the one thing a feedback path wired AROUND the shifter could
    // not produce.
    constexpr double kTone = 500.0;
    constexpr double kShift = 3.0;
    constexpr int kLen = 48000;

    auto render = [&](double feedback) {
        Fl f;
        f.prepare(kSr);
        f.set_mode(Mode::barberpole);
        f.set_center_delay_ms(kCenterMs);
        f.set_barberpole_shift_hz(kShift);
        f.set_feedback(feedback);
        f.set_mix(0.5);
        f.reset();
        std::vector<double> in(static_cast<std::size_t>(kLen + kSettle));
        std::vector<double> out(in.size());
        for (std::size_t k = 0; k < in.size(); ++k)
            in[k] = kProbeAmplitude *
                    std::sin(2.0 * std::numbers::pi * kTone * static_cast<double>(k) / kSr);
        f.process(in.data(), out.data(), static_cast<int>(in.size()));
        return std::vector<double>(out.begin() + kSettle, out.end());
    };

    const auto dry = render(0.0);
    const auto spiral = render(0.7);
    for (int pass = 2; pass <= 4; ++pass) {
        const double hz = kTone + pass * kShift;
        INFO("pass " << pass << " at " << hz << " Hz");
        REQUIRE(magnitude_at(spiral, hz) > 20.0 * magnitude_at(dry, hz));
        REQUIRE(magnitude_at(spiral, hz) > 0.01 * kProbeAmplitude);
    }
    // Monotone decay up the staircase: each pass is quieter than the last.
    for (int pass = 2; pass <= 4; ++pass)
        REQUIRE(magnitude_at(spiral, kTone + pass * kShift) <
                magnitude_at(spiral, kTone + (pass - 1) * kShift));
}

TEST_CASE("a negative barberpole shift descends instead of climbing",
          "[signal][flanger]") {
    constexpr double kTone = 2000.0;
    constexpr double kShift = -4.0;
    constexpr int kLen = 48000;

    Fl f;
    f.prepare(kSr);
    f.set_mode(Mode::barberpole);
    f.set_barberpole_shift_hz(kShift);
    f.set_feedback(0.0);
    f.set_mix(0.5);
    f.reset();
    std::vector<double> in(static_cast<std::size_t>(kLen + kSettle));
    std::vector<double> out(in.size());
    for (std::size_t k = 0; k < in.size(); ++k)
        in[k] = kProbeAmplitude *
                std::sin(2.0 * std::numbers::pi * kTone * static_cast<double>(k) / kSr);
    f.process(in.data(), out.data(), static_cast<int>(in.size()));
    const std::vector<double> segment(out.begin() + kSettle, out.end());

    double dry_gain = 0.0, wet_gain = 0.0;
    Fl::mix_gains(0.5, dry_gain, wet_gain);
    REQUIRE_THAT(magnitude_at(segment, kTone + kShift) / kProbeAmplitude,
                 WithinRel(wet_gain, 0.02));
    REQUIRE(magnitude_at(segment, kTone - kShift) / kProbeAmplitude < 0.02 * wet_gain);
}

TEST_CASE("the BBD engine darkens the wet path and stays bounded",
          "[signal][flanger]") {
    // The character swap, measured against the thing that makes a BBD a BBD:
    // its usable bandwidth is tied to its clock, so the wet path loses treble
    // that the clean line keeps. Asserted as a ratio between the two engines at
    // the same setting, so it is a statement about the engine rather than about
    // any absolute filter shape.
    // Probed quietly. A BBD is a companded device, so its gain is
    // level-dependent BY DESIGN — measured here it passes 0.997 at −60 dBFS,
    // 0.85 at −20 dBFS and 0.28 at full scale. A bandwidth claim measured at a
    // hot level would be measuring the compander instead.
    constexpr double kQuiet = 0.01;
    auto wet_at = [](Engine engine, double hz) {
        Fl f;
        f.prepare(kSr);
        f.set_mode(Mode::classic);
        f.set_delay_engine(engine);
        f.set_center_delay_ms(kCenterMs);
        f.set_depth_ms(0.0);
        f.set_feedback(0.0);
        f.set_mix(1.0);  // wet only: the engine's own response, nothing else
        f.reset();
        return transfer(f, hz, 24000, kQuiet);
    };

    // The treble probe sits at 1.5x the shipped bandwidth law rather than at a
    // round 12 kHz, for a measured reason. 12 kHz is exactly kSr/4, and this
    // engine carries a waveshaper and a compander, so at simple submultiples of
    // the sample rate the odd alias products fold back onto the probe's own
    // bin and inflate the reading: the response sweeps smoothly 0.688 (10 kHz),
    // 0.626 (11 kHz), 0.491 (13 kHz) but reads 0.775 at 12 kHz — a 2 dB spike
    // that is the measurement, not the device. Tying the probe to the shipped
    // constant keeps it both non-degenerate and meaningful.
    const double bandwidth_law = chardelay::kBbdBandwidthMaxHz;
    const double treble_hz = 1.5 * bandwidth_law;

    const double clean_low = wet_at(Engine::clean, 300.0);
    const double bbd_low = wet_at(Engine::bbd, 300.0);
    const double clean_high = wet_at(Engine::clean, treble_hz);
    const double bbd_high = wet_at(Engine::bbd, treble_hz);

    // The clean line is allpass: unity at both ends.
    REQUIRE_THAT(clean_low, WithinRel(1.0, 0.02));
    REQUIRE_THAT(clean_high, WithinRel(1.0, 0.02));
    // The BBD passes the low end and rolls off the top — the clock-tied
    // bandwidth that is the device's signature, stated as a ratio against its
    // OWN low end so it is a claim about the engine rather than about any
    // absolute filter shape.
    REQUIRE(bbd_low > 0.8);
    REQUIRE(bbd_high < 0.5 * bbd_low);

    // And it is still bounded in the loop at the feedback ceiling — the engine
    // that carries a waveshaper and a compander is the one most worth checking.
    Fl loop;
    loop.prepare(kSr);
    loop.set_mode(Mode::classic);
    loop.set_delay_engine(Engine::bbd);
    loop.set_center_delay_ms(kCenterMs);
    loop.set_depth_ms(kDepthMs);
    loop.set_feedback(Fl::kFbClamp);
    loop.set_mix(1.0);
    loop.reset();
    double worst = 0.0;
    bool finite = true;
    const int n = static_cast<int>(5.0 * kSr);
    for (int k = 0; k < n; ++k) {
        const double x = k < 4800 ? 0.5 * std::sin(2.0 * std::numbers::pi * 500.0 * k / kSr) : 0.0;
        double y = 0.0;
        loop.process(&x, &y, 1);
        finite = finite && std::isfinite(y);
        worst = std::max(worst, std::abs(y));
    }
    REQUIRE(finite);
    REQUIRE(worst <= Fl::worst_case_gain());
}
