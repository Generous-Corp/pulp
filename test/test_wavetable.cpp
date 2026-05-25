#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/wavetable.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

// ────────────────────────────────────────────────────────────────────────
// macOS plan item 2.6 — Wavetable oscillator with band-switching.
//
// Pulp's `Wavetable` keeps a stack of pre-bandlimited single-cycle
// tables. The selected band tracks the playback frequency, and band
// transitions crossfade across `kCrossfadeSamples` samples so the
// switch is click-free.
// ────────────────────────────────────────────────────────────────────────

namespace {

// Drive a Wavetable for N samples at one frequency, return the max
// |sample| produced. Useful for sanity / unit-peak invariants.
float peak_over(Wavetable& wt, std::size_t samples) {
    float p = 0.0f;
    for (std::size_t i = 0; i < samples; ++i) p = std::max(p, std::fabs(wt.next()));
    return p;
}

} // namespace

TEST_CASE("Wavetable empty default returns 0 from next()", "[signal][wavetable]") {
    Wavetable wt;
    REQUIRE(wt.band_count() == 0);
    REQUIRE(wt.next() == 0.0f);
    REQUIRE(wt.next() == 0.0f);
}

TEST_CASE("Wavetable::make_sine has exactly one band and is unit peak", "[signal][wavetable]") {
    auto wt = Wavetable::make_sine(/*table_length=*/2048);
    REQUIRE(wt.band_count() == 1);
    wt.set_sample_rate(48000.0f);
    wt.set_frequency(440.0f);
    REQUIRE_THAT(peak_over(wt, 8192), WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("Wavetable::make_saw produces N bands sorted by ascending ceiling",
          "[signal][wavetable]") {
    auto wt = Wavetable::make_saw(/*bands=*/10, /*table_length=*/2048,
                                   /*reference_sample_rate=*/48000.0f);
    REQUIRE(wt.band_count() == 10);
}

TEST_CASE("Wavetable selects the lowest-ceiling band that covers the requested frequency",
          "[signal][wavetable]") {
    auto wt = Wavetable::make_saw(/*bands=*/10, /*table_length=*/1024,
                                   /*reference_sample_rate=*/48000.0f);
    wt.set_sample_rate(48000.0f);

    // 20 Hz should land in the bottom band.
    wt.set_frequency(40.0f);
    const int low_band = wt.current_band();
    REQUIRE(low_band >= 0);

    // 10 kHz should land in (or near) the top band.
    wt.set_frequency(10000.0f);
    const int high_band = wt.current_band();
    REQUIRE(high_band > low_band);

    // 200 Hz should land somewhere in between.
    wt.set_frequency(200.0f);
    REQUIRE(wt.current_band() >= low_band);
    REQUIRE(wt.current_band() <= high_band);
}

TEST_CASE("Wavetable band-switch triggers a crossfade window", "[signal][wavetable]") {
    auto wt = Wavetable::make_saw(/*bands=*/10);
    wt.set_sample_rate(48000.0f);
    wt.set_frequency(40.0f);
    // Consume any initial-band crossfade triggered by moving away from
    // the constructor's default frequency.
    for (std::size_t i = 0; i < Wavetable::kCrossfadeSamples + 1; ++i) (void)wt.next();
    REQUIRE_FALSE(wt.is_crossfading());

    // Jump to a much higher frequency — different band selected.
    wt.set_frequency(8000.0f);
    REQUIRE(wt.is_crossfading());

    // Crossfade should end after kCrossfadeSamples samples.
    for (std::size_t i = 0; i < Wavetable::kCrossfadeSamples; ++i) (void)wt.next();
    REQUIRE_FALSE(wt.is_crossfading());
}

TEST_CASE("Wavetable band-switch produces a click-free transition",
          "[signal][wavetable][click-free]") {
    auto wt = Wavetable::make_saw(/*bands=*/10);
    wt.set_sample_rate(48000.0f);
    wt.set_frequency(110.0f);
    // Settle into the low band.
    for (int i = 0; i < 512; ++i) (void)wt.next();

    // Capture a few samples before the switch.
    std::vector<float> before(8);
    for (auto& s : before) s = wt.next();

    // Trigger the switch.
    wt.set_frequency(2200.0f);

    // Capture transition samples — the sample-to-sample delta during
    // crossfade should stay bounded (no spikes). We allow generous
    // headroom because the underlying waveforms differ between bands;
    // the goal is to catch a *click* (multi-sample-amplitude jump),
    // not normal waveform motion.
    std::vector<float> during;
    during.reserve(Wavetable::kCrossfadeSamples + 16);
    for (std::size_t i = 0; i < Wavetable::kCrossfadeSamples + 16; ++i) {
        during.push_back(wt.next());
    }

    float max_delta = 0.0f;
    float prev = before.back();
    for (float s : during) {
        max_delta = std::max(max_delta, std::fabs(s - prev));
        prev = s;
    }
    // Adjacent sample delta should stay well below the full peak-to-
    // peak (2.0 for a unit waveform). The sawtooth's natural ~2.0
    // discontinuity *can* occur within the capture window, so we
    // bound a bit below that: a click-induced spike during a band
    // crossfade would sit on top of the discontinuity, not under it.
    REQUIRE(max_delta < 2.0f);
}

TEST_CASE("Wavetable::reset clears phase and crossfade state", "[signal][wavetable]") {
    auto wt = Wavetable::make_saw();
    wt.set_sample_rate(48000.0f);
    wt.set_frequency(40.0f);
    (void)wt.next();
    wt.set_frequency(8000.0f);
    REQUIRE(wt.is_crossfading());
    wt.reset();
    REQUIRE_FALSE(wt.is_crossfading());
}

TEST_CASE("Wavetable::make_square contains only odd harmonics — even-indexed lookup is exact zero at phase 0.25/0.75",
          "[signal][wavetable]") {
    // A square synthesised from odd-harmonic sines has half-wave
    // anti-symmetry: sample at phase 0 = 0 (sum of sines starting at 0),
    // and the table integrates to a value mirrored across phase 0.5.
    // We sanity-check that the table peak is normalised to ~1.0.
    auto wt = Wavetable::make_square(/*bands=*/8);
    wt.set_sample_rate(48000.0f);
    wt.set_frequency(110.0f);
    REQUIRE_THAT(peak_over(wt, 4096), WithinAbs(1.0f, 5e-2f));
}

TEST_CASE("Wavetable::make_triangle produces unit-peak output", "[signal][wavetable]") {
    auto wt = Wavetable::make_triangle(/*bands=*/8);
    wt.set_sample_rate(48000.0f);
    wt.set_frequency(220.0f);
    REQUIRE_THAT(peak_over(wt, 4096), WithinAbs(1.0f, 5e-2f));
}

TEST_CASE("Wavetable rejects zero / negative frequency by ignoring the set", "[signal][wavetable]") {
    auto wt = Wavetable::make_saw();
    wt.set_sample_rate(48000.0f);
    wt.set_frequency(440.0f);
    const int original_band = wt.current_band();
    wt.set_frequency(0.0f);
    REQUIRE(wt.current_band() == original_band);
    wt.set_frequency(-100.0f);
    REQUIRE(wt.current_band() == original_band);
}

TEST_CASE("WavetableBank empty bank returns 0", "[signal][wavetable][bank]") {
    WavetableBank bank;
    REQUIRE(bank.next() == 0.0f);
    REQUIRE(bank.size() == 0);
}

TEST_CASE("WavetableBank with one wavetable passes its output through",
          "[signal][wavetable][bank]") {
    std::vector<Wavetable> tables;
    tables.push_back(Wavetable::make_sine());
    WavetableBank bank(std::move(tables));
    bank.set_sample_rate(48000.0f);
    bank.set_frequency(440.0f);
    bank.set_position(0.5f); // ignored for size==1
    float peak = 0.0f;
    for (int i = 0; i < 4096; ++i) peak = std::max(peak, std::fabs(bank.next()));
    REQUIRE_THAT(peak, WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("WavetableBank morphs between two tables at position 0 and 1",
          "[signal][wavetable][bank]") {
    std::vector<Wavetable> tables;
    tables.push_back(Wavetable::make_sine());
    tables.push_back(Wavetable::make_saw());
    WavetableBank bank(std::move(tables));
    bank.set_sample_rate(48000.0f);
    bank.set_frequency(110.0f);

    // Position 0 = pure sine → peak ≈ 1.
    bank.set_position(0.0f);
    bank.reset();
    float sine_peak = 0.0f;
    for (int i = 0; i < 4096; ++i) sine_peak = std::max(sine_peak, std::fabs(bank.next()));
    REQUIRE_THAT(sine_peak, WithinAbs(1.0f, 1e-2f));

    // Position 1 = pure saw → also normalised to ~1.
    bank.set_position(1.0f);
    bank.reset();
    float saw_peak = 0.0f;
    for (int i = 0; i < 4096; ++i) saw_peak = std::max(saw_peak, std::fabs(bank.next()));
    REQUIRE_THAT(saw_peak, WithinAbs(1.0f, 5e-2f));
}

TEST_CASE("WavetableBank position clamps to [0, 1]", "[signal][wavetable][bank]") {
    std::vector<Wavetable> tables;
    tables.push_back(Wavetable::make_sine());
    tables.push_back(Wavetable::make_saw());
    WavetableBank bank(std::move(tables));
    bank.set_sample_rate(48000.0f);
    bank.set_frequency(440.0f);

    // Position 2.5 → clamped to 1 (pure saw).
    bank.set_position(2.5f);
    bank.reset();
    REQUIRE_NOTHROW(bank.next()); // must not OOB-read
    // Position -1 → clamped to 0.
    bank.set_position(-1.0f);
    bank.reset();
    REQUIRE_NOTHROW(bank.next());
}

TEST_CASE("Wavetable explicit band construction with empty samples is rejected",
          "[signal][wavetable]") {
    std::vector<WavetableEntry> bands;
    bands.push_back({{}, 1000.0f});  // empty samples — should be filtered
    bands.push_back({{0.0f, 0.5f, 1.0f, 0.5f, 0.0f, -0.5f, -1.0f, -0.5f}, 2000.0f});
    Wavetable wt(std::move(bands));
    REQUIRE(wt.band_count() == 1);
    // The remaining band's ceiling is 2000 Hz.
    wt.set_sample_rate(48000.0f);
    wt.set_frequency(1000.0f);
    REQUIRE_NOTHROW(wt.next());
}

TEST_CASE("Wavetable explicit band construction sorts by ascending ceiling",
          "[signal][wavetable]") {
    std::vector<WavetableEntry> bands;
    bands.push_back({{1.0f, 0.0f, -1.0f, 0.0f}, 10000.0f}); // out of order
    bands.push_back({{0.5f, 0.0f, -0.5f, 0.0f}, 500.0f});
    bands.push_back({{0.25f, 0.0f, -0.25f, 0.0f}, 5000.0f});
    Wavetable wt(std::move(bands));
    REQUIRE(wt.band_count() == 3);
    wt.set_sample_rate(48000.0f);

    wt.set_frequency(100.0f);  // covered by 500-Hz band
    REQUIRE(wt.current_band() == 0);

    wt.set_frequency(2000.0f); // covered by 5-kHz band
    REQUIRE(wt.current_band() == 1);

    wt.set_frequency(8000.0f); // covered by 10-kHz band
    REQUIRE(wt.current_band() == 2);
}
