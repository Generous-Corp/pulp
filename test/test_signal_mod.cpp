// Modulation toolkit: deterministic randomness, the LFO, the small control
// tools, unit conversions, and the chaos source.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/chaos.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/mod_tools.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::vector<float> run_lfo(Lfo& lfo, int n) {
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out.push_back(lfo.next());
    return out;
}

} // namespace

TEST_CASE("modulation scalar aliases start fresh and track their double forms",
          "[signal][mod][parity][zero-init]") {
    STATIC_REQUIRE(std::is_same_v<OuWalk64, OuWalkT<double>>);
    STATIC_REQUIRE(std::is_same_v<Drift64, DriftT<double>>);
    STATIC_REQUIRE(std::is_same_v<Lfo64, LfoT<double>>);
    STATIC_REQUIRE(std::is_same_v<SlewLimiter64, SlewLimiterT<double>>);
    STATIC_REQUIRE(std::is_same_v<SampleHold64, SampleHoldT<double>>);
    STATIC_REQUIRE(std::is_same_v<Attenuverter64, AttenuverterT<double>>);
    STATIC_REQUIRE(std::is_same_v<Rectifier64, RectifierT<double>>);
    STATIC_REQUIRE(std::is_same_v<Comparator64, ComparatorT<double>>);
    STATIC_REQUIRE(std::is_same_v<Quantizer64, QuantizerT<double>>);
    STATIC_REQUIRE(std::is_same_v<Curve64, CurveT<double>>);
    STATIC_REQUIRE(std::is_same_v<LogisticMap64, LogisticMapT<double>>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<OuWalk64&>().next()), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Drift64&>().fraction()), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Lfo64&>().next()), double>);
    STATIC_REQUIRE(std::is_same_v<
                   decltype(std::declval<SlewLimiter64&>().process(0.0)), double>);
    STATIC_REQUIRE(std::is_same_v<
                   decltype(std::declval<SampleHold64&>().process(0.0, false)), double>);
    STATIC_REQUIRE(std::is_same_v<
                   decltype(std::declval<Attenuverter64&>().process(0.0)), double>);
    STATIC_REQUIRE(std::is_same_v<
                   decltype(std::declval<Rectifier64&>().process(0.0)), double>);
    STATIC_REQUIRE(std::is_same_v<
                   decltype(std::declval<Quantizer64&>().process(0.0)), double>);
    STATIC_REQUIRE(std::is_same_v<
                   decltype(std::declval<Curve64&>().process(0.0)), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<LogisticMap64&>().next()), double>);
    OuWalk walk; OuWalk64 walk64;
    walk.prepare(1500.0); walk64.prepare(1500.0);
    REQUIRE_THAT(static_cast<double>(walk.next()), WithinAbs(walk64.next(), 1.0e-6));

    Drift drift; Drift64 drift64;
    drift.prepare(48000.0); drift64.prepare(48000.0);
    drift.next(); drift64.next();
    REQUIRE_THAT(static_cast<double>(drift.pitch_factor()),
                 WithinAbs(drift64.pitch_factor(), 1.0e-6));
    REQUIRE_THAT(static_cast<double>(drift.fraction()),
                 WithinAbs(drift64.fraction(), 1.0e-6));

    Lfo lfo; Lfo64 lfo64;
    lfo.prepare(48000.0); lfo64.prepare(48000.0);
    lfo.set_rate_hz(7.25); lfo64.set_rate_hz(7.25);
    lfo.set_wave(Lfo::Wave::triangle); lfo64.set_wave(Lfo64::Wave::triangle);
    for (int i = 0; i < 64; ++i)
        REQUIRE_THAT(static_cast<double>(lfo.next()), WithinAbs(lfo64.next(), 1.0e-6));

    SlewLimiter slew; SlewLimiter64 slew64;
    slew.prepare(48000.0f); slew64.prepare(48000.0);
    REQUIRE_THAT(static_cast<double>(slew.process(1.0f)),
                 WithinAbs(slew64.process(1.0), 1.0e-6));

    SampleHold hold; SampleHold64 hold64;
    hold.prepare(48000.0f); hold64.prepare(48000.0);
    REQUIRE_THAT(static_cast<double>(hold.process(0.75f, true)),
                 WithinAbs(hold64.process(0.75, true), 1.0e-6));

    Attenuverter attenuverter; Attenuverter64 attenuverter64;
    Rectifier rectifier; Rectifier64 rectifier64;
    Comparator comparator; Comparator64 comparator64;
    Quantizer quantizer; Quantizer64 quantizer64;
    Curve curve; Curve64 curve64;
    REQUIRE_THAT(static_cast<double>(attenuverter.process(0.25f)),
                 WithinAbs(attenuverter64.process(0.25), 1.0e-6));
    REQUIRE_THAT(static_cast<double>(rectifier.process(-0.25f)),
                 WithinAbs(rectifier64.process(-0.25), 1.0e-6));
    REQUIRE(comparator.process(0.75f) == comparator64.process(0.75));
    REQUIRE_THAT(static_cast<double>(quantizer.process(0.37f)),
                 WithinAbs(quantizer64.process(0.37), 1.0e-6));
    REQUIRE_THAT(static_cast<double>(curve.process(0.37f)),
                 WithinAbs(curve64.process(0.37), 1.0e-6));

    LogisticMap chaos; LogisticMap64 chaos64;
    REQUIRE_THAT(static_cast<double>(chaos.next()), WithinAbs(chaos64.next(), 1.0e-6));

    lfo.reset(); lfo64.reset();
    slew.reset(); slew64.reset();
    hold.reset(); hold64.reset();
    chaos.reset(); chaos64.reset();
    REQUIRE_THAT(static_cast<double>(lfo.next()), WithinAbs(lfo64.next(), 1.0e-6));
    REQUIRE_THAT(static_cast<double>(slew.process(0.5f)),
                 WithinAbs(slew64.process(0.5), 1.0e-6));
    REQUIRE_THAT(static_cast<double>(hold.process(0.5f, true)),
                 WithinAbs(hold64.process(0.5, true), 1.0e-6));
    REQUIRE_THAT(static_cast<double>(chaos.next()), WithinAbs(chaos64.next(), 1.0e-6));
}

