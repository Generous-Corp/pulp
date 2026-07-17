// Hard sync, through-zero FM, and the two composed — the case where a sync reset
// lands in the same sample as FM-driven wraps.
//
// ── Why the FM cases are measured only at commensurate points ──────────────
//
// "Alias floor" is undefined under general FM. The legitimate spectrum lives at
// |m*f_c + n*f_mod| for all integer m, n; sweep f_mod continuously and that set
// is dense, so no classifier can call anything an alias — it would be grading the
// sidebands the patch asked for.
//
// Every FM measurement here therefore sits at a COMMENSURATE point, and the
// proof is arithmetic rather than assertion. With instantaneous frequency
// f(t) = f_c + dev*sin(2*pi*f_mod*t), the phase is
//
//     phase(t) = f_c*t - (dev / (2*pi*f_mod)) * cos(2*pi*f_mod*t) + C
//
// so over one modulator period phase advances by exactly f_c/f_mod — the cosine
// term returns to itself and contributes nothing. Choose f_c = k*f_mod for
// integer k and the waveform repeats exactly every 1/f_mod: the signal is
// periodic at f_mod, its spectrum IS the harmonic series of f_mod, and every
// harmonic above Nyquist is a genuine alias at its fold site. That is a
// legitimate alias measurement, and it is legitimate only here.
//
// For the composed cases the sync forces periodicity at the master's rate
// outright — the slave is reset every master period — so the fundamental is the
// master frequency for any slave frequency, and f_mod is pinned to the master so
// the modulation repeats with it.
//
// ── The rest of the measurement discipline ────────────────────────────────
//
// Unchanged from the static shapes: every claim band-qualified to 20 kHz (fold-
// back is continuous across Nyquist, so a full-band claim is impossible for any
// method); the detection floor PROVEN by negative control rather than read from
// the analyzer's own derived bound (which assumes a white residual, false when
// aliases are discrete tones); and every reading checked against that floor with
// the fit required to have resolved.
//
// ── What is being claimed, and what is not ────────────────────────────────
//
// polyBLEP buys 11-34 dB over the trivial waveform across these cases. The gates
// sit at 8 dB: below the measured minimum with margin, and far above the two
// physically meaningful landmarks — 0 dB (the correction did nothing) and
// negative (it did harm). They are regression tripwires, not certifications.
//
// The honest limit is recorded rather than hidden: once the instantaneous
// frequency approaches Nyquist, the correction stops helping and eventually does
// nothing at all, because the aliasing then comes from the carrier itself
// exceeding the band and not from any discontinuity. No kernel corrects that —
// it is an oversampling problem. `correction cannot rescue a carrier past
// Nyquist` measures exactly where that wall is.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/osc/phase.hpp>
#include <pulp/signal/osc/va.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::osc::PhaseAccumulator;
using pulp::signal::osc::PhaseEvent;
using pulp::signal::osc::PhaseEventKind;
using pulp::signal::osc::VaOscillator;
using pulp::signal::osc::VaShape;
using pulp::test::audio::AliasOptions;
using pulp::test::audio::AliasReport;
using pulp::test::audio::fold_frequency;
using pulp::test::audio::measure_aliasing;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kRenderLength = 8192;
constexpr double kBandHz = 20000.0;
constexpr double kMinImprovementDb = 8.0;

// Master / fundamental. Prime and coprime to 48000, so no harmonic's fold site
// lands on another harmonic for any h the fit reaches — an f0 sharing a large
// factor with the sample rate folds high harmonics exactly onto low ones, which
// is a degenerate fit rather than a measurement.
constexpr double kMasterHz = 1103.0;

// Slave-to-master ratio for the composed cases. Deliberately NOT an integer.
//
// At an integer ratio the slave has gained a whole number of cycles by the time
// the master fires, so its phase sits at 0 (mod 1) and the sync merely stands in
// for the wrap that was about to happen anyway — the reset adds no discontinuity
// of its own and the case silently stops testing sync at all. At 2.7 the slave is
// consistently at phase 0.7 when reset, which is a genuine mid-waveform step.
constexpr double kSyncRatio = 2.7;

// Modulator rate, pinned to the master so the modulation repeats with the sync
// and the composite stays periodic at the master. See the commensurability note.
constexpr double kModHz = 1103.0;

int harmonics_for(double f0) {
    return static_cast<int>(std::ceil(3.0 * kSampleRate / f0));
}

const char* shape_name(VaShape s) {
    switch (s) {
        case VaShape::sine: return "sine";
        case VaShape::saw: return "saw";
        case VaShape::square: return "square";
        case VaShape::triangle: return "triangle";
    }
    return "unknown";
}

