// Cross-cutting alias, precision, fresh-state, and reset contracts for the
// modulation toolkit.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/chaos.hpp>
#include <pulp/signal/envelope.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/lpg.hpp>
#include <pulp/signal/mod_matrix.hpp>
#include <pulp/signal/mod_tools.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/trigger.hpp>
#include <pulp/signal/vca.hpp>

#include <array>
#include <cmath>
#include <span>
#include <type_traits>
#include <utility>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kSampleRate = 48000.0;

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
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<SlewLimiter64&>().process(0.0)), double>);
    STATIC_REQUIRE(
        std::is_same_v<decltype(std::declval<SampleHold64&>().process(0.0, false)), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Attenuverter64&>().process(0.0)), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Rectifier64&>().process(0.0)), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Quantizer64&>().process(0.0)), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Curve64&>().process(0.0)), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<LogisticMap64&>().next()), double>);

    OuWalk walk;
    OuWalk64 walk64;
    walk.prepare(1500.0);
    walk64.prepare(1500.0);
    REQUIRE_THAT(static_cast<double>(walk.next()), WithinAbs(walk64.next(), 1.0e-6));

    Drift drift;
    Drift64 drift64;
    drift.prepare(kSampleRate);
    drift64.prepare(kSampleRate);
    drift.next();
    drift64.next();
    REQUIRE_THAT(static_cast<double>(drift.pitch_factor()),
                 WithinAbs(drift64.pitch_factor(), 1.0e-6));
    REQUIRE_THAT(static_cast<double>(drift.fraction()), WithinAbs(drift64.fraction(), 1.0e-6));

    Lfo lfo;
    Lfo64 lfo64;
    lfo.prepare(kSampleRate);
    lfo64.prepare(kSampleRate);
    lfo.set_rate_hz(7.25);
    lfo64.set_rate_hz(7.25);
    lfo.set_wave(Lfo::Wave::triangle);
    lfo64.set_wave(Lfo64::Wave::triangle);
    for (int i = 0; i < 64; ++i)
        REQUIRE_THAT(static_cast<double>(lfo.next()), WithinAbs(lfo64.next(), 1.0e-6));

    SlewLimiter slew;
    SlewLimiter64 slew64;
    slew.prepare(static_cast<float>(kSampleRate));
    slew64.prepare(kSampleRate);
    REQUIRE_THAT(static_cast<double>(slew.process(1.0f)), WithinAbs(slew64.process(1.0), 1.0e-6));

    SampleHold hold;
    SampleHold64 hold64;
    hold.prepare(static_cast<float>(kSampleRate));
    hold64.prepare(kSampleRate);
    REQUIRE_THAT(static_cast<double>(hold.process(0.75f, true)),
                 WithinAbs(hold64.process(0.75, true), 1.0e-6));

    Attenuverter attenuverter;
    Attenuverter64 attenuverter64;
    Rectifier rectifier;
    Rectifier64 rectifier64;
    Comparator comparator;
    Comparator64 comparator64;
    Quantizer quantizer;
    Quantizer64 quantizer64;
    Curve curve;
    Curve64 curve64;
    REQUIRE_THAT(static_cast<double>(attenuverter.process(0.25f)),
                 WithinAbs(attenuverter64.process(0.25), 1.0e-6));
    REQUIRE_THAT(static_cast<double>(rectifier.process(-0.25f)),
                 WithinAbs(rectifier64.process(-0.25), 1.0e-6));
    REQUIRE(comparator.process(0.75f) == comparator64.process(0.75));
    REQUIRE_THAT(static_cast<double>(quantizer.process(0.37f)),
                 WithinAbs(quantizer64.process(0.37), 1.0e-6));
    REQUIRE_THAT(static_cast<double>(curve.process(0.37f)),
                 WithinAbs(curve64.process(0.37), 1.0e-6));

    LogisticMap chaos;
    LogisticMap64 chaos64;
    REQUIRE_THAT(static_cast<double>(chaos.next()), WithinAbs(chaos64.next(), 1.0e-6));

    lfo.reset();
    lfo64.reset();
    slew.reset();
    slew64.reset();
    hold.reset();
    hold64.reset();
    chaos.reset();
    chaos64.reset();
    REQUIRE_THAT(static_cast<double>(lfo.next()), WithinAbs(lfo64.next(), 1.0e-6));
    REQUIRE_THAT(static_cast<double>(slew.process(0.5f)), WithinAbs(slew64.process(0.5), 1.0e-6));
    REQUIRE_THAT(static_cast<double>(hold.process(0.5f, true)),
                 WithinAbs(hold64.process(0.5, true), 1.0e-6));
    REQUIRE_THAT(static_cast<double>(chaos.next()), WithinAbs(chaos64.next(), 1.0e-6));
}