TEST_CASE("modulation scalar raw value-initialized state replays after reset",
          "[signal][mod][zero-init]") {
    OuWalk walk; OuWalk64 walk64;
    const auto walk_first = walk.next();
    const auto walk64_first = walk64.next();
    walk.reset(); walk64.reset();
    REQUIRE(walk.next() == walk_first);
    REQUIRE(walk64.next() == walk64_first);

    Drift drift; Drift64 drift64;
    drift.next(); drift64.next();
    const auto drift_first = drift.fraction();
    const auto drift64_first = drift64.fraction();
    drift.reset(); drift64.reset();
    drift.next(); drift64.next();
    REQUIRE(drift.fraction() == drift_first);
    REQUIRE(drift64.fraction() == drift64_first);

    Lfo lfo; Lfo64 lfo64;
    const auto lfo_first = lfo.next();
    const auto lfo64_first = lfo64.next();
    lfo.reset(); lfo64.reset();
    REQUIRE(lfo.next() == lfo_first);
    REQUIRE(lfo64.next() == lfo64_first);

    SlewLimiter slew; SlewLimiter64 slew64;
    const auto slew_first = slew.process(0.75f);
    const auto slew64_first = slew64.process(0.75);
    slew.reset(); slew64.reset();
    REQUIRE(slew.process(0.75f) == slew_first);
    REQUIRE(slew64.process(0.75) == slew64_first);

    SampleHold hold; SampleHold64 hold64;
    const auto hold_first = hold.process(0.75f, true);
    const auto hold64_first = hold64.process(0.75, true);
    hold.reset(); hold64.reset();
    REQUIRE(hold.process(0.75f, true) == hold_first);
    REQUIRE(hold64.process(0.75, true) == hold64_first);

    Comparator comparator; Comparator64 comparator64;
    const bool comparator_first = comparator.process(0.75f);
    const bool comparator64_first = comparator64.process(0.75);
    comparator.reset(); comparator64.reset();
    REQUIRE(comparator.process(0.75f) == comparator_first);
    REQUIRE(comparator64.process(0.75) == comparator64_first);

    LogisticMap chaos; LogisticMap64 chaos64;
    const auto chaos_first = chaos.next();
    const auto chaos64_first = chaos64.next();
    chaos.reset(); chaos64.reset();
    REQUIRE(chaos.next() == chaos_first);
    REQUIRE(chaos64.next() == chaos64_first);
}

// ── rng ──────────────────────────────────────────────────────────────────────

TEST_CASE("Xorshift32 is bit-deterministic per seed", "[signal][mod][rng]") {
    Xorshift32 a(12345u);
    Xorshift32 b(12345u);
    Xorshift32 c(12346u);

    bool diverged = false;
    for (int i = 0; i < 4096; ++i) {
        const std::uint32_t va = a.next_u32();
        REQUIRE(va == b.next_u32());
        if (va != c.next_u32()) diverged = true;
    }
    REQUIRE(diverged);
}

TEST_CASE("Xorshift32 repairs a zero state instead of stalling", "[signal][mod][rng]") {
    Xorshift32 zeroed(0u);
    const std::uint32_t first = zeroed.next_u32();
    REQUIRE(first != 0u);
    REQUIRE(zeroed.next_u32() != first);

    // A default-constructed generator produces the same stream as an explicitly
    // zero-seeded one, so a zero-initialized aggregate is a usable generator.
    Xorshift32 fresh;
    Xorshift32 explicit_default(Xorshift32::kDefaultSeed);
    REQUIRE(fresh.next_u32() == explicit_default.next_u32());
}

TEST_CASE("Reset rewinds to the caller's seed, not the default one",
          "[signal][mod][rng]") {
    Xorshift32 rng(4242u);
    const std::uint32_t first = rng.next_u32();
    for (int i = 0; i < 100; ++i) (void)rng.next_u32();
    rng.reset();
    REQUIRE(rng.next_u32() == first);
    // The control: the default stream is a different stream, so the assertion
    // above cannot be passing by accident.
    Xorshift32 defaulted;
    REQUIRE(defaulted.next_u32() != first);

    OuWalk walk;
    walk.prepare(1000.0);
    walk.set_theta(1.0);
    walk.set_sigma(0.5);
    walk.seed(4242u);
    walk.reset();
    const float walk_first = walk.next();
    for (int i = 0; i < 100; ++i) (void)walk.next();
    walk.reset();
    REQUIRE(walk.next() == walk_first);

    OuWalk other;
    other.prepare(1000.0);
    other.set_theta(1.0);
    other.set_sigma(0.5);
    other.seed(9999u);
    other.reset();
    REQUIRE(other.next() != walk_first);
}

TEST_CASE("Re-seeding discards the cached gaussian deviate", "[signal][mod][rng]") {
    // Box-Muller produces two deviates per pair of uniforms and caches the
    // second. A re-seed that left the cache holding it would hand back a value
    // from the stream the caller just replaced.
    Xorshift32 a(11u);
    (void)a.gaussian();      // draws two, returns one, caches the other
    a.seed(22u);

    Xorshift32 fresh(22u);
    REQUIRE(a.gaussian() == fresh.gaussian());
    REQUIRE(a.gaussian() == fresh.gaussian());

    // Control: without the re-seed the two streams differ, so the equality
    // above is not passing by coincidence.
    Xorshift32 unseeded(11u);
    (void)unseeded.gaussian();
    Xorshift32 other(22u);
    REQUIRE(unseeded.gaussian() != other.gaussian());
}

TEST_CASE("Xorshift32 unipolar and bipolar draws stay in range", "[signal][mod][rng]") {
    Xorshift32 rng(7u);
    for (int i = 0; i < 100000; ++i) {
        const float u = rng.next_unipolar();
        REQUIRE(u >= 0.0f);
        REQUIRE(u < 1.0f);
        const float b = rng.next_bipolar();
        REQUIRE(b >= -1.0f);
        REQUIRE(b < 1.0f);
    }
}

TEST_CASE("Box-Muller gaussian moments hold to 2 percent", "[signal][mod][rng]") {
    Xorshift32 rng(99u);
    constexpr int kDraws = 1000000;
    double sum = 0.0;
    double sum_sq = 0.0;
    for (int i = 0; i < kDraws; ++i) {
        const double g = rng.gaussian();
        sum += g;
        sum_sq += g * g;
    }
    const double mean = sum / kDraws;
    const double variance = sum_sq / kDraws - mean * mean;
    REQUIRE_THAT(mean, WithinAbs(0.0, 0.02));
    REQUIRE_THAT(std::sqrt(variance), WithinRel(1.0, 0.02));
}