// The trivial shape, written out independently of the oscillator so a reference
// cannot inherit a bug from the thing it is grading. Matches `VaOscillator`'s
// own definitions, including the sine's exact limit at the period boundary.
double trivial_value(VaShape shape, double p) {
    switch (shape) {
        case VaShape::sine: return p >= 1.0 ? 0.0 : std::sin(2.0 * std::numbers::pi * p);
        case VaShape::saw: return 2.0 * p - 1.0;
        case VaShape::square: return p < 0.5 ? 1.0 : -1.0;
        case VaShape::triangle: return p < 0.5 ? (4.0 * p - 1.0) : (3.0 - 4.0 * p);
    }
    return 0.0;
}

double instantaneous_hz(double carrier_hz, double mod_hz, double deviation_hz, int i) {
    return carrier_hz +
           deviation_hz * std::sin(2.0 * std::numbers::pi * mod_hz * static_cast<double>(i) /
                                   kSampleRate);
}

// Where a master oscillator fires this sample, or a negative value if it does
// not. The master's forward wrap IS the sync event: that is what hard sync means.
double sync_frac_this_sample(PhaseAccumulator& master, double increment) {
    master.advance(increment);
    for (const PhaseEvent& e : master.events())
        if (e.kind == PhaseEventKind::wrap_forward) return e.frac;
    return -1.0;
}

// One renderer for all three regimes: a static slave is deviation 0, and no sync
// is master rate 0. Using one path keeps the trivial and corrected references
// walking the identical phase trajectory, which is what makes their difference
// attributable to the correction and nothing else.
std::vector<double> render(VaShape shape, double master_hz, double carrier_hz,
                           double deviation_hz, bool corrected) {
    PhaseAccumulator master;
    master.reset(0.0);
    VaOscillator osc;
    osc.set_shape(shape);
    osc.reset(0.0);
    PhaseAccumulator trivial_phase;
    trivial_phase.reset(0.0);

    std::vector<double> out(static_cast<std::size_t>(kRenderLength));
    for (int i = 0; i < kRenderLength; ++i) {
        const double sync_frac =
            master_hz > 0.0 ? sync_frac_this_sample(master, master_hz / kSampleRate) : -1.0;
        const double increment = instantaneous_hz(carrier_hz, kModHz, deviation_hz, i) / kSampleRate;

        if (corrected) {
            out[static_cast<std::size_t>(i)] =
                sync_frac >= 0.0 ? osc.next_synced(increment, sync_frac, 0.0) : osc.next(increment);
        } else {
            out[static_cast<std::size_t>(i)] = trivial_value(shape, trivial_phase.phase());
            if (sync_frac >= 0.0) trivial_phase.advance_synced(increment, sync_frac, 0.0);
            else trivial_phase.advance(increment);
        }
    }
    return out;
}

// An exactly bandlimited saw: every harmonic below Nyquist summed at its own
// frequency in double precision. No discontinuity, so zero alias energy BY
// CONSTRUCTION — which is what makes it a negative control rather than an
// assertion. Optionally injects a tone of known level on a fold site.
std::vector<double> bandlimited_saw(double f0, double alias_hz = 0.0,
                                    double alias_amplitude = 0.0) {
    const int max_h = static_cast<int>(std::floor((kSampleRate / 2.0) / f0));
    std::vector<double> out(static_cast<std::size_t>(kRenderLength), 0.0);
    for (int i = 0; i < kRenderLength; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        double v = 0.0;
        for (int h = 1; h <= max_h; ++h) {
            const double sign = (h % 2 == 1) ? 1.0 : -1.0;
            v += sign * std::sin(2.0 * std::numbers::pi * h * f0 * t + 0.3) / h;
        }
        v *= 2.0 / std::numbers::pi;
        if (alias_amplitude > 0.0)
            v += alias_amplitude * std::sin(2.0 * std::numbers::pi * alias_hz * t + 0.9);
        out[static_cast<std::size_t>(i)] = v;
    }
    return out;
}

constexpr double kBandlimitedFundamental = 2.0 / std::numbers::pi;

AliasReport analyze(const std::vector<double>& signal, double f0) {
    pulp::audio::Buffer<float> buffer(1, static_cast<int>(signal.size()));
    for (int i = 0; i < static_cast<int>(signal.size()); ++i)
        buffer.channel(0)[i] = static_cast<float>(signal[static_cast<std::size_t>(i)]);

    AliasOptions options;
    options.num_harmonics = harmonics_for(f0);
    options.analysis_length = static_cast<int>(signal.size());
    options.max_alias_frequency_hz = kBandHz;
    return measure_aliasing(std::as_const(buffer).view(), f0, kSampleRate, options);
}