TEST_CASE("modulation scalar raw value-initialized state replays after reset",
          "[signal][mod][zero-init]") {
    OuWalk walk;
    OuWalk64 walk64;
    const auto walk_first = walk.next();
    const auto walk64_first = walk64.next();
    walk.reset();
    walk64.reset();
    REQUIRE(walk.next() == walk_first);
    REQUIRE(walk64.next() == walk64_first);

    Drift drift;
    Drift64 drift64;
    drift.next();
    drift64.next();
    const auto drift_first = drift.fraction();
    const auto drift64_first = drift64.fraction();
    drift.reset();
    drift64.reset();
    drift.next();
    drift64.next();
    REQUIRE(drift.fraction() == drift_first);
    REQUIRE(drift64.fraction() == drift64_first);

    Lfo lfo;
    Lfo64 lfo64;
    const auto lfo_first = lfo.next();
    const auto lfo64_first = lfo64.next();
    lfo.reset();
    lfo64.reset();
    REQUIRE(lfo.next() == lfo_first);
    REQUIRE(lfo64.next() == lfo64_first);

    SlewLimiter slew;
    SlewLimiter64 slew64;
    const auto slew_first = slew.process(0.75f);
    const auto slew64_first = slew64.process(0.75);
    slew.reset();
    slew64.reset();
    REQUIRE(slew.process(0.75f) == slew_first);
    REQUIRE(slew64.process(0.75) == slew64_first);

    SampleHold hold;
    SampleHold64 hold64;
    const auto hold_first = hold.process(0.75f, true);
    const auto hold64_first = hold64.process(0.75, true);
    hold.reset();
    hold64.reset();
    REQUIRE(hold.process(0.75f, true) == hold_first);
    REQUIRE(hold64.process(0.75, true) == hold64_first);

    Comparator comparator;
    Comparator64 comparator64;
    const bool comparator_first = comparator.process(0.75f);
    const bool comparator64_first = comparator64.process(0.75);
    comparator.reset();
    comparator64.reset();
    REQUIRE(comparator.process(0.75f) == comparator_first);
    REQUIRE(comparator64.process(0.75) == comparator64_first);

    LogisticMap chaos;
    LogisticMap64 chaos64;
    const auto chaos_first = chaos.next();
    const auto chaos64_first = chaos64.next();
    chaos.reset();
    chaos64.reset();
    REQUIRE(chaos.next() == chaos_first);
    REQUIRE(chaos64.next() == chaos64_first);
}