TEST_CASE("mix64 and unit_from are stateless and well distributed", "[signal][mod][rng]") {
    // mix64 is a bijection, so it fixes zero; rng_key offsets by the golden
    // constant precisely so the natural first key does not land there.
    REQUIRE(mix64(0ull) == 0ull);
    REQUIRE(mix64(1ull) != mix64(2ull));
    REQUIRE(rng_key(0ull, 0ull) != 0ull);
    REQUIRE(unit_from(rng_key(0ull, 0ull)) > 0.0f);

    // Purpose-keyed draws are independent of request order: the same key gives
    // the same value however many other keys were drawn in between.
    const float first = unit_from(rng_key(0xABCDull, 7ull));
    for (int i = 0; i < 100; ++i) (void)unit_from(rng_key(0xABCDull, static_cast<std::uint64_t>(i)));
    REQUIRE(unit_from(rng_key(0xABCDull, 7ull)) == first);

    double sum = 0.0;
    constexpr int kDraws = 100000;
    for (int i = 0; i < kDraws; ++i) {
        const float u = unit_from(rng_key(1ull, static_cast<std::uint64_t>(i)));
        REQUIRE(u >= 0.0f);
        REQUIRE(u < 1.0f);
        sum += u;
    }
    REQUIRE_THAT(sum / kDraws, WithinAbs(0.5, 0.01));
}

TEST_CASE("OuWalk stationary std matches sigma over sqrt(2 theta)", "[signal][mod][rng]") {
    OuWalk walk;
    walk.prepare(1000.0);
    walk.set_theta(4.0);
    walk.set_sigma(0.5);
    walk.seed(2024u);
    walk.reset();

    // Burn in past the transient from starting at the mean.
    for (int i = 0; i < 20000; ++i) (void)walk.next();

    constexpr int kSamples = 400000;
    double sum = 0.0;
    double sum_sq = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        const double y = walk.next();
        sum += y;
        sum_sq += y * y;
    }
    const double mean = sum / kSamples;
    const double measured = std::sqrt(sum_sq / kSamples - mean * mean);
    REQUIRE_THAT(measured, WithinRel(walk.stationary_std(), 0.10));
}

TEST_CASE("OuWalk is bit-deterministic and bounded", "[signal][mod][rng]") {
    OuWalk a;
    OuWalk b;
    for (auto* w : {&a, &b}) {
        w->prepare(48000.0);
        w->set_theta(1.0);
        w->set_sigma(20.0); // large enough to exercise the clamp
        w->seed(5u);
        w->reset();
    }
    for (int i = 0; i < 10000; ++i) {
        const float x = a.next();
        REQUIRE(x == b.next());
        REQUIRE(std::abs(x) <= OuWalk::kClamp);
    }
}

TEST_CASE("Drift interpolates between control points", "[signal][mod][rng]") {
    Drift drift;
    drift.prepare(48000.0);
    drift.set_cents(50.0);
    drift.set_theta(2.0);
    drift.set_sigma(1.0);
    drift.seed(11u);
    drift.reset();

    float previous = drift.fraction();
    float largest_step = 0.0f;
    for (int i = 0; i < 48000; ++i) {
        drift.next();
        const float value = drift.fraction();
        largest_step = std::max(largest_step, std::abs(value - previous));
        previous = value;
        REQUIRE(std::isfinite(drift.pitch_factor()));
    }
    // A staircase would step by the full control-point delta in one sample.
    // Interpolated, each sample moves at most 1/kDecimation of that.
    REQUIRE(largest_step < 0.05f);
}

TEST_CASE("Drift pitch factor is unity at zero cents", "[signal][mod][rng]") {
    Drift drift;
    drift.prepare(48000.0);
    drift.set_cents(0.0);
    drift.reset();
    for (int i = 0; i < 1000; ++i) {
        drift.next();
        REQUIRE_THAT(drift.pitch_factor(), WithinAbs(1.0f, 1e-6f));
    }
}

TEST_CASE("Drift fraction honours the unit contract at extreme sigma",
          "[signal][mod][rng]") {
    Drift drift;
    drift.prepare(48000.0);
    drift.set_theta(1.0);
    drift.set_sigma(4.0); // legal, and enough to saturate the walk's clamp
    drift.seed(1234u);
    drift.reset();
    float peak = 0.0f;
    for (int i = 0; i < 480000; ++i) {
        drift.next();
        peak = std::max(peak, std::abs(drift.fraction()));
    }
    REQUIRE(peak <= 1.0f);
}

TEST_CASE("OuWalk stays stable when theta exceeds the update rate",
          "[signal][mod][rng]") {
    // Naive Euler drift (theta * dt) overshoots the mean once theta * dt
    // passes 1 and oscillates clamp-to-clamp past 2. The walk applies the
    // exact per-step decay factor, so any theta is a hard pull toward the
    // mean, never an oscillator — including at control rates as low as
    // DriftT's at an 8 kHz session.
    OuWalk walk;
    walk.prepare(250.0);
    walk.set_theta(600.0); // theta * dt = 2.4
    walk.set_sigma(0.001);
    walk.seed(1u);
    walk.reset();
    float peak = 0.0f;
    for (int i = 0; i < 200; ++i) peak = std::max(peak, std::abs(walk.next()));
    REQUIRE(peak < 0.05f);
}

// ── lfo: rate ────────────────────────────────────────────────────────────────

TEST_CASE("Lfo frequency is accurate to 0.01 percent", "[signal][mod][lfo]") {
    constexpr double kSampleRate = 48000.0;
    constexpr double kRate = 5.0;
    constexpr int kSeconds = 100;
    constexpr int kSamples = static_cast<int>(kSampleRate) * kSeconds;

    Lfo lfo;
    lfo.prepare(kSampleRate);
    lfo.set_rate_hz(kRate);

    int upward_crossings = 0;
    float previous = -1.0f;
    for (int i = 0; i < kSamples; ++i) {
        const float value = lfo.next();
        if (previous < 0.0f && value >= 0.0f) ++upward_crossings;
        previous = value;
    }

    const double total_cycles =
        static_cast<double>(lfo.cycles_completed()) + lfo.phase();
    REQUIRE_THAT(total_cycles, WithinRel(kRate * kSeconds, 1e-4));
    // One upward crossing per cycle, plus or minus the one at the boundary.
    REQUIRE(upward_crossings >= 500);
    REQUIRE(upward_crossings <= 501);
}

