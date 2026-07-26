// The pitch family's bake-layer catalog suite.
//
// The DSP block's own acceptance suite (test_signal_pitch_shifter.cpp) proves
// the shifter. This file proves the NODE — that the graph, the bake and the
// parameter-injection channel deliver those controls to it in real musical
// units, over the production path (bake → claim_param_injection →
// ParamInjector → routed executor), without allocating.
//
// ## Measurement recipe, and the trap the DSP header warns about
//
// fs = 48 kHz over 128-frame blocks. The test tone is 3 kHz — 8 whole periods
// per block, so repeating one block is a continuous sine rather than a
// re-triggered one. Analysis captures 64 blocks (8192 samples), in which 3 kHz
// is 512 whole periods, so a coherent DFT at 3 kHz and at every frequency used
// below (1500, 6000, 9000, 12000) is leakage-free and exact.
//
// THE TRAP, quoting the DSP header: the two crossfade taps read the same stream
// half a window apart, so for an input partial at `f` their separation is
// `q = f · window_ms / 1000` half-cycles. When `q` is an EVEN integer the taps
// are in phase and that partial shifts to a single clean line at `r·f` with no
// warble; when `q` is ODD the line at `r·f` is fully SUPPRESSED and the energy
// splits into two sidebands. "A measurement that ignores it will read a correct
// implementation as broken."
//
// So the tone and the window are chosen together, not independently:
//
//   * 3 kHz with the default 40 ms window gives q = 120, EVEN. Every ratio test
//     below therefore looks for one clean line at `r · 3000`.
//   * The window-realization test deliberately uses an 11 ms window, where
//     q = 33 is ODD, precisely so the carrier vanishes — that is what proves a
//     registration-time realization reaches the audio.
//
// `q` depends on the INPUT frequency and the window only, never on the ratio,
// so a clean-carrier tone stays clean at every shift.
//
// Non-bin-aligned ratios (detune, a detented 7.2 semitones) cannot be read
// coherently, so those use a Hann-windowed DTFT with a ternary search for the
// peak. Ratios are always measured as `peak_hz / tone_hz` — a RATIO against the
// input fundamental, which is the thing that distinguishes a pitch shifter from
// a frequency shifter, and the two-tone case below turns that into a test that
// can actually tell them apart.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/forge_pitch_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>
#include <vector>

using namespace pulp::host;
namespace whammy = pulp::host::pitch::whammy;
namespace harmony = pulp::host::pitch::harmony;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using pulp::test::harmonic_magnitude;
using pulp::test::immediate;
using pulp::test::sine_block;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;
constexpr double kToneHz = 3000.0;   // 8 whole periods per block; q = 120, EVEN
constexpr float kAmplitude = 0.5f;
constexpr double kPi = 3.14159265358979323846;

using Shifter = pulp::signal::PitchShifter;
using Fixture = pulp::test::BakedNodeFixture<1>;   // MONO: no cross-channel coupling

/// Blocks rendered and discarded before analysis.
///
/// The node's own delay is the dominant term and it is large: the crossfade
/// window's centre is 960 samples at the default 40 ms window, i.e. 7.5 blocks,
/// before the DC blocker and any glide. 96 blocks is 12288 samples — about
/// 13 window-centres — which lands the output in steady state with room to
/// spare. Every ratio test also injects a zero glide, so the only thing left
/// settling is the line itself.
constexpr int kSettleBlocks = 96;

/// Blocks captured for analysis. 64 blocks is 8192 samples, in which every
/// analysis frequency used here has a whole number of periods.
constexpr int kCaptureBlocks = 64;
constexpr int kCaptureLen = kCaptureBlocks * kFrames;

std::vector<float> tone(double hz = kToneHz, float amp = kAmplitude) {
    return sine_block(kFrames, hz, kSr, amp);
}

/// Two tones an octave apart, for the ratio-versus-offset discriminator.
/// Both land on whole periods per block and both have even `q`.
std::vector<float> two_tone(float amp = 0.35f) {
    auto low = sine_block(kFrames, kToneHz, kSr, amp);
    const auto high = sine_block(kFrames, 2.0 * kToneHz, kSr, amp);
    for (std::size_t i = 0; i < low.size(); ++i) low[i] += high[i];
    return low;
}

/// Applies a batch of immediate parameter events over the production path.
void set_params(ParamInjector& injector,
                std::initializer_list<std::pair<pulp::state::ParamID, float>> values) {
    for (const auto& [id, v] : values)
        REQUIRE(injector.inject(immediate(id, v)) == InjectStatus::Ok);
}

/// Settles, then captures a contiguous analysis window.
std::vector<float> capture(Fixture& fixture, const std::vector<float>& input) {
    const std::vector<std::vector<float>> in{input};
    for (int b = 0; b < kSettleBlocks; ++b) fixture.render(in);

    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(kCaptureLen));
    for (int b = 0; b < kCaptureBlocks; ++b) {
        const auto block = fixture.render(in);
        out.insert(out.end(), block[0].begin(), block[0].end());
    }
    return out;
}

/// Coherent magnitude at a bin-aligned frequency. Exact — the capture holds a
/// whole number of periods of it.
double magnitude_at(const std::vector<float>& x, double hz) {
    return harmonic_magnitude(x, 1, hz, kSr);
}

/// Hann-windowed magnitude at an arbitrary frequency, for the ratios that do
/// not land on a bin.
double windowed_magnitude(const std::vector<float>& x, double hz) {
    const double w = -2.0 * kPi * hz / kSr;
    double re = 0.0, im = 0.0, norm = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double win =
            0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(n) /
                                  static_cast<double>(x.size())));
        const double ph = w * static_cast<double>(n);
        re += win * x[n] * std::cos(ph);
        im += win * x[n] * std::sin(ph);
        norm += win;
    }
    return 2.0 * std::hypot(re, im) / norm;
}