int first_in_band_alias_harmonic(double f0) {
    for (int h = static_cast<int>(std::floor((kSampleRate / 2.0) / f0)) + 1;
         h <= harmonics_for(f0); ++h)
        if (fold_frequency(h * f0, kSampleRate) <= kBandHz) return h;
    return 0;
}

// Measure the correction's benefit in one regime, and require the reading to be
// one the measurement can actually support.
double improvement_db(VaShape shape, double master_hz, double carrier_hz, double deviation_hz,
                      double fundamental_hz) {
    const auto trivial = analyze(render(shape, master_hz, carrier_hz, deviation_hz, false),
                                 fundamental_hz);
    const auto corrected = analyze(render(shape, master_hz, carrier_hz, deviation_hz, true),
                                   fundamental_hz);

    INFO(shape_name(shape) << ": master " << master_hz << " Hz, carrier " << carrier_hz
                           << " Hz, deviation " << deviation_hz << " Hz -> worst in-band alias "
                           << trivial.worst_alias_db << " dBc trivial, " << corrected.worst_alias_db
                           << " dBc corrected (" << (trivial.worst_alias_db - corrected.worst_alias_db)
                           << " dB better); analyzer-derived floor " << corrected.detection_floor_db);

    REQUIRE_FALSE(trivial.has_unresolved_in_band_alias);
    REQUIRE_FALSE(corrected.has_unresolved_in_band_alias);
    REQUIRE(corrected.worst_alias_db > corrected.detection_floor_db);
    return trivial.worst_alias_db - corrected.worst_alias_db;
}

} // namespace

// ── The measurement, before anything is measured with it ──────────────────

TEST_CASE("alias measurement separates a clean signal from a known-bad one",
          "[signal][osc][alias][sync]") {
    // The negative control. Every gate below is read through this measurement,
    // and a gate whose measurement cannot see failure passes silently. The
    // analyzer's own derived floor cannot establish this — it assumes a white
    // residual, and aliases are discrete tones — so the floor is proven by
    // construction instead.
    const double f0 = kMasterHz;
    const int h = first_in_band_alias_harmonic(f0);
    REQUIRE(h > 0);
    const double site_hz = fold_frequency(h * f0, kSampleRate);
    REQUIRE(site_hz <= kBandHz);

    SECTION("alias removed: the reading collapses") {
        const auto report = analyze(bandlimited_saw(f0), f0);
        INFO("bandlimited saw with no alias by construction reads " << report.worst_alias_db
                                                                    << " dBc");
        REQUIRE_FALSE(report.has_unresolved_in_band_alias);
        CHECK(report.worst_alias_db < -140.0);
    }

    SECTION("alias injected at a known level: it is recovered") {
        for (const double truth_db : {-60.0, -100.0, -120.0}) {
            const double amplitude = std::pow(10.0, truth_db / 20.0) * kBandlimitedFundamental;
            const auto report = analyze(bandlimited_saw(f0, site_hz, amplitude), f0);
            INFO("injected " << truth_db << " dBc at " << site_hz << " Hz; read "
                             << report.worst_alias_db << " dBc");
            REQUIRE_FALSE(report.has_unresolved_in_band_alias);
            CHECK(report.worst_alias_index == h);
            CHECK_THAT(report.worst_alias_db, WithinAbs(truth_db, 1.0));
        }
    }
}

// ── Sync alone ────────────────────────────────────────────────────────────

TEST_CASE("hard sync improves alias rejection at a static slave frequency",
          "[signal][osc][alias][sync]") {
    // A synced oscillator's fundamental is the MASTER's rate, whatever the slave
    // is doing: the reset forces the waveform to repeat with the master. That is
    // why the fit is at kMasterHz for every ratio here.
    //
    // The reset is a value step of arbitrary magnitude — unlike a wrap, whose
    // endpoints are 1 -> 0 and known in advance. The corrector reads the
    // magnitude off the event's own endpoints, which is what lets one path serve
    // both.
    for (const VaShape shape : {VaShape::saw, VaShape::square, VaShape::triangle, VaShape::sine}) {
        for (const double ratio : {1.7, 2.7, 5.3}) {
            const double improvement =
                improvement_db(shape, kMasterHz, kMasterHz * ratio, 0.0, kMasterHz);
            INFO(shape_name(shape) << " at sync ratio " << ratio << ": " << improvement
                                   << " dB improvement");
            CHECK(improvement >= kMinImprovementDb);
        }
    }
}