TEST_CASE("event and envelope aliases start fresh and track their double forms",
          "[signal][mod][parity][zero-init]") {
    STATIC_REQUIRE(std::is_same_v<TriggerDetect64, TriggerDetectT<double>>);
    STATIC_REQUIRE(std::is_same_v<GateGen64, GateGenT<double>>);
    STATIC_REQUIRE(std::is_same_v<ClockDivider64, ClockDividerT>);
    STATIC_REQUIRE(std::is_same_v<ClockMult64, ClockMultT>);
    STATIC_REQUIRE(std::is_same_v<BurstGen64, BurstGenT<double>>);
    STATIC_REQUIRE(std::is_same_v<TrigDelay64, TrigDelayT>);
    STATIC_REQUIRE(std::is_same_v<Ar64, ArT<double>>);
    STATIC_REQUIRE(std::is_same_v<Ad64, AdT<double>>);
    STATIC_REQUIRE(std::is_same_v<Ahd64, AhdT<double>>);
    STATIC_REQUIRE(std::is_same_v<Dahdsr64, DahdsrT<double>>);
    STATIC_REQUIRE(std::is_same_v<ModEnv64, ModEnvT<double>>);
    STATIC_REQUIRE(std::is_same_v<TransientDetector64, TransientDetectorT<double>>);
    STATIC_REQUIRE(
        std::is_same_v<decltype(std::declval<BurstGen64&>().process(false).level), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Ar64&>().next()), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Ad64&>().next()), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Ahd64&>().next()), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Dahdsr64&>().next()), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<ModEnv64&>().next()), double>);
    STATIC_REQUIRE(
        std::is_same_v<decltype(std::declval<TransientDetector64&>().process(0.0)), double>);

    TriggerDetect detect;
    TriggerDetect64 detect64;
    detect.prepare(kSampleRate);
    detect64.prepare(kSampleRate);
    REQUIRE(detect.process(true) == detect64.process(true));
    REQUIRE(detect.process(true) == detect64.process(true));

    GateGen gate;
    GateGen64 gate64;
    gate.prepare(kSampleRate);
    gate64.prepare(kSampleRate);
    gate.set_length_samples(3);
    gate64.set_length_samples(3);
    for (int i = 0; i < 5; ++i)
        REQUIRE(gate.process(i == 0) == gate64.process(i == 0));

    ClockDivider divider;
    ClockDivider64 divider64;
    ClockMult multiplier;
    ClockMult64 multiplier64;
    TrigDelay delay;
    TrigDelay64 delay64;
    divider.set_division(3);
    divider64.set_division(3);
    for (int i = 0; i < 7; ++i)
        REQUIRE(divider.process(true) == divider64.process(true));
    multiplier.set_multiplier(2);
    multiplier64.set_multiplier(2);
    for (int i = 0; i < 20; ++i)
        REQUIRE(multiplier.process(i == 0 || i == 8) == multiplier64.process(i == 0 || i == 8));
    delay.set_delay_samples(3);
    delay64.set_delay_samples(3);
    for (int i = 0; i < 6; ++i)
        REQUIRE(delay.process(i == 0) == delay64.process(i == 0));

    BurstGen burst;
    BurstGen64 burst64;
    burst.prepare(kSampleRate);
    burst64.prepare(kSampleRate);
    burst.set_count(3);
    burst64.set_count(3);
    burst.set_spacing_ms(0.1);
    burst64.set_spacing_ms(0.1);
    burst.set_levels(0.9f, 0.3f);
    burst64.set_levels(0.9, 0.3);
    for (int i = 0; i < 20; ++i) {
        const auto hit = burst.process(i == 0);
        const auto hit64 = burst64.process(i == 0);
        REQUIRE(hit.fired == hit64.fired);
        REQUIRE_THAT(static_cast<double>(hit.level), WithinAbs(hit64.level, 1.0e-6));
    }

    Ar ar;
    Ar64 ar64;
    Ad ad;
    Ad64 ad64;
    Ahd ahd;
    Ahd64 ahd64;
    Dahdsr dahdsr;
    Dahdsr64 dahdsr64;
    ModEnv mod_env;
    ModEnv64 mod_env64;
    ar.prepare(kSampleRate);
    ar64.prepare(kSampleRate);
    ad.prepare(kSampleRate);
    ad64.prepare(kSampleRate);
    ahd.prepare(kSampleRate);
    ahd64.prepare(kSampleRate);
    dahdsr.prepare(kSampleRate);
    dahdsr64.prepare(kSampleRate);
    mod_env.prepare(kSampleRate);
    mod_env64.prepare(kSampleRate);
    ar.set_attack_ms(1.0);
    ar64.set_attack_ms(1.0);
    ar.gate(true);
    ar64.gate(true);
    ad.set_attack_ms(1.0);
    ad64.set_attack_ms(1.0);
    ad.trigger(0.8f);
    ad64.trigger(0.8);
    ahd.set_attack_ms(1.0);
    ahd64.set_attack_ms(1.0);
    ahd.trigger(0.7f);
    ahd64.trigger(0.7);
    dahdsr.set_attack_ms(1.0);
    dahdsr64.set_attack_ms(1.0);
    dahdsr.note_on(0.6f);
    dahdsr64.note_on(0.6);
    mod_env.set_attack_ms(1.0);
    mod_env64.set_attack_ms(1.0);
    mod_env.trigger(0.5f);
    mod_env64.trigger(0.5);
    for (int i = 0; i < 64; ++i) {
        REQUIRE_THAT(static_cast<double>(ar.next()), WithinAbs(ar64.next(), 1.0e-6));
        REQUIRE_THAT(static_cast<double>(ad.next()), WithinAbs(ad64.next(), 1.0e-6));
        REQUIRE_THAT(static_cast<double>(ahd.next()), WithinAbs(ahd64.next(), 1.0e-6));
        REQUIRE_THAT(static_cast<double>(dahdsr.next()), WithinAbs(dahdsr64.next(), 1.0e-6));
        REQUIRE_THAT(static_cast<double>(mod_env.next()), WithinAbs(mod_env64.next(), 1.0e-6));
    }

    TransientDetector transient;
    TransientDetector64 transient64;
    transient.prepare(kSampleRate);
    transient64.prepare(kSampleRate);
    for (int i = 0; i < 64; ++i)
        REQUIRE_THAT(static_cast<double>(transient.process(i < 8 ? 1.0f : 0.0f)),
                     WithinAbs(transient64.process(i < 8 ? 1.0 : 0.0), 5.0e-5));

    gate.reset();
    gate64.reset();
    divider.reset();
    divider64.reset();
    delay.reset();
    delay64.reset();
    ar.reset();
    ar64.reset();
    REQUIRE(gate.process(false) == gate64.process(false));
    REQUIRE(divider.process(true) == divider64.process(true));
    REQUIRE(delay.process(false) == delay64.process(false));
    REQUIRE_THAT(static_cast<double>(ar.next()), WithinAbs(ar64.next(), 1.0e-6));
}