/// Locates an isolated spectral peak by ternary search. The bracket must hold
/// exactly one line.
double peak_hz(const std::vector<float>& x, double lo, double hi) {
    for (int i = 0; i < 120 && (hi - lo) > 1e-6 * hi; ++i) {
        const double a = lo + (hi - lo) / 3.0;
        const double b = hi - (hi - lo) / 3.0;
        if (windowed_magnitude(x, a) > windowed_magnitude(x, b)) hi = b; else lo = a;
    }
    return 0.5 * (lo + hi);
}

/// The shift ratio the node actually produced, measured as a RATIO against the
/// input fundamental. Searches a +/-6 % bracket around the expectation, which
/// at these ratios cannot reach a neighbouring line.
double measured_ratio(const std::vector<float>& x, double expected_ratio) {
    const double centre = kToneHz * expected_ratio;
    return peak_hz(x, centre * 0.94, centre * 1.06) / kToneHz;
}

double ratio_of(double semitones) { return std::pow(2.0, semitones / 12.0); }

/// A fixture with a zero glide and the pedal law under test, ready to measure.
/// Glide is injected to zero because the default 60 ms portamento is a
/// performance feature, not part of the steady state any ratio test is about.
Fixture make_fixture(double window_ms = Shifter::kWindowMsDefault) {
    return Fixture(whammy::make_whammy_node(window_ms), kSr, kFrames);
}