TEST_CASE("sync corrects a sine, which no wrap ever does", "[signal][osc][alias][sync]") {
    // A sine is continuous across a wrap, so the static shape needs no correction
    // and gets none. A sync is a different animal: it jumps the phase to an
    // unrelated point, so the value genuinely steps and the step must be
    // corrected. This is the case that catches a corrector which decides "sine =
    // nothing to do" once, up front, instead of asking each discontinuity.
    const double improvement =
        improvement_db(VaShape::sine, kMasterHz, kMasterHz * kSyncRatio, 0.0, kMasterHz);
    INFO("synced sine: " << improvement << " dB improvement");
    CHECK(improvement >= kMinImprovementDb);
}

// ── Through-zero FM alone ─────────────────────────────────────────────────

TEST_CASE("the phase genuinely reverses through zero", "[signal][osc][sync]") {
    // The point of through-zero FM, asserted on the phase itself rather than
    // inferred from the output staying finite. An accumulator that clamped at
    // zero — or that only wrapped at the top — would still produce plausible
    // audio while silently deleting the through-zero region.
    SECTION("a negative increment runs the phase backward") {
        VaOscillator osc;
        osc.set_shape(VaShape::saw);
        osc.reset(0.5);
        double previous = osc.phase();
        for (int i = 0; i < 3; ++i) {
            osc.next(-0.1);
            INFO("step " << i << ": phase " << previous << " -> " << osc.phase());
            CHECK(osc.phase() < previous);
            previous = osc.phase();
        }
        CHECK_THAT(previous, WithinAbs(0.2, 1e-12));
    }

    SECTION("crossing zero downward wraps to the top and reports it") {
        PhaseAccumulator phase;
        phase.reset(0.05);
        const int events = phase.advance(-0.1);
        CHECK_THAT(phase.phase(), WithinAbs(0.95, 1e-12));
        REQUIRE(events == 1);
        CHECK(phase.events()[0].kind == PhaseEventKind::wrap_backward);
    }
}

TEST_CASE("through-zero FM improves alias rejection at a commensurate point",
          "[signal][osc][alias][sync]") {
    // Carrier is an exact multiple of the modulator, so the signal is periodic at
    // the modulator and its spectrum is that harmonic series — see the file
    // header for why an alias claim is meaningful HERE and nowhere else under FM.
    const double carrier = 3.0 * kModHz;
    for (const VaShape shape : {VaShape::saw, VaShape::square, VaShape::triangle}) {
        // 2000 Hz keeps the frequency positive; 5000 Hz drives it to -1691 Hz,
        // which is the through-zero case proper.
        for (const double deviation : {2000.0, 5000.0}) {
            INFO("instantaneous frequency spans " << (carrier - deviation) << " .. "
                                                  << (carrier + deviation) << " Hz");
            const double improvement = improvement_db(shape, 0.0, carrier, deviation, kModHz);
            CHECK(improvement >= kMinImprovementDb);
        }
    }
}

TEST_CASE("FM alone leaves a sine untouched, because it has nothing to correct",
          "[signal][osc][alias][sync]") {
    // Not a defect — the honest boundary of what a discontinuity corrector does.
    // An unsynced sine is continuous no matter how its frequency is modulated, so
    // there is no step and no slope break, and the corrector contributes exactly
    // nothing. Any aliasing an FM sine produces comes from its sidebands crossing
    // Nyquist, which is an oversampling problem that no BLEP addresses.
    const double carrier = 3.0 * kModHz;
    const auto trivial = analyze(render(VaShape::sine, 0.0, carrier, 5000.0, false), kModHz);
    const auto corrected = analyze(render(VaShape::sine, 0.0, carrier, 5000.0, true), kModHz);

    INFO("FM sine: trivial " << trivial.worst_alias_db << " dBc, corrected "
                             << corrected.worst_alias_db << " dBc");
    // Bit-identical: the corrector must not fire on a continuous shape even when
    // its frequency is swinging through zero.
    const auto a = render(VaShape::sine, 0.0, carrier, 5000.0, false);
    const auto b = render(VaShape::sine, 0.0, carrier, 5000.0, true);
    int differing = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) ++differing;
    CHECK(differing == 0);
}

// ── The composition ───────────────────────────────────────────────────────