TEST_CASE("event and envelope raw value-initialized state replays after reset",
          "[signal][mod][zero-init]") {
    TriggerDetect detect;
    TriggerDetect64 detect64;
    REQUIRE(detect.process(true));
    REQUIRE(detect64.process(true));
    detect.reset();
    detect64.reset();
    REQUIRE(detect.process(true));
    REQUIRE(detect64.process(true));

    GateGen gate;
    GateGen64 gate64;
    REQUIRE(gate.process(true));
    REQUIRE(gate64.process(true));
    gate.reset();
    gate64.reset();
    REQUIRE(gate.process(true));
    REQUIRE(gate64.process(true));

    ClockDivider divider;
    ClockDivider64 divider64;
    REQUIRE(divider.process(true));
    REQUIRE(divider64.process(true));
    divider.reset();
    divider64.reset();
    REQUIRE(divider.process(true));
    REQUIRE(divider64.process(true));

    ClockMult multiplier;
    ClockMult64 multiplier64;
    (void)multiplier.process(true);
    (void)multiplier64.process(true);
    multiplier.reset();
    multiplier64.reset();
    REQUIRE(multiplier.process(true) == multiplier64.process(true));

    BurstGen burst;
    BurstGen64 burst64;
    const auto burst_first = burst.process(true);
    const auto burst64_first = burst64.process(true);
    burst.reset();
    burst64.reset();
    REQUIRE(burst.process(true).fired == burst_first.fired);
    REQUIRE(burst64.process(true).fired == burst64_first.fired);

    TrigDelay delay;
    TrigDelay64 delay64;
    const bool delay_first = delay.process(true);
    const bool delay64_first = delay64.process(true);
    delay.reset();
    delay64.reset();
    REQUIRE(delay.process(true) == delay_first);
    REQUIRE(delay64.process(true) == delay64_first);

    Ar ar;
    Ar64 ar64;
    Ad ad;
    Ad64 ad64;
    Ahd ahd;
    Ahd64 ahd64;
    Dahdsr dahdsr;
    Dahdsr64 dahdsr64;
    ModEnv mod_env;
    ModEnv64 mod_env64;
    REQUIRE(std::isfinite(ar.next()));
    REQUIRE(std::isfinite(ar64.next()));
    REQUIRE(std::isfinite(ad.next()));
    REQUIRE(std::isfinite(ad64.next()));
    REQUIRE(std::isfinite(ahd.next()));
    REQUIRE(std::isfinite(ahd64.next()));
    REQUIRE(std::isfinite(dahdsr.next()));
    REQUIRE(std::isfinite(dahdsr64.next()));
    REQUIRE(std::isfinite(mod_env.next()));
    REQUIRE(std::isfinite(mod_env64.next()));
    ar.reset();
    ar64.reset();
    ad.reset();
    ad64.reset();
    ahd.reset();
    ahd64.reset();
    dahdsr.reset();
    dahdsr64.reset();
    mod_env.reset();
    mod_env64.reset();
    REQUIRE(ar.next() == 0.0f);
    REQUIRE(ar64.next() == 0.0);
    REQUIRE(ad.next() == 0.0f);
    REQUIRE(ad64.next() == 0.0);
    REQUIRE(ahd.next() == 0.0f);
    REQUIRE(ahd64.next() == 0.0);
    REQUIRE(dahdsr.next() == 0.0f);
    REQUIRE(dahdsr64.next() == 0.0);
    REQUIRE(mod_env.next() == 0.0f);
    REQUIRE(mod_env64.next() == 0.0);

    TransientDetector transient;
    TransientDetector64 transient64;
    REQUIRE(std::isfinite(transient.process(1.0f)));
    REQUIRE(std::isfinite(transient64.process(1.0)));
    transient.reset();
    transient64.reset();
    REQUIRE(std::isfinite(transient.process(1.0f)));
    REQUIRE(std::isfinite(transient64.process(1.0)));
}