void zero_glide(ParamInjector& injector) {
    set_params(injector, {{whammy::kGlideUpMs, 0.0f}, {whammy::kGlideDownMs, 0.0f}});
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// The load-bearing case: the pedal shifts pitch by a RATIO
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("The pedal shifts pitch by the ratio its position names",
          "[host][forge][pitch][whammy]") {
    // The whole point of the node, over the whole production path. Heel and toe
    // default to 0 and +12 semitones, so the pedal sweeps exactly one octave
    // and the endpoints are the two ratios that land on bins: r = 1 and r = 2.
    auto fixture = make_fixture();
    auto injector = fixture.claim_injector();
    REQUIRE(injector.valid());
    zero_glide(injector);

    // Heel: unison. The line stays where the input put it.
    set_params(injector, {{whammy::kPedal, 0.0f}});
    {
        const auto out = capture(fixture, tone());
        REQUIRE_THAT(measured_ratio(out, 1.0), WithinRel(1.0, 1e-3));
        REQUIRE(magnitude_at(out, kToneHz) > 0.1);
    }

    // Toe: an octave up. Measured as a ratio against the input fundamental.
    set_params(injector, {{whammy::kPedal, 1.0f}});
    {
        const auto out = capture(fixture, tone());
        REQUIRE_THAT(measured_ratio(out, 2.0), WithinRel(2.0, 1e-3));

        // And the input's own line is gone — the whammy default is dry-muted,
        // so a shifter that merely ADDED an octave would still pass a
        // "there is energy at 6 kHz" check.
        const double shifted = magnitude_at(out, 2.0 * kToneHz);
        const double original = magnitude_at(out, kToneHz);
        REQUIRE(shifted > 0.1);
        REQUIRE(20.0 * std::log10(original / shifted) < -30.0);
    }

    // Mid-pedal: +6 semitones, a ratio that lands nowhere near a bin. This is
    // the case that proves the law is continuous rather than a two-position
    // switch.
    set_params(injector, {{whammy::kPedal, 0.5f}});
    {
        const auto out = capture(fixture, tone());
        REQUIRE_THAT(measured_ratio(out, ratio_of(6.0)),
                     WithinRel(ratio_of(6.0), 2e-3));
    }
}

TEST_CASE("The shift is a ratio and not a frequency offset",
          "[host][forge][pitch][whammy]") {
    // The discriminator. A frequency SHIFTER moves every partial by the same
    // number of Hz; a pitch shifter multiplies every partial by the same ratio.
    // With an input of 3 kHz + 6 kHz shifted an octave up:
    //
    //   ratio behaviour  -> 6 kHz and 12 kHz
    //   offset behaviour -> 6 kHz and  9 kHz   (both moved by +3 kHz)
    //
    // The 6 kHz line is common to both, which is why a single-tone test cannot
    // tell them apart and this one can. Both input partials have even `q`
    // (120 and 240), so both shift to clean lines.
    auto fixture = make_fixture();
    auto injector = fixture.claim_injector();
    zero_glide(injector);
    set_params(injector, {{whammy::kPedal, 1.0f}});

    const auto out = capture(fixture, two_tone());

    const double at_6k = magnitude_at(out, 6000.0);
    const double at_9k = magnitude_at(out, 9000.0);
    const double at_12k = magnitude_at(out, 12000.0);

    REQUIRE(at_6k > 0.05);
    REQUIRE(at_12k > 0.05);
    // The ratio image is present and the offset image is not, by a wide margin.
    REQUIRE(20.0 * std::log10(at_9k / at_12k) < -30.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// The realization: the crossfade window is frozen at registration
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("The crossfade window is a realization and not an injectable param",
          "[host][forge][pitch][whammy][realization]") {
    // The rule: anything that moves `latency_samples()` is frozen at
    // registration, because a node whose reported latency changes under the
    // audio thread breaks host delay compensation. `window_ms` IS this node's
    // latency, so it is a construction argument.
    //
    // Half one — it really does move the latency, so the rule really does bite.
    REQUIRE(whammy::whammy_latency_samples(kSr, 40.0) == 960);
    REQUIRE(whammy::make_whammy_node(40.0).latency_samples(kSr) == 960);
    REQUIRE(whammy::make_whammy_node(40.0).type_id == whammy::kTypeId);
    REQUIRE(whammy::make_whammy_node(80.0).type_id != whammy::kTypeId);
    REQUIRE(whammy::make_whammy_node(80.0).type_id ==
            whammy::make_whammy_node(80.0).type_id);
    REQUIRE(whammy::make_whammy_node(1.0).type_id ==
            whammy::make_whammy_node(Shifter::kWindowMsMin).type_id);
    REQUIRE(whammy::whammy_latency_samples(kSr, 80.0) == 1920);
    REQUIRE(whammy::whammy_latency_samples(kSr, Shifter::kWindowMsMin) !=
            whammy::whammy_latency_samples(kSr, Shifter::kWindowMsMax));
    // Computed from the DSP's own expression, not a literal restated here.
    for (double ms : {10.0, 25.0, 40.0, 63.0, 100.0}) {
        Shifter probe;
        probe.prepare(kSr);
        probe.set_window_ms(ms);
        REQUIRE(whammy::whammy_latency_samples(kSr, ms) == probe.latency_samples());
    }

    // Half two — no injectable param can reach it. Enumerated rather than
    // eyeballed, so adding a window param later fails this case.
    const auto node = whammy::make_whammy_node();
    const pulp::state::ParamID expected[] = {
        whammy::kPedal,          whammy::kPedalMode,      whammy::kShiftSource,
        whammy::kShiftSemitones, whammy::kHeelSemis,      whammy::kToeSemis,
        whammy::kIntervalASemis, whammy::kIntervalBSemis, whammy::kDetuneCents,
        whammy::kDiveFloorSemis, whammy::kGlideUpMs,      whammy::kGlideDownMs,
        whammy::kMix,            whammy::kDetents,        whammy::kInterp,
        whammy::kDriftDepth,
    };
    REQUIRE(node.baked_params.size() == std::size(expected));
    for (const auto& row : node.baked_params) {
        bool known = false;
        for (auto id : expected)
            if (row.id == id) known = true;
        REQUIRE(known);
    }
}

TEST_CASE("The window realization reaches the audio",
          "[host][forge][pitch][whammy][realization]") {
    // A realization that no test can hear is indistinguishable from a dead
    // argument. This one is audible in the sharpest way the DSP header
    // describes: `q = f · window_ms / 1000` decides whether the two taps are in
    // phase for a given partial.
    //
    //   40 ms -> q = 120, EVEN -> the taps sum -> ONE clean line at r·f
    //   11 ms -> q =  33, ODD  -> the taps cancel -> the line at r·f VANISHES
    //                             and the energy splits into two sidebands at
    //                             r·f +/- f_warble
    //
    // Same tone, same pedal, same everything else. Only the construction
    // argument differs.
    const double even_window = 40.0;
    const double odd_window = 11.0;

    Shifter probe;
    probe.prepare(kSr);
    probe.set_window_ms(even_window);
    REQUIRE_THAT(probe.tap_phase_pi(kToneHz), WithinAbs(120.0, 1e-9));
    probe.set_window_ms(odd_window);
    REQUIRE_THAT(probe.tap_phase_pi(kToneHz), WithinAbs(33.0, 1e-9));

    const auto carrier_and_sidebands = [](double window_ms) {
        auto fixture = make_fixture(window_ms);
        auto injector = fixture.claim_injector();
        zero_glide(injector);
        set_params(injector, {{whammy::kPedal, 1.0f}});
        const auto out = capture(fixture, tone());

        // f_warble = |1 - r| * 1000 / window_ms, from the shipped law.
        const double warble = std::abs(1.0 - 2.0) * 1000.0 / window_ms;
        const double carrier = windowed_magnitude(out, 2.0 * kToneHz);
        const double side = 0.5 * (windowed_magnitude(out, 2.0 * kToneHz - warble) +
                                   windowed_magnitude(out, 2.0 * kToneHz + warble));
        return std::pair{carrier, side};
    };

    const auto [even_carrier, even_side] = carrier_and_sidebands(even_window);
    const auto [odd_carrier, odd_side] = carrier_and_sidebands(odd_window);

    // Even q: the carrier dominates its own sidebands.
    REQUIRE(even_carrier > 0.1);
    REQUIRE(20.0 * std::log10(even_side / even_carrier) < -20.0);

    // Odd q: the carrier is suppressed and the sidebands carry the energy.
    REQUIRE(odd_side > 0.1);
    REQUIRE(20.0 * std::log10(odd_carrier / odd_side) < -20.0);

    // Which is a large, unmistakable difference between two nodes that differ
    // only in a construction argument.
    REQUIRE(20.0 * std::log10(odd_carrier / even_carrier) < -20.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Every injectable param moves the baked node's audio
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("The pedal mode selects which law the pedal drives",
          "[host][forge][pitch][whammy][params]") {
    // `pedal_mode` is injectable — it changes the LAW, not the topology or the
    // latency. Proved by holding the pedal still and changing only the mode:
    // each mode's law puts the same pedal position at a different ratio.
    auto fixture = make_fixture();
    auto injector = fixture.claim_injector();
    zero_glide(injector);

    // Harmony A/B set to +12 and -12 so both endpoints land on exact ratios.
    set_params(injector, {{whammy::kIntervalASemis, 12.0f},
                          {whammy::kIntervalBSemis, -12.0f},
                          {whammy::kDiveFloorSemis, -24.0f},
                          {whammy::kDetuneCents, static_cast<float>(Shifter::kDetuneCentsMax)}});

    // Whammy at heel: unison.
    set_params(injector, {{whammy::kPedalMode, whammy::kModeWhammy},
                          {whammy::kPedal, 0.0f}});
    REQUIRE_THAT(measured_ratio(capture(fixture, tone()), 1.0), WithinRel(1.0, 1e-3));

    // Harmony at heel: interval A, an octave up. SAME pedal position.
    set_params(injector, {{whammy::kPedalMode, whammy::kModeHarmony}});
    REQUIRE_THAT(measured_ratio(capture(fixture, tone()), 2.0), WithinRel(2.0, 1e-3));

    // Harmony at toe: interval B, an octave down.
    set_params(injector, {{whammy::kPedal, 1.0f}});
    REQUIRE_THAT(measured_ratio(capture(fixture, tone()), 0.5), WithinRel(0.5, 1e-3));

    // Dive at toe: the floor, here set to -24 semitones so r = 1/4 exactly.
    set_params(injector, {{whammy::kPedalMode, whammy::kModeDive}});
    REQUIRE_THAT(measured_ratio(capture(fixture, tone()), 0.25), WithinRel(0.25, 1e-3));

    // Detune at toe: a sub-semitone offset. 50 cents is the parameter's
    // ceiling, and it lands nowhere near a bin — which is why the ratio search
    // exists.
    set_params(injector, {{whammy::kPedalMode, whammy::kModeDetune}});
    {
        const double expected = ratio_of(Shifter::kDetuneCentsMax / 100.0);
        REQUIRE_THAT(measured_ratio(capture(fixture, tone()), expected),
                     WithinRel(expected, 2e-3));
        // Genuinely a detune rather than a rounding error: 50 cents is a
        // semitone's half, plainly distinct from unison.
        REQUIRE(expected > 1.02);
    }
}

TEST_CASE("The heel and toe endpoints set the whammy sweep",
          "[host][forge][pitch][whammy][params]") {
    auto fixture = make_fixture();
    auto injector = fixture.claim_injector();
    zero_glide(injector);

    // A downward whammy: heel unison, toe an octave DOWN. The same pedal
    // position that gave r = 2 above now gives r = 0.5.
    set_params(injector, {{whammy::kHeelSemis, 0.0f},
                          {whammy::kToeSemis, -12.0f},
                          {whammy::kPedal, 1.0f}});
    REQUIRE_THAT(measured_ratio(capture(fixture, tone()), 0.5), WithinRel(0.5, 1e-3));

    // And a heel that is not unison: the pedal at rest already transposes.
    set_params(injector, {{whammy::kHeelSemis, 12.0f},
                          {whammy::kToeSemis, 12.0f},
                          {whammy::kPedal, 0.0f}});
    REQUIRE_THAT(measured_ratio(capture(fixture, tone()), 2.0), WithinRel(2.0, 1e-3));
}

TEST_CASE("The direct shift source bypasses the pedal law",
          "[host][forge][pitch][whammy][params]") {
    // The selector exists so a host re-sending both automation lanes cannot
    // flip the block back and forth. Proved by pinning the pedal at heel, where
    // the pedal law says unison, and asserting the direct target wins.
    auto fixture = make_fixture();
    auto injector = fixture.claim_injector();
    zero_glide(injector);
    set_params(injector, {{whammy::kPedal, 0.0f}, {whammy::kShiftSemitones, 12.0f}});

    set_params(injector, {{whammy::kShiftSource, whammy::kSourcePedal}});
    REQUIRE_THAT(measured_ratio(capture(fixture, tone()), 1.0), WithinRel(1.0, 1e-3));

    set_params(injector, {{whammy::kShiftSource, whammy::kSourceDirect}});
    REQUIRE_THAT(measured_ratio(capture(fixture, tone()), 2.0), WithinRel(2.0, 1e-3));

    // The direct target is continuous too, not just the octave.
    set_params(injector, {{whammy::kShiftSemitones, -5.0f}});
    REQUIRE_THAT(measured_ratio(capture(fixture, tone()), ratio_of(-5.0)),
                 WithinRel(ratio_of(-5.0), 2e-3));
}

TEST_CASE("Detents snap the pedal target to musical intervals",
          "[host][forge][pitch][whammy][params]") {
    // A pedal position whose raw target is 7.2 semitones — 0.2 inside the
    // 0.35-semitone capture band around the +7 detent. With detents off the
    // node plays 7.2; with them on it plays exactly 7. The two ratios differ by
    // 1.2 %, comfortably above the search's precision.
    auto fixture = make_fixture();
    auto injector = fixture.claim_injector();
    zero_glide(injector);

    const double raw_semis = 7.2;
    const double pedal = raw_semis / Shifter::kToeSemisDefault;
    REQUIRE(std::abs(raw_semis - 7.0) < Shifter::kDetentSnapSemis);

    set_params(injector, {{whammy::kPedal, static_cast<float>(pedal)}});

    set_params(injector, {{whammy::kDetents, 0.0f}});
    const double loose = measured_ratio(capture(fixture, tone()), ratio_of(raw_semis));
    REQUIRE_THAT(loose, WithinRel(ratio_of(raw_semis), 2e-3));

    set_params(injector, {{whammy::kDetents, 1.0f}});
    const double snapped = measured_ratio(capture(fixture, tone()), ratio_of(7.0));
    REQUIRE_THAT(snapped, WithinRel(ratio_of(7.0), 2e-3));

    // The snap is a real move, not a tolerance overlap.
    REQUIRE(std::abs(loose - snapped) / snapped > 0.005);

    // Detents are inert in the modes whose targets are not intervals — the DSP
    // header's `mode_uses_detents`. Detune's default target sits three times
    // INSIDE the capture band, so a literal reading would snap every detune
    // setting to unison and make the mode do nothing.
    REQUIRE(Shifter::mode_uses_detents(pulp::signal::PedalMode::whammy));
    REQUIRE(Shifter::mode_uses_detents(pulp::signal::PedalMode::harmony));
    REQUIRE_FALSE(Shifter::mode_uses_detents(pulp::signal::PedalMode::detune));
    REQUIRE_FALSE(Shifter::mode_uses_detents(pulp::signal::PedalMode::dive));

    set_params(injector, {{whammy::kPedalMode, whammy::kModeDetune},
                          {whammy::kDetuneCents, static_cast<float>(Shifter::kDetuneCentsMax)},
                          {whammy::kPedal, 1.0f},
                          {whammy::kDetents, 1.0f}});
    const double expected = ratio_of(Shifter::kDetuneCentsMax / 100.0);
    REQUIRE_THAT(measured_ratio(capture(fixture, tone()), expected),
                 WithinRel(expected, 2e-3));
}

TEST_CASE("The mix param blends dry against the shifted leg",
          "[host][forge][pitch][whammy][params]") {
    auto fixture = make_fixture();
    auto injector = fixture.claim_injector();
    zero_glide(injector);
    set_params(injector, {{whammy::kPedal, 1.0f}});   // an octave up

    // Fully dry: the node is a wire, and the octave is not there at all.
    set_params(injector, {{whammy::kMix, 0.0f}});
    {
        const auto out = capture(fixture, tone());
        REQUIRE_THAT(magnitude_at(out, kToneHz),
                     WithinRel(static_cast<double>(kAmplitude), 1e-3));
        REQUIRE(20.0 * std::log10(magnitude_at(out, 2.0 * kToneHz) /
                                  magnitude_at(out, kToneHz)) < -60.0);
    }

    // Fully wet: only the octave.
    set_params(injector, {{whammy::kMix, 1.0f}});
    {
        const auto out = capture(fixture, tone());
        REQUIRE(20.0 * std::log10(magnitude_at(out, kToneHz) /
                                  magnitude_at(out, 2.0 * kToneHz)) < -30.0);
    }

    // Halfway: both present, at the equal-power gains the DSP applies. The two
    // legs are at different frequencies, so each one's magnitude is its own
    // gain times the input amplitude with no interference term.
    set_params(injector, {{whammy::kMix, 0.5f}});
    {
        const auto out = capture(fixture, tone());
        const double equal_power = std::cos(0.25 * kPi);   // both legs at mix 0.5
        REQUIRE_THAT(magnitude_at(out, kToneHz),
                     WithinRel(equal_power * kAmplitude, 0.02));
        REQUIRE_THAT(magnitude_at(out, 2.0 * kToneHz),
                     WithinRel(equal_power * kAmplitude, 0.05));
    }

    // The node's registered default is the whammy convention (dry muted), and
    // it comes from the DSP's own table rather than from a literal here.
    const auto node = whammy::make_whammy_node();
    for (const auto& row : node.baked_params)
        if (row.id == whammy::kMix)
            REQUIRE_THAT(row.default_value,
                         WithinRel(whammy::whammy_default_mix_for_mode(
                                       pulp::signal::PedalMode::whammy),
                                   1e-6f));
}

TEST_CASE("Glide slows a pedal move without changing where it arrives",
          "[host][forge][pitch][whammy][params]") {
    // Portamento is a performance control: it must change WHEN the target is
    // reached and not WHAT it is.
    const auto energy_after_jump = [](float glide_ms, int blocks) {
        auto fixture = make_fixture();
        auto injector = fixture.claim_injector();
        set_params(injector, {{whammy::kGlideUpMs, glide_ms},
                              {whammy::kGlideDownMs, glide_ms},
                              {whammy::kPedal, 0.0f}});
        const std::vector<std::vector<float>> in{tone()};
        for (int b = 0; b < kSettleBlocks; ++b) fixture.render(in);

        // Jump the pedal to the toe, then listen to the transition only.
        set_params(injector, {{whammy::kPedal, 1.0f}});
        std::vector<float> out;
        for (int b = 0; b < blocks; ++b) {
            const auto block = fixture.render(in);
            out.insert(out.end(), block[0].begin(), block[0].end());
        }
        return windowed_magnitude(out, 2.0 * kToneHz);
    };

    // 40 blocks is 5120 samples, about 107 ms — long enough for a zero glide to
    // have arrived and settled, far short of a 2-second one.
    constexpr int kTransitionBlocks = 40;
    const double fast = energy_after_jump(0.0f, kTransitionBlocks);
    const double slow = energy_after_jump(static_cast<float>(Shifter::kGlideMsMax),
                                          kTransitionBlocks);
    REQUIRE(fast > slow);
    REQUIRE(20.0 * std::log10(slow / fast) < -12.0);

    // But it arrives at the same place once it gets there.
    auto fixture = make_fixture();
    auto injector = fixture.claim_injector();
    set_params(injector, {{whammy::kGlideUpMs, 200.0f},
                          {whammy::kGlideDownMs, 200.0f},
                          {whammy::kPedal, 1.0f}});
    REQUIRE_THAT(measured_ratio(capture(fixture, tone()), 2.0), WithinRel(2.0, 1e-3));
}

TEST_CASE("The interpolation param changes the tap without changing the ratio",
          "[host][forge][pitch][whammy][params]") {
    // `interp` injects rather than being frozen because it changes the read
    // STENCIL, not the structure — `latency_samples()` reports the window
    // centre and does not depend on it. So the two settings must differ
    // audibly while landing on the same pitch.
    //
    // WHICH RATIO IS CHOSEN MATTERS, and not for a reason that is obvious. The
    // delay ramps at `1 − r` samples per sample, so at r = 2 it steps by
    // exactly −1.0 and EVERY read lands on a sample: both interpolants return
    // that sample and the two settings are bit-identical. Same at r = 4
    // (−3.0) and trivially at r = 1 (no ramp at all). Measuring this param at
    // an octave would read a perfectly working node as a dead knob — which is
    // exactly what the first draft of this case did.
    const auto render_semitones = [](float interp, float semitones) {
        auto fixture = make_fixture();
        auto injector = fixture.claim_injector();
        zero_glide(injector);
        set_params(injector, {{whammy::kShiftSource, whammy::kSourceDirect},
                              {whammy::kShiftSemitones, semitones},
                              {whammy::kInterp, interp}});
        return capture(fixture, tone());
    };

    // First, that property itself, since a future reader will otherwise
    // rediscover it the hard way: at a whole-sample ramp the interpolant is
    // unobservable.
    for (float whole_step_semis : {12.0f, 24.0f, 0.0f}) {
        const auto a = render_semitones(whammy::kInterpLinear, whole_step_semis);
        const auto b = render_semitones(whammy::kInterpCubic, whole_step_semis);
        for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
    }

    // Now at +7 semitones, where the ramp is −0.4983 samples per sample and
    // every read is genuinely fractional.
    constexpr float kFractionalSemis = 7.0f;
    const auto linear = render_semitones(whammy::kInterpLinear, kFractionalSemis);
    const auto cubic = render_semitones(whammy::kInterpCubic, kFractionalSemis);

    bool differs = false;
    for (std::size_t i = 0; i < linear.size(); ++i)
        if (linear[i] != cubic[i]) differs = true;
    REQUIRE(differs);

    // The same pitch either way — a read-quality control, not a tuning one.
    const double expected = ratio_of(kFractionalSemis);
    REQUIRE_THAT(measured_ratio(linear, expected), WithinRel(expected, 2e-3));
    REQUIRE_THAT(measured_ratio(cubic, expected), WithinRel(expected, 2e-3));

    // And it does NOT move the reported latency, which is what allows it to be
    // a param at all.
    Shifter probe;
    probe.prepare(kSr);
    const int before = probe.latency_samples();
    probe.set_interp(pulp::signal::PitchInterp::cubic);
    REQUIRE(probe.latency_samples() == before);
}

TEST_CASE("Drift depth is off by default and wobbles the pitch when raised",
          "[host][forge][pitch][whammy][params]") {
    const auto render_with = [](float depth) {
        auto fixture = make_fixture();
        auto injector = fixture.claim_injector();
        zero_glide(injector);
        set_params(injector, {{whammy::kPedal, 1.0f}, {whammy::kDriftDepth, depth}});
        return capture(fixture, tone());
    };

    const auto still = render_with(0.0f);
    const auto drifting = render_with(1.0f);

    bool differs = false;
    for (std::size_t i = 0; i < still.size(); ++i)
        if (still[i] != drifting[i]) differs = true;
    REQUIRE(differs);

    // At zero the generator is not advanced at all, so the render is
    // reproducible bit for bit — series law 2, over the baked path.
    const auto again = render_with(0.0f);
    for (std::size_t i = 0; i < still.size(); ++i) REQUIRE(still[i] == again[i]);

    // Seeded, so even the drifting render repeats exactly.
    const auto drifting_again = render_with(1.0f);
    for (std::size_t i = 0; i < drifting.size(); ++i)
        REQUIRE(drifting[i] == drifting_again[i]);

    // The wobble is small — 0.5 % of the ratio at full depth, about 8.6 cents —
    // so the line stays where it was, just less sharply.
    REQUIRE_THAT(measured_ratio(drifting, 2.0), WithinRel(2.0, 0.01));
    REQUIRE(whammy::make_whammy_node().baked_params.back().default_value == 0.0f);
}

// ═══════════════════════════════════════════════════════════════════════════
// Registry facts
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("The worst-case gain cites the invariant the DSP suite asserts",
          "[host][forge][pitch][whammy][registry]") {
    // Series law 8: the registry number must be a bound the module's own suite
    // asserts, not an estimate. That suite sweeps the mix and asserts
    // `worst <= kWorstCaseGain * dc_blocker_peak_gain()` — this is the same
    // expression, read from the same shipped accessor.
    Shifter probe;
    probe.prepare(kSr);
    const double expected = Shifter::kWorstCaseGain;
    REQUIRE_THAT(static_cast<double>(whammy::whammy_worst_case_gain(kSr)),
                 WithinRel(expected, 1e-6));

    // Neither factor may be dropped. √2 is the topology; the DC blocker's
    // The wet leg's bound is the DC blocker's TIME-DOMAIN peak — the L1 norm
    // of its impulse response, exactly 2 — not its magnitude peak at Nyquist
    // (1.000327), which bounds the response to a steady sinusoid and says
    // nothing about the largest single sample.
    REQUIRE_THAT(Shifter::kWorstCaseGain, WithinRel(std::sqrt(5.0), 1e-12));
    REQUIRE_THAT(Shifter::kDcBlockerPeakGain, WithinAbs(2.0, 1e-12));
    // Rate-INDEPENDENT, derived rather than simplified: the L1 norm is 2 for
    // every pole position, so the 5 Hz corner — and hence the sample rate —
    // drops out. This asserted the OPPOSITE while the bound was built from the
    // rate-dependent magnitude peak, so the wrong bound had a passing test
    // certifying a property only the wrong derivation had.
    REQUIRE_THAT(static_cast<double>(whammy::whammy_worst_case_gain(44100.0)),
                 WithinRel(static_cast<double>(whammy::whammy_worst_case_gain(96000.0)), 1e-9));

    // And the baked node honours it. The bound is over the whole mix sweep,
    // which is where the equal-power sum peaks.
    for (float mix : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        auto fixture = make_fixture();
        auto injector = fixture.claim_injector();
        zero_glide(injector);
        set_params(injector, {{whammy::kPedal, 1.0f}, {whammy::kMix, mix}});
        const auto out = capture(fixture, tone(kToneHz, 1.0f));
        double peak = 0.0;
        for (float v : out) peak = std::max(peak, std::abs(static_cast<double>(v)));
        REQUIRE(peak <= whammy::whammy_worst_case_gain(kSr) + 1e-4);
    }
}

TEST_CASE("Every baked param row mirrors the DSP's canonical range",
          "[host][forge][pitch][whammy][registry]") {
    // The node's table is the module's contract restated for the host. If a DSP
    // clamp moves and the row does not, a host knob travels somewhere the block
    // silently pulls it back from — which reads as a broken automation lane.
    const auto node = whammy::make_whammy_node();
    const auto row = [&](pulp::state::ParamID id) {
        for (const auto& r : node.baked_params)
            if (r.id == id) return r;
        FAIL("missing param row");
        return node.baked_params.front();
    };

    using S = Shifter;
    REQUIRE_THAT(row(whammy::kShiftSemitones).min_value,
                 WithinRel(static_cast<float>(S::kShiftSemisMin), 1e-6f));
    REQUIRE_THAT(row(whammy::kShiftSemitones).max_value,
                 WithinRel(static_cast<float>(S::kShiftSemisMax), 1e-6f));
    REQUIRE_THAT(row(whammy::kToeSemis).default_value,
                 WithinRel(static_cast<float>(S::kToeSemisDefault), 1e-6f));
    REQUIRE_THAT(row(whammy::kHeelSemis).default_value,
                 WithinAbs(static_cast<float>(S::kHeelSemisDefault), 1e-6f));
    REQUIRE_THAT(row(whammy::kIntervalASemis).default_value,
                 WithinRel(static_cast<float>(S::kIntervalASemisDefault), 1e-6f));
    REQUIRE_THAT(row(whammy::kIntervalBSemis).default_value,
                 WithinRel(static_cast<float>(S::kIntervalBSemisDefault), 1e-6f));
    REQUIRE_THAT(row(whammy::kDetuneCents).max_value,
                 WithinRel(static_cast<float>(S::kDetuneCentsMax), 1e-6f));
    REQUIRE_THAT(row(whammy::kDetuneCents).default_value,
                 WithinRel(static_cast<float>(S::kDetuneCentsDefault), 1e-6f));
    REQUIRE_THAT(row(whammy::kDiveFloorSemis).min_value,
                 WithinRel(static_cast<float>(S::kDiveFloorSemisMin), 1e-6f));
    REQUIRE_THAT(row(whammy::kDiveFloorSemis).max_value,
                 WithinRel(static_cast<float>(S::kDiveFloorSemisMax), 1e-6f));
    REQUIRE_THAT(row(whammy::kDiveFloorSemis).default_value,
                 WithinRel(static_cast<float>(S::kDiveFloorSemisDefault), 1e-6f));
    REQUIRE_THAT(row(whammy::kGlideUpMs).max_value,
                 WithinRel(static_cast<float>(S::kGlideMsMax), 1e-6f));
    REQUIRE_THAT(row(whammy::kGlideUpMs).default_value,
                 WithinRel(static_cast<float>(S::kGlideMsDefault), 1e-6f));

    // Structure: mono, lowerable, and a stable type id a saved graph depends on.
    REQUIRE(node.num_input_ports == 1);
    REQUIRE(node.num_output_ports == 1);
    REQUIRE(node.lowerable);
    REQUIRE(node.type_id == std::string("pitch.whammy"));
    REQUIRE(node.version == 1);

    // Every row's default sits inside its own range — trivial to get wrong when
    // a range is written from one constant and a default from another.
    for (const auto& r : node.baked_params) {
        REQUIRE(r.min_value <= r.default_value);
        REQUIRE(r.default_value <= r.max_value);
    }
}

TEST_CASE("The stepped encodings decode to the modes they name",
          "[host][forge][pitch][whammy][registry]") {
    using pulp::signal::PedalMode;
    REQUIRE(whammy::decode_pedal_mode(whammy::kModeWhammy) == PedalMode::whammy);
    REQUIRE(whammy::decode_pedal_mode(whammy::kModeHarmony) == PedalMode::harmony);
    REQUIRE(whammy::decode_pedal_mode(whammy::kModeDetune) == PedalMode::detune);
    REQUIRE(whammy::decode_pedal_mode(whammy::kModeDive) == PedalMode::dive);

    // Rounds rather than truncates, so a host sending 0.9999 for "harmony" gets
    // harmony. Truncation would silently give whammy for every value a
    // normalised automation lane lands just below.
    REQUIRE(whammy::decode_pedal_mode(0.9999f) == PedalMode::harmony);
    REQUIRE(whammy::decode_pedal_mode(2.5001f) == PedalMode::dive);
    // And it is total: out-of-range values clamp instead of indexing off the end.
    REQUIRE(whammy::decode_pedal_mode(-7.0f) == PedalMode::whammy);
    REQUIRE(whammy::decode_pedal_mode(99.0f) == PedalMode::dive);

    // The per-mode conventional mixes, for the preset layer. Deliberately NOT
    // applied when the mode param moves — a mode change mid-performance must
    // not jump the level under the player's foot.
    REQUIRE_THAT(whammy::whammy_default_mix_for_mode(PedalMode::whammy),
                 WithinRel(1.0f, 1e-6f));
    REQUIRE_THAT(whammy::whammy_default_mix_for_mode(PedalMode::dive),
                 WithinRel(1.0f, 1e-6f));
    REQUIRE_THAT(whammy::whammy_default_mix_for_mode(PedalMode::harmony),
                 WithinRel(0.5f, 1e-6f));
    REQUIRE_THAT(whammy::whammy_default_mix_for_mode(PedalMode::detune),
                 WithinRel(0.4f, 1e-6f));
}

TEST_CASE("Changing the pedal mode does not jump the mix",
          "[host][forge][pitch][whammy][params]") {
    // The corollary of the note above, asserted over the audio path: switching
    // from whammy (conventional mix 1.0) to harmony (conventional 0.5) must
    // leave the level alone, because `pedal_mode` is an automatable param and
    // the conventional defaults belong to presets.
    auto fixture = make_fixture();
    auto injector = fixture.claim_injector();
    zero_glide(injector);
    // Both modes pointed at the same interval so only the mix could differ.
    set_params(injector, {{whammy::kPedal, 0.0f},
                          {whammy::kHeelSemis, 12.0f},
                          {whammy::kIntervalASemis, 12.0f},
                          {whammy::kPedalMode, whammy::kModeWhammy}});
    const auto as_whammy = capture(fixture, tone());

    set_params(injector, {{whammy::kPedalMode, whammy::kModeHarmony}});
    const auto as_harmony = capture(fixture, tone());

    REQUIRE_THAT(magnitude_at(as_harmony, 2.0 * kToneHz),
                 WithinRel(magnitude_at(as_whammy, 2.0 * kToneHz), 1e-3));
}

// ═══════════════════════════════════════════════════════════════════════════
// The RT contract
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("The baked node allocates nothing while rendering",
          "[host][forge][pitch][whammy][rt]") {
    // `ReusableRenderer` rather than the fixture's convenience `render()`,
    // which builds its own output vectors — a probe would attribute those to
    // the node and the suite would blame the DSP for the harness.
    auto fixture = make_fixture();
    auto injector = fixture.claim_injector();
    const std::vector<std::vector<float>> in{tone()};
    pulp::test::ReusableRenderer<1> renderer(fixture, in);

    // Warm the node outside the probe: the first render is where the executor
    // touches whatever it lazily initialises.
    for (int b = 0; b < 8; ++b) renderer.render();

    // Every param, at a value away from its default, injected and rendered
    // inside the probe.
    const auto events = {
        immediate(whammy::kPedal, 0.6f),
        immediate(whammy::kPedalMode, whammy::kModeHarmony),
        immediate(whammy::kShiftSource, whammy::kSourceDirect),
        immediate(whammy::kShiftSemitones, -7.0f),
        immediate(whammy::kHeelSemis, -3.0f),
        immediate(whammy::kToeSemis, 9.0f),
        immediate(whammy::kIntervalASemis, 4.0f),
        immediate(whammy::kIntervalBSemis, -5.0f),
        immediate(whammy::kDetuneCents, 21.0f),
        immediate(whammy::kDiveFloorSemis, -36.0f),
        immediate(whammy::kGlideUpMs, 12.0f),
        immediate(whammy::kGlideDownMs, 90.0f),
        immediate(whammy::kMix, 0.4f),
        immediate(whammy::kDetents, 1.0f),
        immediate(whammy::kInterp, whammy::kInterpCubic),
        immediate(whammy::kDriftDepth, 0.7f),
    };

    {
        pulp::test::RtAllocationProbe probe;
        for (const auto& e : events) injector.inject(e);
        for (int b = 0; b < 16; ++b) renderer.render();
        REQUIRE(probe.allocation_count() == 0);
    }

    // Not vacuous: the renders under the probe actually produced signal.
    double energy = 0.0;
    for (float v : renderer.output()) energy += static_cast<double>(v) * v;
    REQUIRE(energy > 0.0);
}

TEST_CASE("The harmony factory exposes the complete canonical parameter surface",
          "[host][forge][pitch][harmony]") {
    auto node = harmony::make_harmony_engine_node();
    REQUIRE(node.type_id == harmony::kTypeId);
    REQUIRE(node.num_input_ports == 1);
    REQUIRE(node.num_output_ports == 1);
    REQUIRE(node.baked_params.size() == 12);
    REQUIRE(harmony::worst_case_gain() ==
            static_cast<float>(pulp::signal::HarmonyEngine::kWorstCaseGain));
    REQUIRE(harmony::worst_case_gain() > 9.9f);
    REQUIRE(harmony::latency_samples(kSr)>0);
    REQUIRE(node.latency_samples(kSr) == harmony::latency_samples(kSr));
    Fixture fixture(std::move(node), kSr, kFrames);
    auto injector = fixture.claim_injector();
    REQUIRE(injector.inject(immediate(harmony::kKey, 2.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(harmony::kScale, 1.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(harmony::kV1Interval, 2.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(harmony::kV2Enable, 1.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(harmony::kV2Interval, 4.0f)) == InjectStatus::Ok);
    auto input = tone();
    pulp::test::ReusableRenderer<1> renderer(fixture, {input});
    for (int i = 0; i < 64; ++i) renderer.render();
    for (float x : renderer.output()) REQUIRE(std::isfinite(x));
    pulp::test::RtAllocationProbe probe;
    for (int i = 0; i < 16; ++i) renderer.render();
    REQUIRE(probe.allocation_count() == 0);
}