TEST_CASE("sync composed with through-zero FM keeps the correction",
          "[signal][osc][alias][sync]") {
    // THE case this file exists for: a sync reset landing in the same sample as
    // FM-driven wraps, at a ratio where the reset is a real mid-waveform step.
    //
    // It works for one structural reason, not by enumerating the combinations:
    // every event carries the endpoints of its own jump, so a sample's events are
    // corrected by summing their steps, and coincident events telescope. A sync
    // to 0 under a negative increment reports the sync (p -> 0) AND a backward
    // wrap (0 -> 1) at one position; summed, the intermediate cancels and the
    // pair describes p -> 1, which is what actually happened.
    const double carrier = kMasterHz * kSyncRatio;
    for (const VaShape shape : {VaShape::saw, VaShape::square, VaShape::triangle}) {
        for (const double deviation : {2000.0, 5000.0}) {
            const double improvement =
                improvement_db(shape, kMasterHz, carrier, deviation, kMasterHz);
            INFO(shape_name(shape) << " sync+TZFM at deviation " << deviation << ": "
                                   << improvement << " dB improvement");
            CHECK(improvement >= kMinImprovementDb);
        }
    }
}

TEST_CASE("composing sync with FM does not cost the correction its benefit",
          "[signal][osc][alias][sync]") {
    // The composition's failure mode would be quiet: each mechanism works alone,
    // and together they degrade. Comparing composed against sync-alone at the
    // same ratio is what would expose that, so it is asserted rather than assumed
    // from the two passing separately.
    const double carrier = kMasterHz * kSyncRatio;
    for (const VaShape shape : {VaShape::saw, VaShape::square, VaShape::triangle}) {
        const double sync_only = improvement_db(shape, kMasterHz, carrier, 0.0, kMasterHz);
        const double composed = improvement_db(shape, kMasterHz, carrier, 5000.0, kMasterHz);
        INFO(shape_name(shape) << ": sync alone " << sync_only << " dB, composed with TZFM "
                               << composed << " dB");
        // Composed measures 11.6-14.8 dB against 12.9-16.9 alone: within a couple
        // of dB, not a collapse. A real composition failure would show here as
        // composed falling to a fraction of sync-alone.
        CHECK(composed >= kMinImprovementDb);
        CHECK(composed >= sync_only - 4.0);
    }
}

TEST_CASE("correction cannot rescue a carrier past Nyquist", "[signal][osc][alias][sync]") {
    // The honest wall, recorded because it bounds every claim above.
    //
    // As the deviation grows the instantaneous frequency approaches and then
    // exceeds Nyquist, and the aliasing stops coming from discontinuities and
    // starts coming from the carrier itself being unrepresentable. A
    // discontinuity corrector has nothing to say about that: correcting a step
    // perfectly does not bandlimit a sine that is running past half the sample
    // rate. Fixing this needs oversampling on the FM path, which is not in this
    // file and is not claimed by it.
    //
    // The sine is the clearest witness — it has no discontinuity, so all of its
    // aliasing is the uncorrectable kind.
    const double carrier = kMasterHz * kSyncRatio;

    SECTION("a sine's benefit falls away as its frequency nears Nyquist") {
        const double moderate = improvement_db(VaShape::sine, kMasterHz, carrier, 5000.0, kMasterHz);
        const double extreme = improvement_db(VaShape::sine, kMasterHz, carrier, 20000.0, kMasterHz);
        INFO("synced sine: " << moderate << " dB at 5 kHz deviation (peak "
                             << (carrier + 5000.0) << " Hz), " << extreme
                             << " dB at 20 kHz deviation (peak " << (carrier + 20000.0)
                             << " Hz, against a 24 kHz Nyquist)");
        // Measures ~12.4 dB and ~6.1 dB. The claim is the DIRECTION and that the
        // wall is real, not the exact figures.
        CHECK(moderate >= kMinImprovementDb);
        CHECK(extreme < moderate);
    }

    SECTION("past Nyquist the output is beyond rescue, corrected or not") {
        // Deviation 60 kHz drives |f| to 63 kHz — over the sample rate itself, so
        // the phase advances more than a cycle per sample and the shape is simply
        // not sampled. The aliases end up LOUDER than the fundamental.
        const auto trivial = analyze(render(VaShape::sine, kMasterHz, carrier, 60000.0, false),
                                     kMasterHz);
        const auto corrected = analyze(render(VaShape::sine, kMasterHz, carrier, 60000.0, true),
                                       kMasterHz);
        INFO("synced sine at 60 kHz deviation: trivial " << trivial.worst_alias_db
                                                         << " dBc, corrected "
                                                         << corrected.worst_alias_db << " dBc");
        // Both are ABOVE 0 dBc: the alias exceeds the fundamental. Stated as a
        // measurement, not smoothed into a pass.
        CHECK(trivial.worst_alias_db > 0.0);
        CHECK(corrected.worst_alias_db > 0.0);
        // And the correction buys essentially nothing here — under 3 dB either
        // way. Asserting a floor AND a ceiling keeps this from silently becoming
        // a success story.
        CHECK(std::fabs(trivial.worst_alias_db - corrected.worst_alias_db) < 3.0);
    }
}