TEST_CASE("Lfo period_samples round-trips through set_period_samples",
          "[signal][mod][lfo]") {
    Lfo lfo;
    lfo.prepare(48000.0);
    lfo.set_period_samples(9600.0);
    REQUIRE_THAT(lfo.period_samples(), WithinRel(9600.0, 1e-9));
    REQUIRE_THAT(lfo.rate_hz(), WithinRel(5.0, 1e-9));
}

TEST_CASE("Lfo rate survives a prepare at a lower sample rate",
          "[signal][mod][lfo]") {
    Lfo lfo;
    lfo.prepare(8000.0);
    lfo.set_rate_hz(6000.0);                 // clamped to 0.45 * 8000 = 3600
    REQUIRE_THAT(lfo.rate_hz(), WithinRel(3600.0, 1e-9));
    lfo.prepare(96000.0);                    // 6000 Hz is legal again
    REQUIRE_THAT(lfo.rate_hz(), WithinRel(6000.0, 1e-9));
}

TEST_CASE("Lfo stays bounded and finite at audio rate", "[signal][mod][lfo]") {
    Lfo lfo;
    lfo.prepare(48000.0);
    lfo.set_rate_hz(20000.0);
    for (int i = 0; i < 100000; ++i) {
        const float value = lfo.next();
        REQUIRE(std::isfinite(value));
        REQUIRE(std::abs(value) <= 1.0f);
    }
}

TEST_CASE("Lfo cycle counter is wide enough for a day-long free run",
          "[signal][mod][lfo]") {
    // At the 0.45 * sample_rate rate ceiling a free-running LFO completes
    // ~86400 cycles per second at 192 kHz; a 32-bit counter overflows —
    // undefined behavior — in under seven hours of playback.
    Lfo lfo;
    REQUIRE(sizeof(decltype(lfo.cycles_completed())) >= 8);
}

// ── lfo: shape ───────────────────────────────────────────────────────────────

TEST_CASE("Lfo morph endpoints bit-match the pure waves", "[signal][mod][lfo]") {
    const struct {
        float morph;
        Lfo::Wave wave;
    } kEndpoints[] = {
        {0.0f, Lfo::Wave::sine},
        {1.0f, Lfo::Wave::triangle},
        {2.0f, Lfo::Wave::saw_up},
        {3.0f, Lfo::Wave::square},
    };

    for (const auto& endpoint : kEndpoints) {
        Lfo morphed;
        Lfo pure;
        for (auto* lfo : {&morphed, &pure}) {
            lfo->prepare(48000.0);
            lfo->set_rate_hz(3.0);
        }
        morphed.set_shape_morph(endpoint.morph);
        pure.set_wave(endpoint.wave);

        for (int i = 0; i < 32000; ++i) REQUIRE(morphed.next() == pure.next());
    }
}