TEST_CASE("voice aliases start fresh and track their double forms",
          "[signal][mod][parity][zero-init]") {
    STATIC_REQUIRE(std::is_same_v<Vca64, VcaT<double>>);
    STATIC_REQUIRE(std::is_same_v<Lpg64, LpgT<double>>);
    STATIC_REQUIRE(std::is_same_v<ModMatrix64, ModMatrixT<32, double>>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Vca64&>().process(0.0, 0.0)), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<Lpg64&>().process(0.0)), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<ModMatrix64::Slot>().depth), double>);
    STATIC_REQUIRE(std::is_same_v<decltype(&Lpg64::set_colour), void (Lpg64::*)(double)>);
    STATIC_REQUIRE(std::is_same_v<decltype(&Lpg64::set_range_hz), void (Lpg64::*)(double, double)>);
    STATIC_REQUIRE(std::is_same_v<decltype(&Lpg64::set_droop), void (Lpg64::*)(double)>);
    STATIC_REQUIRE(std::is_same_v<decltype(&Lpg64::cutoff_hz), double (Lpg64::*)() const>);

    Vca vca;
    Vca64 vca64;
    vca.prepare(static_cast<float>(kSampleRate));
    vca64.prepare(kSampleRate);
    REQUIRE_THAT(static_cast<double>(vca.process(0.25f, 0.75f)),
                 WithinAbs(vca64.process(0.25, 0.75), 1.0e-6));

    Lpg lpg;
    Lpg64 lpg64;
    lpg.prepare(static_cast<float>(kSampleRate));
    lpg64.prepare(kSampleRate);
    lpg.set_gate(0.7f);
    lpg64.set_gate(0.7);
    for (int i = 0; i < 64; ++i)
        REQUIRE_THAT(static_cast<double>(lpg.process(0.25f)),
                     WithinAbs(lpg64.process(0.25), 1.0e-5));
    REQUIRE_THAT(static_cast<double>(lpg.commanded_cutoff_hz()),
                 WithinRel(lpg64.commanded_cutoff_hz(), 1.0e-5));
    REQUIRE(lpg.cutoff_hz() == lpg.commanded_cutoff_hz());

    ModMatrix matrix;
    ModMatrix64 matrix64;
    const std::array<float, 1> sources{0.5f};
    const std::array<double, 1> sources64{0.5};
    std::array<float, 1> dests{0.0f};
    std::array<double, 1> dests64{0.0};
    matrix.set_slot(0, 0, 0, 0.75f);
    matrix64.set_slot(0, 0, 0, 0.75);
    matrix.evaluate(std::span<const float>(sources), std::span<float>(dests));
    matrix64.evaluate(std::span<const double>(sources64), std::span<double>(dests64));
    REQUIRE_THAT(static_cast<double>(dests[0]), WithinAbs(dests64[0], 1.0e-6));
}

TEST_CASE("voice raw value-initialized state replays after reset", "[signal][mod][zero-init]") {
    Vca vca;
    Vca64 vca64;
    const auto vca_first = vca.process(0.5f, 0.75f);
    const auto vca64_first = vca64.process(0.5, 0.75);
    vca.reset();
    vca64.reset();
    REQUIRE(vca.process(0.5f, 0.75f) == vca_first);
    REQUIRE(vca64.process(0.5, 0.75) == vca64_first);

    Lpg lpg;
    Lpg64 lpg64;
    lpg.strike(0.8f);
    lpg64.strike(0.8);
    const auto lpg_first = lpg.process(0.5f);
    const auto lpg64_first = lpg64.process(0.5);
    lpg.reset();
    lpg64.reset();
    lpg.strike(0.8f);
    lpg64.strike(0.8);
    REQUIRE(lpg.process(0.5f) == lpg_first);
    REQUIRE(lpg64.process(0.5) == lpg64_first);

    ModMatrix matrix;
    ModMatrix64 matrix64;
    const std::array<float, 1> source{0.5f};
    const std::array<double, 1> source64{0.5};
    std::array<float, 1> dest{0.25f};
    std::array<double, 1> dest64{0.25};
    matrix.evaluate(std::span<const float>(source), std::span<float>(dest));
    matrix64.evaluate(std::span<const double>(source64), std::span<double>(dest64));
    REQUIRE(dest[0] == 0.25f);
    REQUIRE(dest64[0] == 0.25);
}