// ── Composition mechanics ─────────────────────────────────────────────────

TEST_CASE("coincident events sum rather than contradict", "[signal][osc][sync]") {
    // Two events can share a sub-sample position, and a corrector must add them.
    // Syncing to 0 under a negative increment is the case: the accumulator reports
    // the sync (p -> 0) and a backward wrap (0 -> 1) at one frac, and the phase is
    // 0 only at that instant. Treating either as authoritative alone would correct
    // a step that did not happen; summing them recovers the one that did.
    PhaseAccumulator phase;
    phase.reset(0.3);
    const int events = phase.advance_synced(-0.1, 0.4, 0.0);
    REQUIRE(events == 2);

    const auto& sync = phase.events()[0];
    const auto& wrap = phase.events()[1];
    CHECK(sync.kind == PhaseEventKind::sync);
    CHECK(wrap.kind == PhaseEventKind::wrap_backward);
    // Same position: a corrector cannot order them apart.
    CHECK(sync.frac == wrap.frac);

    // Summed, a saw's steps telescope: the intermediate value(0) cancels and what
    // is left is the jump that physically occurred.
    const auto saw = [](double p) { return 2.0 * p - 1.0; };
    const double summed = (saw(sync.phase_after) - saw(sync.phase_before)) +
                          (saw(wrap.phase_after) - saw(wrap.phase_before));
    const double direct = saw(1.0) - saw(sync.phase_before);
    INFO("sync " << sync.phase_before << " -> " << sync.phase_after << " then wrap "
                 << wrap.phase_before << " -> " << wrap.phase_after << "; summed step " << summed
                 << " vs direct " << direct);
    CHECK_THAT(summed, WithinAbs(direct, 1e-12));
}

TEST_CASE("a sync splits the sample, and the split is not cosmetic",
          "[signal][osc][sync]") {
    // A sync does not merely interrupt the phase — it REPLACES it, so the sample
    // holds two trajectories: one from the entry phase up to the reset, and one
    // from the target onward. An internal threshold (the triangle's apex, the
    // square's pulse edge) has to be looked for in each, from that segment's own
    // starting phase.
    //
    // The aggregate alias gates above cannot see this, and it is worth being
    // precise about why rather than trusting them to. The first segment's
    // trajectory IS the unsegmented one, so a crossing before the reset lands in
    // the same place either way; only a crossing AFTER the reset is misplaced or
    // lost. In the FM cases that is a couple of dozen samples out of 8192 — real,
    // but far too few to move a worst-alias reading. A gate that cannot fail on a
    // bug is not covering it, so this case pins the arithmetic directly.
    //
    // Ground truth is computed here from the kernels' closed form, not captured
    // from a run.
    const auto triangle = [](double p) { return p < 0.5 ? (4.0 * p - 1.0) : (3.0 - 4.0 * p); };
    constexpr double kEntry = 0.1;
    constexpr double kIncrement = 0.2;
    constexpr double kSyncFrac = 0.5;
    constexpr double kTarget = 0.45;

    // Segment 1 runs 0.10 -> 0.20 and crosses nothing. The reset then jumps the
    // phase to 0.45, and segment 2 runs 0.45 -> 0.55, crossing the apex halfway
    // through its own span — at sample position 0.5 + 0.5 * 0.5 = 0.75.
    double crossings[PhaseAccumulator::max_events_per_sample];
    const int after_reset = pulp::signal::osc::threshold_crossings(
        kTarget, kIncrement * (1.0 - kSyncFrac), 0.5, std::span<double>(crossings));
    REQUIRE(after_reset == 1);
    CHECK_THAT(crossings[0], WithinAbs(0.5, 1e-12));

    // The trajectory the phase never took: scanning the whole sample from the
    // entry phase sees 0.10 -> 0.30 and finds no apex at all.
    const int unsegmented = pulp::signal::osc::threshold_crossings(
        kEntry, kIncrement, 0.5, std::span<double>(crossings));
    INFO("an unsegmented scan of the same sample finds " << unsegmented
                                                         << " apex crossings, not 1");
    CHECK(unsegmented == 0);

    // What the sample must contain: the shape at the entry phase, the sync's own
    // value step, and the apex's slope break at position 0.75. The sync itself
    // breaks no slope here — 0.20 and 0.45 are both on the rising leg.
    const double sync_value_step = triangle(kTarget) - triangle(0.2);
    const double apex_slope_step = (-4.0 - 4.0) * kIncrement;
    const double expected = triangle(kEntry) +
                            pulp::signal::osc::poly_blep(kSyncFrac, sync_value_step).before +
                            pulp::signal::osc::poly_blamp(0.75, apex_slope_step).before;

    VaOscillator osc;
    osc.set_shape(VaShape::triangle);
    osc.reset(kEntry);
    const double produced = osc.next_synced(kIncrement, kSyncFrac, kTarget);

    INFO("expected " << expected << ", produced " << produced << "; the apex term alone is "
                     << pulp::signal::osc::poly_blamp(0.75, apex_slope_step).before);
    CHECK(produced == expected);
}