TEST_CASE("Lfo pure waveforms hit their defining points", "[signal][mod][lfo]") {
    // One cycle per 4 samples makes the phase land exactly on 0, 0.25, 0.5,
    // and 0.75, so the shapes can be checked against their closed forms.
    auto sample_cycle = [](Lfo::Wave wave) {
        Lfo lfo;
        lfo.prepare(4.0);
        lfo.set_period_samples(4.0);
        lfo.set_wave(wave);
        return run_lfo(lfo, 4);
    };

    const auto sine = sample_cycle(Lfo::Wave::sine);
    REQUIRE_THAT(sine[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(sine[1], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(sine[3], WithinAbs(-1.0f, 1e-6f));

    const auto triangle = sample_cycle(Lfo::Wave::triangle);
    REQUIRE_THAT(triangle[0], WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(triangle[2], WithinAbs(1.0f, 1e-6f));

    const auto saw_up = sample_cycle(Lfo::Wave::saw_up);
    REQUIRE_THAT(saw_up[0], WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(saw_up[2], WithinAbs(0.0f, 1e-6f));

    const auto saw_down = sample_cycle(Lfo::Wave::saw_down);
    REQUIRE_THAT(saw_down[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(saw_down[2], WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("Lfo pulse width sets the square duty cycle", "[signal][mod][lfo]") {
    Lfo lfo;
    lfo.prepare(48000.0);
    lfo.set_period_samples(1000.0);
    lfo.set_wave(Lfo::Wave::square);
    lfo.set_pulse_width(0.25f);

    int high = 0;
    for (int i = 0; i < 1000; ++i)
        if (lfo.next() > 0.0f) ++high;
    REQUIRE(high == 250);
}

TEST_CASE("Lfo triangle bias skews the peak position", "[signal][mod][lfo]") {
    Lfo lfo;
    lfo.prepare(48000.0);
    lfo.set_period_samples(1000.0);
    lfo.set_wave(Lfo::Wave::triangle);
    lfo.set_tri_bias(0.5f); // peak at 0.75 of the cycle

    const auto cycle = run_lfo(lfo, 1000);
    const auto peak = std::max_element(cycle.begin(), cycle.end());
    REQUIRE(std::distance(cycle.begin(), peak) == 750);
    REQUIRE_THAT(*peak, WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("Lfo random blend crossfades toward the held random value",
          "[signal][mod][lfo]") {
    Lfo blended;
    Lfo dry;
    Lfo wet;
    for (auto* lfo : {&blended, &dry, &wet}) {
        lfo->prepare(48000.0);
        lfo->set_period_samples(500.0);
        lfo->set_seed(4242u);
        lfo->reset();
    }
    blended.set_random_blend(0.5f);
    wet.set_wave(Lfo::Wave::sh_random);

    for (int i = 0; i < 5000; ++i) {
        const float mixed = blended.next();
        const float expected = 0.5f * dry.next() + 0.5f * wet.next();
        REQUIRE_THAT(mixed, WithinAbs(expected, 1e-6f));
    }
}

// ── lfo: phase, stereo, quadrature ───────────────────────────────────────────

TEST_CASE("Lfo stereo channels are identical at zero offset", "[signal][mod][lfo]") {
    for (auto wave : {Lfo::Wave::sine, Lfo::Wave::square, Lfo::Wave::sh_random,
                      Lfo::Wave::smooth_random}) {
        Lfo lfo;
        lfo.prepare(48000.0);
        lfo.set_period_samples(700.0);
        lfo.set_wave(wave);
        lfo.set_stereo_offset(0.0f);
        for (int i = 0; i < 5000; ++i) {
            float left = 0.0f;
            float right = 0.0f;
            lfo.next_stereo(left, right);
            REQUIRE(left == right);
        }
    }
}

TEST_CASE("Lfo stereo offset shifts the right channel by a whole cycle fraction",
          "[signal][mod][lfo]") {
    Lfo lfo;
    lfo.prepare(48000.0);
    lfo.set_period_samples(1000.0);
    lfo.set_stereo_offset(0.25f);

    std::vector<float> left(2000);
    std::vector<float> right(2000);
    for (int i = 0; i < 2000; ++i) lfo.next_stereo(left[static_cast<std::size_t>(i)],
                                                   right[static_cast<std::size_t>(i)]);

    // A quarter-cycle offset at a 1000-sample period is a 250-sample lead.
    for (int i = 0; i < 1000; ++i) {
        REQUIRE_THAT(right[static_cast<std::size_t>(i)],
                     WithinAbs(left[static_cast<std::size_t>(i + 250)], 1e-5f));
    }
}

TEST_CASE("Lfo quadrature pair is orthogonal", "[signal][mod][lfo]") {
    Lfo lfo;
    lfo.prepare(48000.0);
    lfo.set_period_samples(1024.0);

    double correlation = 0.0;
    for (int i = 0; i < 4096; ++i) {
        float s = 0.0f;
        float c = 0.0f;
        lfo.next_quadrature(s, c);
        REQUIRE_THAT(s * s + c * c, WithinAbs(1.0f, 1e-5f));
        correlation += static_cast<double>(s) * static_cast<double>(c);
    }
    REQUIRE_THAT(correlation / 4096.0, WithinAbs(0.0, 1e-6));
}

TEST_CASE("Lfo phase offset rotates the output", "[signal][mod][lfo]") {
    Lfo plain;
    Lfo offset;
    for (auto* lfo : {&plain, &offset}) {
        lfo->prepare(48000.0);
        lfo->set_period_samples(800.0);
    }
    offset.set_phase_offset(0.5f);

    const auto a = run_lfo(plain, 1600);
    const auto b = run_lfo(offset, 1600);
    for (int i = 0; i < 800; ++i) {
        REQUIRE_THAT(b[static_cast<std::size_t>(i)],
                     WithinAbs(a[static_cast<std::size_t>(i + 400)], 1e-5f));
    }
}

// ── lfo: lifecycle ───────────────────────────────────────────────────────────

TEST_CASE("Lfo delay and fade stage boundaries are sample exact",
          "[signal][mod][lfo]") {
    Lfo lfo;
    lfo.prepare(48000.0);
    lfo.set_rate_hz(1.0);
    lfo.set_delay_ms(10.0);   // 480 samples
    lfo.set_fade_in_ms(20.0); // 960 samples
    lfo.reset();

    for (int i = 0; i < 480; ++i) {
        REQUIRE(lfo.stage() == Lfo::Stage::delay);
        REQUIRE(lfo.next() == 0.0f);
    }
    REQUIRE(lfo.stage() == Lfo::Stage::fade_in);
    // Phase must not have moved during the delay: the wave starts at its start.
    REQUIRE(lfo.phase() == 0.0);

    for (int i = 0; i < 960; ++i) {
        REQUIRE(lfo.stage() == Lfo::Stage::fade_in);
        (void)lfo.next();
    }
    REQUIRE(lfo.stage() == Lfo::Stage::sustain);
}

TEST_CASE("Lfo fade in ramps depth from zero to one", "[signal][mod][lfo]") {
    Lfo lfo;
    lfo.prepare(48000.0);
    lfo.set_period_samples(4.0); // peak once every 4 samples
    lfo.set_fade_in_ms(20.0);    // 960 samples
    lfo.reset();

    // Sample index 1 of each 4-sample cycle is the sine peak, so the output
    // there is exactly the fade envelope.
    (void)lfo.next();
    const float first_peak = lfo.next(); // sample index 1
    for (int i = 0; i < 475; ++i) (void)lfo.next();
    const float mid_peak = lfo.next(); // sample index 477, just under half the fade

    REQUIRE(first_peak < 0.01f);
    REQUIRE_THAT(mid_peak, WithinAbs(0.5f, 0.02f));
}

TEST_CASE("Lfo repeat count stops after N cycles", "[signal][mod][lfo]") {
    Lfo lfo;
    lfo.prepare(48000.0);
    lfo.set_period_samples(1000.0);
    lfo.set_repeat_count(3);
    lfo.reset();

    for (int i = 0; i < 3000 - 1; ++i) {
        (void)lfo.next();
        REQUIRE_FALSE(lfo.finished());
    }
    (void)lfo.next();
    REQUIRE(lfo.finished());
    REQUIRE(lfo.cycles_completed() == 3);
    for (int i = 0; i < 100; ++i) REQUIRE(lfo.next() == 0.0f);
}

TEST_CASE("Lfo repeat plus fade out is a shaped one-shot", "[signal][mod][lfo]") {
    Lfo lfo;
    lfo.prepare(48000.0);
    lfo.set_period_samples(1000.0);
    lfo.set_repeat_count(1);
    lfo.set_fade_out_ms(10.0, true); // 480 samples
    lfo.reset();

    for (int i = 0; i < 1000; ++i) (void)lfo.next();
    REQUIRE(lfo.stage() == Lfo::Stage::fade_out);
    for (int i = 0; i < 480; ++i) (void)lfo.next();
    REQUIRE(lfo.finished());
}

TEST_CASE("Lfo free mode ignores retrigger, retrig mode honours it",
          "[signal][mod][lfo]") {
    Lfo running;
    running.prepare(48000.0);
    running.set_period_samples(1000.0);
    running.set_mode(Lfo::Mode::free);
    for (int i = 0; i < 250; ++i) (void)running.next();
    const double before = running.phase();
    running.retrigger();
    REQUIRE(running.phase() == before);

    Lfo retriggered;
    retriggered.prepare(48000.0);
    retriggered.set_period_samples(1000.0);
    retriggered.set_mode(Lfo::Mode::retrig);
    for (int i = 0; i < 250; ++i) (void)retriggered.next();
    REQUIRE(retriggered.phase() > 0.0);
    retriggered.retrigger();
    REQUIRE(retriggered.phase() == 0.0);
}

TEST_CASE("Lfo one shot completes a single cycle without a repeat count",
          "[signal][mod][lfo]") {
    Lfo lfo;
    lfo.prepare(48000.0);
    lfo.set_period_samples(500.0);
    lfo.set_mode(Lfo::Mode::one_shot);
    lfo.reset();
    lfo.retrigger();

    for (int i = 0; i < 500; ++i) (void)lfo.next();
    REQUIRE(lfo.finished());
}

// ── lfo: random determinism ──────────────────────────────────────────────────

TEST_CASE("Lfo random waveforms are bit-deterministic per seed",
          "[signal][mod][lfo]") {
    for (auto wave : {Lfo::Wave::sh_random, Lfo::Wave::smooth_random}) {
        for (int segments : {1, 4}) {
            Lfo a;
            Lfo b;
            for (auto* lfo : {&a, &b}) {
                lfo->prepare(48000.0);
                lfo->set_period_samples(300.0);
                lfo->set_wave(wave);
                lfo->set_random_segments(segments);
                lfo->set_seed(31337u);
                lfo->reset();
            }
            bool moved = false;
            for (int i = 0; i < 6000; ++i) {
                const float va = a.next();
                REQUIRE(va == b.next());
                if (va != 0.0f) moved = true;
            }
            REQUIRE(moved);
        }
    }
}

TEST_CASE("Lfo sample-and-hold random holds one value per cycle",
          "[signal][mod][lfo]") {
    Lfo lfo;
    lfo.prepare(48000.0);
    lfo.set_period_samples(100.0);
    lfo.set_wave(Lfo::Wave::sh_random);
    lfo.set_seed(8u);
    lfo.reset();

    const auto values = run_lfo(lfo, 500);
    for (int cycle = 0; cycle < 5; ++cycle) {
        const float held = values[static_cast<std::size_t>(cycle * 100)];
        for (int i = 1; i < 100; ++i)
            REQUIRE(values[static_cast<std::size_t>(cycle * 100 + i)] == held);
    }
    REQUIRE(values[0] != values[100]);
}

TEST_CASE("Lfo N-segment smooth random latches N targets per cycle",
          "[signal][mod][lfo]") {
    Lfo one;
    Lfo four;
    for (auto* lfo : {&one, &four}) {
        lfo->prepare(48000.0);
        lfo->set_period_samples(400.0);
        lfo->set_wave(Lfo::Wave::smooth_random);
        lfo->set_seed(77u);
    }
    four.set_random_segments(Lfo::kDefaultRandomSegments);
    one.reset();
    four.reset();

    auto direction_changes = [](const std::vector<float>& v) {
        int changes = 0;
        for (std::size_t i = 2; i < v.size(); ++i) {
            const float d0 = v[i - 1] - v[i - 2];
            const float d1 = v[i] - v[i - 1];
            if (d0 * d1 < 0.0f) ++changes;
        }
        return changes;
    };

    const auto slow = run_lfo(one, 4000);
    const auto fast = run_lfo(four, 4000);
    REQUIRE(direction_changes(fast) > direction_changes(slow));
}

// ── small tools ──────────────────────────────────────────────────────────────

TEST_CASE("SlewLimiter linear mode takes exactly the set ramp time",
          "[signal][mod][tools]") {
    SlewLimiter slew;
    slew.prepare(48000.0f);
    slew.set_mode(SlewLimiter::Mode::linear);
    slew.set_times_ms(10.0f, 20.0f); // 480 up, 960 down
    slew.reset(0.0f);

    for (int i = 0; i < 479; ++i) (void)slew.process(1.0f);
    REQUIRE(slew.current() < 1.0f);
    REQUIRE_THAT(slew.process(1.0f), WithinAbs(1.0f, 1e-5f));

    for (int i = 0; i < 959; ++i) (void)slew.process(0.0f);
    REQUIRE(slew.current() > 0.0f);
    REQUIRE_THAT(slew.process(0.0f), WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("SlewLimiter exponential mode reaches one time constant",
          "[signal][mod][tools]") {
    SlewLimiter slew;
    slew.prepare(48000.0f);
    slew.set_mode(SlewLimiter::Mode::exponential);
    slew.set_times_ms(10.0f, 10.0f);
    slew.reset(0.0f);

    for (int i = 0; i < 480; ++i) (void)slew.process(1.0f);
    REQUIRE_THAT(slew.current(), WithinAbs(0.632f, 0.005f));
}

TEST_CASE("SlewLimiter keeps moving when the step is below float precision",
          "[signal][mod][tools]") {
    // A two-minute linear slew at 384 kHz steps ~2.2e-8 per sample — less
    // than half an ulp of 0.5f, which a float accumulator absorbs, stalling
    // the value forever. The limiter accumulates in double, so the ramp
    // keeps its documented travel time at any rate and length.
    SlewLimiter slew;
    slew.prepare(384000.0f);
    slew.set_mode(SlewLimiter::Mode::linear);
    slew.set_times_ms(120000.0f, 120000.0f);
    slew.reset(0.5f);
    for (int i = 0; i < 1000000; ++i) (void)slew.process(1.0f);
    // One million samples of a 46.08-million-sample full-span ramp.
    REQUIRE_THAT(slew.current(), WithinAbs(0.5f + 1.0e6f / 46.08e6f, 2.0e-4f));
}

TEST_CASE("SampleHold latches on the clock's rising edge", "[signal][mod][tools]") {
    SampleHold hold;
    hold.prepare(48000.0f);
    hold.reset(0.0f);

    REQUIRE_THAT(hold.process(0.5f, true), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(hold.process(0.9f, true), WithinAbs(0.5f, 1e-6f)); // level, not edge
    REQUIRE_THAT(hold.process(0.9f, false), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(hold.process(0.9f, true), WithinAbs(0.9f, 1e-6f)); // new edge
}

TEST_CASE("SampleHold glide ramps to the latched value", "[signal][mod][tools]") {
    SampleHold hold;
    hold.prepare(48000.0f);
    hold.set_glide_ms(10.0f); // 480 samples
    hold.reset(0.0f);

    const float first = hold.process(1.0f, true);
    REQUIRE(first < 1.0f);
    REQUIRE(hold.held() == 1.0f);
    for (int i = 0; i < 480; ++i) (void)hold.process(1.0f, false);
    REQUIRE_THAT(hold.process(1.0f, false), WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("Attenuverter inverts and re-centres", "[signal][mod][tools]") {
    Attenuverter attenuverter;
    attenuverter.set_gain(-0.8f);
    attenuverter.set_offset(1.0f);
    REQUIRE_THAT(attenuverter.process(0.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(attenuverter.process(1.0f), WithinAbs(0.2f, 1e-6f));

    attenuverter.set_gain(10.0f); // clamped
    REQUIRE_THAT(attenuverter.gain(), WithinAbs(Attenuverter::kMaxGain, 1e-6f));
}

TEST_CASE("Bipolar and unipolar helpers round-trip", "[signal][mod][tools]") {
    for (float x = -1.0f; x <= 1.0f; x += 0.05f)
        REQUIRE_THAT(uni_to_bi(bi_to_uni(x)), WithinAbs(x, 1e-6f));
}

TEST_CASE("Rectifier folds or gates the negative half", "[signal][mod][tools]") {
    Rectifier rectifier;
    rectifier.set_mode(Rectifier::Mode::full_wave);
    REQUIRE_THAT(rectifier.process(-0.7f), WithinAbs(0.7f, 1e-6f));
    rectifier.set_mode(Rectifier::Mode::half_wave);
    REQUIRE_THAT(rectifier.process(-0.7f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(rectifier.process(0.7f), WithinAbs(0.7f, 1e-6f));
}

TEST_CASE("Comparator hysteresis suppresses chatter", "[signal][mod][tools]") {
    Comparator comparator;
    comparator.set_threshold(0.5f); // default hysteresis 0.025
    REQUIRE_THAT(comparator.hysteresis(), WithinAbs(0.025f, 1e-6f));

    REQUIRE_FALSE(comparator.process(0.51f)); // inside the dead band
    REQUIRE(comparator.process(0.53f));
    REQUIRE(comparator.process(0.49f)); // still inside, stays high
    REQUIRE_FALSE(comparator.process(0.47f));

    comparator.set_hysteresis(0.0f);
    REQUIRE(comparator.process(0.51f));
}

TEST_CASE("Comparator keeps a dead band at the default threshold of zero",
          "[signal][mod][tools]") {
    // A bipolar signal's natural threshold is 0, where a proportional-only
    // auto hysteresis would derive a dead band of 0 — the exact chatter the
    // class exists to prevent.
    Comparator comparator;
    REQUIRE(comparator.hysteresis() > 0.0f);

    Xorshift32 rng(7u);
    int transitions = 0;
    bool prev = false;
    for (int i = 0; i < 480000; ++i) {
        // 0.5 Hz sine plus a sliver of noise, as any real mod signal carries.
        const float x =
            std::sin(static_cast<float>(2.0 * 3.14159265358979 * 0.5 * i / 48000.0))
            + 2.0e-4f * rng.next_bipolar();
        const bool gate = comparator.process(x);
        if (gate != prev) ++transitions;
        prev = gate;
    }
    REQUIRE(transitions == 10); // two clean crossings per cycle, five cycles
}

TEST_CASE("Quantizer snaps to N equally spaced levels", "[signal][mod][tools]") {
    Quantizer quantizer;
    quantizer.set_range(0.0f, 1.0f);
    quantizer.set_steps(5); // 0, 0.25, 0.5, 0.75, 1

    REQUIRE_THAT(quantizer.process(0.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(quantizer.process(0.3f), WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(quantizer.process(0.4f), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(quantizer.process(0.375f), WithinAbs(0.5f, 1e-6f)); // ties round up
    REQUIRE_THAT(quantizer.process(1.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(quantizer.process(5.0f), WithinAbs(1.0f, 1e-6f)); // clamps
    REQUIRE_THAT(quantizer.process(-5.0f), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("Stage curve law matches its closed form", "[signal][mod][tools]") {
    for (float curve = -1.0f; curve <= 1.0f; curve += 0.25f) {
        REQUIRE_THAT(stage_curve(0.0f, curve), WithinAbs(0.0f, 1e-6f));
        REQUIRE_THAT(stage_curve(1.0f, curve), WithinAbs(1.0f, 1e-6f));
    }
    // Linear limit.
    for (float p = 0.0f; p <= 1.0f; p += 0.1f)
        REQUIRE_THAT(stage_curve(p, 0.0f), WithinAbs(p, 1e-6f));

    // (1 - e^-4) / (1 - e^-8) at p = 0.5, curve = +1.
    const float expected = (1.0f - std::exp(-4.0f)) / (1.0f - std::exp(-8.0f));
    REQUIRE_THAT(stage_curve(0.5f, 1.0f), WithinAbs(expected, 1e-6f));
}

TEST_CASE("Rise and fall curves both call plus one exponential",
          "[signal][mod][tools]") {
    // An exponential attack is slow to leave zero...
    REQUIRE(curve_rise(0.5f, 1.0f) < 0.5f);
    // ...and an exponential decay drops fast and then tails.
    REQUIRE(curve_fall(0.5f, 1.0f) < 0.5f);
    // The logarithmic sign is the mirror of each.
    REQUIRE(curve_rise(0.5f, -1.0f) > 0.5f);
    REQUIRE(curve_fall(0.5f, -1.0f) > 0.5f);

    // Endpoints are exact for every curve.
    for (float c = -1.0f; c <= 1.0f; c += 0.5f) {
        REQUIRE_THAT(curve_rise(0.0f, c), WithinAbs(0.0f, 1e-6f));
        REQUIRE_THAT(curve_rise(1.0f, c), WithinAbs(1.0f, 1e-6f));
        REQUIRE_THAT(curve_fall(0.0f, c), WithinAbs(1.0f, 1e-6f));
        REQUIRE_THAT(curve_fall(1.0f, c), WithinAbs(0.0f, 1e-6f));
    }
}

TEST_CASE("Curve shaper follows the rise law and smoothstep",
          "[signal][mod][tools]") {
    Curve curve;
    curve.set_shape(Curve::Shape::stage_curve);
    curve.set_curve(1.0f);
    REQUIRE_THAT(curve.process(0.5f), WithinAbs(curve_rise(0.5f, 1.0f), 1e-6f));

    curve.set_shape(Curve::Shape::smoothstep);
    REQUIRE_THAT(curve.process(0.5f), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(curve.process(0.25f), WithinAbs(smoothstep(0.25f), 1e-6f));
    REQUIRE_THAT(curve.process(0.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(curve.process(1.0f), WithinAbs(1.0f, 1e-6f));
}

// ── units ────────────────────────────────────────────────────────────────────

TEST_CASE("Unit conversions round-trip", "[signal][mod][units]") {
    using namespace pulp::signal::units;

    for (float db = -60.0f; db <= 12.0f; db += 6.0f)
        REQUIRE_THAT(linear_to_db(db_to_linear(db)), WithinAbs(db, 1e-3f));

    for (float note = 21.0f; note <= 108.0f; note += 12.0f)
        REQUIRE_THAT(hz_to_midi(midi_to_hz(note)), WithinAbs(note, 1e-3f));

    REQUIRE_THAT(midi_to_hz(69.0f), WithinAbs(440.0f, 1e-3f));
    REQUIRE_THAT(semitones_to_ratio(12.0f), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(cents_to_ratio(1200.0f), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(ratio_to_cents(cents_to_ratio(37.0f)), WithinAbs(37.0f, 1e-3f));

    for (float t = 0.0f; t <= 1.0f; t += 0.125f)
        REQUIRE_THAT(taper_log_inverse(taper_log(t, 0.01f, 20.0f), 0.01f, 20.0f),
                     WithinAbs(t, 1e-5f));

    REQUIRE_THAT(beats_to_seconds(4.0f, 120.0f), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(seconds_to_beats(2.0f, 120.0f), WithinAbs(4.0f, 1e-6f));
}

TEST_CASE("One-pole and T60 helpers match their definitions",
          "[signal][mod][units]") {
    using namespace pulp::signal::units;

    // A one-pole driven with its own coefficient reaches 63.2% in one time
    // constant.
    const float coefficient = ms_to_onepole_coef(10.0f, 48000.0f);
    float y = 0.0f;
    for (int i = 0; i < 480; ++i) y += coefficient * (1.0f - y);
    REQUIRE_THAT(y, WithinAbs(0.632f, 0.005f));

    // A feedback path at the T60 gain is 60 dB down after T60 seconds.
    const float gain = t60_to_per_sample_gain(0.5f, 48000.0f);
    float amplitude = 1.0f;
    for (int i = 0; i < 24000; ++i) amplitude *= gain;
    REQUIRE_THAT(linear_to_db(amplitude), WithinAbs(-60.0f, 0.1f));
}

TEST_CASE("Musical division table is order-locked and correct",
          "[signal][mod][units]") {
    using namespace pulp::signal::units;

    REQUIRE_THAT(division_to_beats(Division::whole), WithinAbs(4.0f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::half), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::quarter), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::eighth), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::sixteenth), WithinAbs(0.25f, 1e-6f));

    REQUIRE_THAT(division_to_beats(Division::quarter_dotted), WithinAbs(1.5f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::eighth_triplet),
                 WithinAbs(1.0f / 3.0f, 1e-6f));
    // Three triplet eighths fill one beat: the property the shared table exists
    // to make every synced object agree on.
    REQUIRE_THAT(3.0f * division_to_beats(Division::eighth_triplet),
                 WithinAbs(1.0f, 1e-5f));

    // Index overload addresses the same rows.
    for (int i = 0; i < kDivisionCount; ++i)
        REQUIRE(division_to_beats(i) == division_to_beats(static_cast<Division>(i)));

    // Monotone decreasing across straight note values.
    REQUIRE(division_to_beats(Division::sixty_fourth)
            < division_to_beats(Division::thirty_second));

    // A synced LFO period lands where the tempo says it should.
    REQUIRE_THAT(division_to_samples(Division::quarter, 120.0f, 48000.0),
                 WithinAbs(24000.0, 1e-3));
}

// ── chaos ────────────────────────────────────────────────────────────────────

TEST_CASE("LogisticMap is deterministic and stays in the unit interval",
          "[signal][mod][chaos]") {
    LogisticMap a;
    LogisticMap b;
    for (auto* map : {&a, &b}) {
        map->set_r(4.0);
        map->seed(0.31415);
        map->reset();
    }
    for (int i = 0; i < 100000; ++i) {
        const float x = a.next();
        REQUIRE(x == b.next());
        REQUIRE(x > 0.0f);
        REQUIRE(x < 1.0f);
    }
}

TEST_CASE("LogisticMap settles to a four-cycle at r = 3.5",
          "[signal][mod][chaos]") {
    LogisticMap map;
    map.set_r(3.5);
    map.seed(0.4);
    map.reset();
    for (int i = 0; i < 20000; ++i) (void)map.next();

    float orbit[4];
    for (float& value : orbit) value = map.next();
    for (int repeat = 0; repeat < 20; ++repeat)
        for (float value : orbit) REQUIRE_THAT(map.next(), WithinAbs(value, 1e-4f));
}

TEST_CASE("LogisticMap seed applies without an explicit reset",
          "[signal][mod][chaos]") {
    // Same immediate-application semantics as Xorshift32::seed(); reset()
    // rewinds to the seed rather than being the only way to apply it.
    LogisticMap seeded;
    seeded.seed(0.123);
    LogisticMap reference;
    reference.seed(0.123);
    reference.reset();
    for (int i = 0; i < 100; ++i) REQUIRE(seeded.next() == reference.next());
}

TEST_CASE("LogisticMap bipolar output spans the range", "[signal][mod][chaos]") {
    LogisticMap map;
    map.set_r(4.0);
    map.seed(0.2);
    map.reset();
    float lowest = 1.0f;
    float highest = -1.0f;
    for (int i = 0; i < 10000; ++i) {
        const float x = map.next_bipolar();
        lowest = std::min(lowest, x);
        highest = std::max(highest, x);
    }
    REQUIRE(lowest < -0.9f);
    REQUIRE(highest > 0.9f);
}