TEST_CASE("several events in one sample are all corrected", "[signal][osc][sync]") {
    // Under heavy modulation the phase can cover more than a cycle in a sample, so
    // a sync can arrive alongside multiple wraps. The corrector must walk them all
    // — dropping any one leaves an uncorrected discontinuity.
    PhaseAccumulator phase;
    phase.reset(0.1);
    const int events = phase.advance_synced(2.5, 0.5, 0.0);
    INFO("increment 2.5 with a sync at mid-sample produced " << events << " events");
    // Wraps before the reset, the reset, and wraps after it.
    CHECK(events >= 3);
    CHECK_FALSE(phase.truncated());

    // Chronological, and every event inside the sample.
    double previous = -1.0;
    for (const PhaseEvent& e : phase.events()) {
        CHECK(e.frac >= 0.0);
        CHECK(e.frac <= 1.0);
        CHECK(e.frac >= previous);
        previous = e.frac;
    }

    SECTION("the oscillator stays bounded when they cluster") {
        // Correcting overlapping discontinuities is beyond a two-point kernel, so
        // the contract here is boundedness, not accuracy. Claiming accuracy would
        // be claiming something polyBLEP cannot deliver.
        for (const VaShape shape : {VaShape::saw, VaShape::square, VaShape::triangle}) {
            const auto rendered = render(shape, kMasterHz, kMasterHz * kSyncRatio, 60000.0, true);
            INFO(shape_name(shape) << " with clustered wraps");
            for (const double x : rendered) REQUIRE(std::isfinite(x));
        }
    }
}

// ── Nulls ─────────────────────────────────────────────────────────────────

TEST_CASE("zero modulation depth is bit-identical to no modulation",
          "[signal][osc][sync]") {
    // The strongest null available for the FM path, and it passes for a reason
    // worth stating: there IS no FM path. Frequency arrives as a per-sample
    // increment, so through-zero FM is the caller varying that argument and needs
    // no code of its own — which is exactly why nothing can be active at zero
    // depth. The test pins that property so a future FM-specific path cannot
    // quietly appear and start contributing when it should be inert.
    for (const VaShape shape : {VaShape::sine, VaShape::saw, VaShape::square, VaShape::triangle}) {
        VaOscillator modulated;
        VaOscillator plain;
        modulated.set_shape(shape);
        plain.set_shape(shape);
        modulated.reset(0.1);
        plain.reset(0.1);

        const double carrier = kMasterHz * kSyncRatio;
        int differing = 0;
        for (int i = 0; i < 2048; ++i) {
            const double increment = instantaneous_hz(carrier, kModHz, 0.0, i) / kSampleRate;
            if (modulated.next(increment) != plain.next(carrier / kSampleRate)) ++differing;
        }
        INFO(shape_name(shape) << ": " << differing << " of 2048 samples differ at zero depth");
        CHECK(differing == 0);
    }
}

TEST_CASE("sync at ratio one with an aligned phase changes nothing audible",
          "[signal][osc][sync]") {
    // A master and slave at the same rate from the same phase wrap together, so
    // the reset lands exactly where the phase was already going: a no-op.
    //
    // This is a tolerance null and NOT a bit-exact one, deliberately. `advance` does
    // the sample's arithmetic in one span while `advance_synced` splits it either
    // side of the reset, so the two reassociate the same additions differently and
    // land ~1e-15 apart. That is float reassociation, not a correction firing —
    // and the distinction is exactly what the bound has to be chosen to preserve:
    // loose enough to admit reassociation, tight enough that any real correction
    // (which would be of order the waveform itself) fails it.
    for (const VaShape shape : {VaShape::sine, VaShape::saw, VaShape::square, VaShape::triangle}) {
        PhaseAccumulator master;
        master.reset(0.0);
        VaOscillator synced;
        VaOscillator free_running;
        synced.set_shape(shape);
        free_running.set_shape(shape);
        synced.reset(0.0);
        free_running.reset(0.0);

        const double increment = kMasterHz / kSampleRate;
        double worst = 0.0;
        for (int i = 0; i < 2048; ++i) {
            const double sync_frac = sync_frac_this_sample(master, increment);
            const double a = sync_frac >= 0.0 ? synced.next_synced(increment, sync_frac, 0.0)
                                              : synced.next(increment);
            const double b = free_running.next(increment);
            worst = std::fmax(worst, std::fabs(a - b));
        }
        INFO(shape_name(shape) << ": worst |synced - free-running| = " << worst);
        CHECK(worst < 1e-12);
    }
}

TEST_CASE("syncing to zero while running backward composes at one position",
          "[signal][osc][sync]") {
    // The operating point where sync and through-zero FM meet, and the one the
    // accumulator's own docs single out: reset to 0 under a negative increment
    // and the phase is at 0 for an instant before continuing down past it, so a
    // sync (p -> 0) and a backward wrap (0 -> 1) are reported at the SAME
    // position. Two corrections land on one sub-sample instant and must sum.
    //
    // Ground truth by hand rather than captured: entry phase 0.3 puts the saw at
    // -0.4. Segment one advances -0.1 * 0.4 = -0.04 to phase 0.26. The sync steps
    // value(0) - value(0.26) = -0.52; the backward wrap then steps
    // value(1) - value(0) = +2.0. Those telescope to +1.48 — the jump that
    // physically happened, 0.26 -> 1.0 — and the before tap at d = 0.4 scales by
    // 0.5*d^2 - d + 0.5 = 0.18, so the sample is -0.4 + 0.18 * 1.48 = -0.1336.
    VaOscillator osc;
    osc.set_shape(VaShape::saw);
    osc.reset(0.3);
    const double produced = osc.next_synced(-0.1, 0.4, 0.0);
    INFO("saw, entry phase 0.3, increment -0.1, sync to 0 at frac 0.4: " << produced);
    CHECK_THAT(produced, WithinAbs(-0.1336, 1e-12));

    SECTION("a sync landing on a sample boundary stays in range, either direction") {
        // frac = 0 puts the reset exactly on the entry sample. Going backward this
        // is also where the accumulator reports a wrap at frac 0 — the case where
        // a corrector reading its residual off the wrong side of the step lands a
        // full step outside the waveform's range.
        for (const double increment : {0.1, -0.1}) {
            for (const VaShape shape : {VaShape::saw, VaShape::square, VaShape::triangle,
                                        VaShape::sine}) {
                VaOscillator o;
                o.set_shape(shape);
                o.reset(0.3);
                const double v = o.next_synced(increment, 0.0, 0.0);
                INFO(shape_name(shape) << " increment " << increment
                                       << ", sync to 0 at frac 0: " << v);
                REQUIRE(std::isfinite(v));
                CHECK(std::abs(v) <= 1.0 + 1e-9);
            }
        }
    }

    SECTION("sustained backward running with a periodic reset stays in range") {
        for (const VaShape shape : {VaShape::saw, VaShape::square, VaShape::triangle,
                                    VaShape::sine}) {
            VaOscillator o;
            o.set_shape(shape);
            // Start on the boundary: the first sample is the one a backward wrap
            // lands on.
            o.reset(0.0);
            for (int i = 0; i < 256; ++i) {
                const double v = (i % 8 == 0) ? o.next_synced(-0.13, 0.5, 0.0) : o.next(-0.13);
                INFO(shape_name(shape) << " sample " << i << " = " << v);
                REQUIRE(std::isfinite(v));
                CHECK(std::abs(v) <= 1.0 + 1e-9);
            }
        }
    }
}

TEST_CASE("synced oscillator output is deterministic", "[signal][osc][sync]") {
    for (const VaShape shape : {VaShape::sine, VaShape::saw, VaShape::square, VaShape::triangle}) {
        const auto first = render(shape, kMasterHz, kMasterHz * kSyncRatio, 5000.0, true);
        const auto second = render(shape, kMasterHz, kMasterHz * kSyncRatio, 5000.0, true);
        REQUIRE(first.size() == second.size());
        for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);
    }
}
