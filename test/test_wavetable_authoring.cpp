#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/wavetable_authoring.hpp>
#include <pulp/signal/wavetable.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr int kAliasRenderLength = 16384;
constexpr int kAliasAnalysisOffset = 256;

pulp::audio::Buffer<float> periodic_source(std::size_t period, std::size_t periods,
                                           std::size_t harmonics = 1) {
    pulp::audio::Buffer<float> source(1, period * periods);
    for (std::size_t frame = 0; frame < source.num_samples(); ++frame) {
        const double phase = static_cast<double>(frame % period) / static_cast<double>(period);
        double sample = 0.0;
        for (std::size_t harmonic = 1; harmonic <= harmonics; ++harmonic)
            sample += std::sin(kTwoPi * harmonic * phase + 0.17 * harmonic) /
                      static_cast<double>(harmonic);
        source.channel(0)[frame] = static_cast<float>(sample);
    }
    return source;
}

pulp::audio::WavetableAuthoringRecipe recipe_for(std::size_t period) {
    pulp::audio::WavetableAuthoringRecipe recipe;
    recipe.automatic_cycle.minimum_cycle_samples = static_cast<std::uint32_t>(period - period / 4);
    recipe.automatic_cycle.maximum_cycle_samples = static_cast<std::uint32_t>(period + period / 4);
    recipe.automatic_cycle.minimum_correlation = 0.99;
    recipe.table_length = 256;
    recipe.num_bands = 6;
    recipe.reference_sample_rate = kSampleRate;
    recipe.guard_harmonics = 1;
    recipe.maximum_seam_error = 1.0e-4;
    return recipe;
}

double harmonic_magnitude(const std::vector<float>& samples, std::size_t harmonic) {
    std::complex<double> coefficient{0.0, 0.0};
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double phase =
            -kTwoPi * static_cast<double>(harmonic * i) / static_cast<double>(samples.size());
        coefficient += static_cast<double>(samples[i]) *
                       std::complex<double>{std::cos(phase), std::sin(phase)};
    }
    return 2.0 * std::abs(coefficient) / static_cast<double>(samples.size());
}

std::size_t retained_harmonics(const pulp::signal::WavetableEntry& band,
                               const pulp::audio::WavetableAuthoringRecipe& recipe) {
    const double nyquist = recipe.reference_sample_rate * 0.5;
    const auto safe =
        static_cast<std::size_t>(std::floor(nyquist / static_cast<double>(band.max_frequency_hz)));
    const std::size_t guarded = safe > recipe.guard_harmonics ? safe - recipe.guard_harmonics : 1;
    return std::max<std::size_t>(1, std::min<std::size_t>(recipe.table_length / 2 - 1, guarded));
}

pulp::test::audio::AliasReport analyze_aliases(const std::vector<float>& signal,
                                               double fundamental) {
    pulp::audio::Buffer<float> buffer(1, signal.size());
    std::copy(signal.begin(), signal.end(), buffer.channel(0).begin());
    pulp::test::audio::AliasOptions options;
    options.num_harmonics = static_cast<int>(std::ceil(3.0 * kSampleRate / fundamental));
    options.analysis_offset = kAliasAnalysisOffset;
    options.analysis_length = static_cast<int>(signal.size()) - kAliasAnalysisOffset;
    options.max_alias_frequency_hz = 20000.0;
    return pulp::test::audio::measure_aliasing(std::as_const(buffer).view(), fundamental,
                                               kSampleRate, options);
}

double alias_bound_db(const pulp::test::audio::AliasReport& report) {
    return report.worst_alias_db > report.detection_floor_db ? report.worst_alias_db
                                                             : report.detection_floor_db;
}

std::vector<float> render_table(pulp::signal::Wavetable& table, double fundamental) {
    table.reset();
    table.set_frequency_immediate(static_cast<float>(fundamental));
    std::vector<float> signal(kAliasRenderLength + kAliasAnalysisOffset);
    for (float& sample : signal)
        sample = table.next();
    return signal;
}

std::vector<float> bandlimited_control(double fundamental, double injected_alias_hz = 0.0,
                                       double injected_alias_amplitude = 0.0) {
    const int maximum_harmonic = static_cast<int>(std::floor((kSampleRate * 0.5) / fundamental));
    std::vector<float> signal(kAliasRenderLength + kAliasAnalysisOffset);
    for (std::size_t frame = 0; frame < signal.size(); ++frame) {
        const double time = static_cast<double>(frame) / kSampleRate;
        double sample = 0.0;
        for (int harmonic = 1; harmonic <= maximum_harmonic; ++harmonic)
            sample += std::sin(kTwoPi * harmonic * fundamental * time + 0.3) / harmonic;
        if (injected_alias_amplitude > 0.0)
            sample += injected_alias_amplitude * std::sin(kTwoPi * injected_alias_hz * time + 0.9);
        signal[frame] = static_cast<float>(sample);
    }
    return signal;
}

int first_in_band_alias_harmonic(double fundamental) {
    const int maximum = static_cast<int>(std::ceil(3.0 * kSampleRate / fundamental));
    for (int harmonic = static_cast<int>(std::floor((kSampleRate * 0.5) / fundamental)) + 1;
         harmonic <= maximum; ++harmonic) {
        const double folded =
            pulp::test::audio::fold_frequency(harmonic * fundamental, kSampleRate);
        if (folded <= 20000.0)
            return harmonic;
    }
    return 0;
}

} // namespace

TEST_CASE("wavetable authoring reuses the released cycle estimator",
          "[audio][wavetable-authoring][cycle]") {
    constexpr std::size_t period = 64;
    auto source = periodic_source(period, 8, 9);
    auto recipe = recipe_for(period);

    const auto direct = pulp::audio::estimate_sample_heritage_auto_cycle(
        std::as_const(source).view(), recipe.automatic_cycle);
    REQUIRE(direct.valid());

    const auto result = pulp::audio::compile_wavetable(
        std::as_const(source).view(), kSampleRate, recipe,
        {{"fixture", "synthesized", "test-evidence"}, "CC0-1.0", "unit test"});

    REQUIRE(result.valid());
    CHECK(result.chosen_cycle.length_frames == direct.cycle_samples);
    CHECK(result.chosen_cycle.length_frames == period);
    CHECK_THAT(result.cycle_correlation, WithinAbs(direct.correlation, 1.0e-12));
    CHECK(result.bands.size() == recipe.num_bands);
    CHECK(result.source_audio_sha256.size() == 64);
    CHECK(result.materialized_table_sha256.size() == 64);
    for (const auto& band : result.bands) {
        CHECK(band.samples.size() == recipe.table_length);
        CHECK(std::all_of(band.samples.begin(), band.samples.end(),
                          [](float value) { return std::isfinite(value); }));
    }
}

TEST_CASE("automatic wavetable seam selection is discriminating and fail closed",
          "[audio][wavetable-authoring][cycle][seam]") {
    constexpr std::size_t period = 64;
    auto recipe = recipe_for(period);
    recipe.automatic_cycle.minimum_correlation = 0.9;

    SECTION("the earliest uniquely clean adjacent-cycle boundary is selected") {
        auto source = periodic_source(period, 5, 5);
        for (std::size_t frame = 0; frame < period; ++frame)
            source.channel(0)[frame] += 0.2f;
        const auto result =
            pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate, recipe);
        REQUIRE(result.valid());
        CHECK(result.chosen_cycle.length_frames == period);
        CHECK(result.chosen_cycle.start_frame == period);
    }

    SECTION("uniformly bad adjacent-cycle seams report NoReliableCycle") {
        auto source = periodic_source(period, 5, 5);
        for (std::size_t frame = 0; frame < source.num_samples(); ++frame)
            source.channel(0)[frame] += 0.1f * static_cast<float>(frame / period);
        const auto result =
            pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate, recipe);
        CHECK(result.status == pulp::audio::WavetableCompileStatus::NoReliableCycle);
        CHECK(result.bands.empty());
    }
}

TEST_CASE("wavetable authoring rejects wrong seams and malformed recipes",
          "[audio][wavetable-authoring][validation]") {
    constexpr std::size_t period = 64;
    auto source = periodic_source(period, 8, 7);
    auto recipe = recipe_for(period);
    recipe.explicit_cycle = pulp::audio::WavetableCycleSelection{0, period};
    REQUIRE(
        pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate, recipe).valid());

    SECTION("a one-sample cycle mutation fails the independent source seam gate") {
        recipe.explicit_cycle = pulp::audio::WavetableCycleSelection{0, period + 1};
        const auto result =
            pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate, recipe);
        CHECK(result.status == pulp::audio::WavetableCompileStatus::InvalidExplicitCycle);
        CHECK(result.bands.empty());
    }

    SECTION("the radix-two FFT requirement fails closed") {
        recipe.table_length = 300;
        const auto result =
            pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate, recipe);
        CHECK(result.status == pulp::audio::WavetableCompileStatus::InvalidRecipe);
        CHECK(result.bands.empty());
    }

    SECTION("an explicit cycle does not depend on unused automatic-search options") {
        recipe.automatic_cycle.minimum_cycle_samples = 0;
        recipe.automatic_cycle.maximum_cycle_samples = 0;
        recipe.automatic_cycle.minimum_correlation = std::numeric_limits<double>::quiet_NaN();
        CHECK(pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate, recipe)
                  .valid());
    }

    SECTION("physically invalid source rates fail closed") {
        const auto result =
            pulp::audio::compile_wavetable(std::as_const(source).view(), 1000.0, recipe);
        CHECK(result.status == pulp::audio::WavetableCompileStatus::InvalidSource);
        CHECK(result.bands.empty());
    }

    SECTION("non-finite source samples fail closed") {
        source.channel(0)[3] = std::numeric_limits<float>::quiet_NaN();
        const auto result =
            pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate, recipe);
        CHECK(result.status == pulp::audio::WavetableCompileStatus::InvalidSource);
        CHECK(result.bands.empty());
    }
}

TEST_CASE("wavetable authoring mip policy rejects the first unsafe harmonic",
          "[audio][wavetable-authoring][mip][oracle]") {
    constexpr std::size_t period = 128;
    auto source = periodic_source(period, 6, 48);
    auto recipe = recipe_for(period);
    recipe.explicit_cycle = pulp::audio::WavetableCycleSelection{0, period};
    recipe.maximum_seam_error = 1.0e-3;
    const auto result =
        pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate, recipe);
    REQUIRE(result.valid());

    const auto factory = pulp::signal::Wavetable::make_saw(
        recipe.num_bands, recipe.table_length, static_cast<float>(recipe.reference_sample_rate));
    REQUIRE(factory.band_count() == result.bands.size());

    double reference_fundamental = -1.0;
    bool observed_retained_upper_partial = false;
    for (std::size_t index = 0; index < result.bands.size(); ++index) {
        const auto& band = result.bands[index];
        CHECK_THAT(band.max_frequency_hz, WithinAbs(factory.band_max_frequency_hz(index), 0.0));
        const auto retained = retained_harmonics(band, recipe);
        const double fundamental = harmonic_magnitude(band.samples, 1);
        const double rejected = harmonic_magnitude(band.samples, retained + 1);
        INFO("ceiling=" << band.max_frequency_hz << " retained=" << retained
                        << " fundamental=" << fundamental << " rejected=" << rejected);
        CHECK(rejected < std::max(1.0e-5, fundamental * 2.0e-5));
        if (reference_fundamental < 0.0)
            reference_fundamental = fundamental;
        else
            CHECK_THAT(fundamental, WithinAbs(reference_fundamental, 2.0e-5));

        if (retained >= 2) {
            const auto upper = std::min<std::size_t>(retained, 48);
            CHECK(harmonic_magnitude(band.samples, upper) > 1.0e-4);
            observed_retained_upper_partial = true;
        }

        // Defect-specific positive control: restoring H+1 must be visible to
        // this direct projection, independent of the production FFT.
        auto mutant = band.samples;
        for (std::size_t i = 0; i < mutant.size(); ++i)
            mutant[i] += 0.02f * static_cast<float>(
                                     std::cos(kTwoPi * static_cast<double>((retained + 1) * i) /
                                              static_cast<double>(mutant.size())));
        CHECK(harmonic_magnitude(mutant, retained + 1) > 0.015);
    }
    CHECK(observed_retained_upper_partial);
}

TEST_CASE("wavetable authoring hashes are deterministic and content addressed",
          "[audio][wavetable-authoring][hash]") {
    constexpr std::size_t period = 64;
    auto source = periodic_source(period, 6, 12);
    auto recipe = recipe_for(period);
    recipe.explicit_cycle = pulp::audio::WavetableCycleSelection{0, period};
    const pulp::audio::WavetableAuthoringProvenance provenance{
        {"fixture", "synthesized", "evidence"}, "CC0-1.0", "determinism"};

    const auto first = pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate,
                                                      recipe, provenance);
    const auto second = pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate,
                                                       recipe, provenance);
    REQUIRE(first.valid());
    REQUIRE(second.valid());
    CHECK(first.source_audio_sha256 == second.source_audio_sha256);
    CHECK(first.materialized_table_sha256 == second.materialized_table_sha256);
    REQUIRE(first.bands.size() == second.bands.size());
    for (std::size_t band = 0; band < first.bands.size(); ++band)
        CHECK(first.bands[band].samples == second.bands[band].samples);

    source.channel(0)[period / 3] += 0.01f;
    recipe.maximum_seam_error = 1.0;
    const auto changed = pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate,
                                                        recipe, provenance);
    REQUIRE(changed.valid());
    CHECK(changed.source_audio_sha256 != first.source_audio_sha256);
    CHECK(changed.materialized_table_sha256 != first.materialized_table_sha256);
}

TEST_CASE("wavetable authoring rejects folded energy before cycle downsampling",
          "[audio][wavetable-authoring][resample][oracle]") {
    constexpr std::size_t period = 512;
    constexpr std::size_t table_length = 256;
    constexpr std::size_t rejected_source_harmonic = 200;
    constexpr std::size_t folded_harmonic = table_length - rejected_source_harmonic;
    pulp::audio::Buffer<float> source(1, period * 3);
    for (std::size_t frame = 0; frame < source.num_samples(); ++frame) {
        const double phase = static_cast<double>(frame % period) / static_cast<double>(period);
        source.channel(0)[frame] =
            static_cast<float>(0.4 * std::sin(kTwoPi * 3.0 * phase + 0.2) +
                               0.8 * std::sin(kTwoPi * rejected_source_harmonic * phase + 0.4));
    }

    auto recipe = recipe_for(period);
    recipe.table_length = table_length;
    recipe.explicit_cycle = pulp::audio::WavetableCycleSelection{0, period};
    const auto result =
        pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate, recipe);
    REQUIRE(result.valid());
    REQUIRE_FALSE(result.bands.empty());
    const auto& widest = result.bands.front().samples;
    const double wanted = harmonic_magnitude(widest, 3);
    const double folded = harmonic_magnitude(widest, folded_harmonic);
    INFO("wanted=" << wanted << " folded=" << folded);
    CHECK(folded < wanted * 1.0e-3);

    // Positive control: naive periodic decimation folds source H=200 exactly
    // onto destination H=56, proving the projection can see the defect.
    std::vector<float> mutant(table_length);
    for (std::size_t i = 0; i < mutant.size(); ++i)
        mutant[i] = source.channel(0)[i * 2];
    CHECK(harmonic_magnitude(mutant, folded_harmonic) > 0.5);
}

TEST_CASE("wavetable downsampling drops the first unsafe bin at its work boundary",
          "[audio][wavetable-authoring][resample][boundary]") {
    constexpr std::size_t table_length = 256;
    constexpr std::size_t retained = table_length / 2 - 1;
    constexpr std::size_t work_cap = 1u << 25;
    constexpr std::size_t synthesis_work = table_length * retained;
    constexpr std::size_t largest_cycle = (work_cap - synthesis_work) / (retained + 1);
    constexpr std::size_t period = largest_cycle - largest_cycle % table_length;
    constexpr std::size_t first_unsafe = table_length / 2 + 1;
    constexpr std::size_t folded = table_length - first_unsafe;
    static_assert(period > table_length && period % table_length == 0);

    pulp::audio::Buffer<float> source(1, period + 2);
    for (std::size_t frame = 0; frame < source.num_samples(); ++frame) {
        const double phase = static_cast<double>(frame % period) / period;
        source.channel(0)[frame] =
            static_cast<float>(0.4 * std::sin(kTwoPi * 3.0 * phase + 0.2) +
                               0.8 * std::sin(kTwoPi * first_unsafe * phase + 0.4));
    }

    auto recipe = recipe_for(64);
    recipe.table_length = table_length;
    recipe.explicit_cycle = pulp::audio::WavetableCycleSelection{0, period};
    const auto result =
        pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate, recipe);
    REQUIRE(result.valid());
    const double wanted = harmonic_magnitude(result.bands.front().samples, 3);
    const double rejected = harmonic_magnitude(result.bands.front().samples, folded);
    INFO("period=" << period << " ratio=" << period / table_length << " wanted=" << wanted
                   << " folded=" << rejected);
    CHECK(rejected < wanted * 1.0e-5);

    std::vector<float> mutant(table_length);
    constexpr std::size_t stride = period / table_length;
    for (std::size_t frame = 0; frame < table_length; ++frame)
        mutant[frame] = source.channel(0)[frame * stride];
    CHECK(harmonic_magnitude(mutant, folded) > 0.5);
}

TEST_CASE("authored bands remain alias-clean through Wavetable playback",
          "[audio][wavetable-authoring][playback][alias]") {
    constexpr std::size_t period = 512;
    auto source = periodic_source(period, 4, period / 2 - 1);
    auto recipe = recipe_for(period);
    recipe.table_length = 2048;
    recipe.explicit_cycle = pulp::audio::WavetableCycleSelection{0, period};
    recipe.maximum_seam_error = 1.0e-3;
    auto result = pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate, recipe);
    REQUIRE(result.valid());
    REQUIRE(result.bands.size() > 2);

    pulp::signal::Wavetable table(std::move(result.bands));
    table.set_sample_rate(static_cast<float>(kSampleRate));
    int measured = 0;
    double worst = -1000.0;
    double worst_fundamental = 0.0;
    for (std::size_t band = 1; band + 1 < table.band_count(); ++band) {
        const double lower = table.band_max_frequency_hz(band - 1);
        const double ceiling = table.band_max_frequency_hz(band);
        if (ceiling < 500.0 || ceiling > 0.45 * kSampleRate)
            continue;
        for (const double fraction : {0.99, 0.999}) {
            const double fundamental = std::max(lower * 1.02, ceiling * fraction);
            const auto report = analyze_aliases(render_table(table, fundamental), fundamental);
            INFO("band=" << band << " f0=" << fundamental << " alias=" << report.worst_alias_db
                         << " floor=" << report.detection_floor_db);
            REQUIRE_FALSE(report.has_unresolved_in_band_alias);
            const double bound = alias_bound_db(report);
            if (bound > worst) {
                worst = bound;
                worst_fundamental = fundamental;
            }
            ++measured;
        }
    }
    REQUIRE(measured > 0);
    REQUIRE(worst_fundamental > 0.0);

    const auto clean = analyze_aliases(bandlimited_control(worst_fundamental), worst_fundamental);
    REQUIRE_FALSE(clean.has_unresolved_in_band_alias);
    CHECK(clean.worst_alias_db < -130.0);
    const double proven_floor_bound = alias_bound_db(clean);

    const int alias_harmonic = first_in_band_alias_harmonic(worst_fundamental);
    REQUIRE(alias_harmonic > 0);
    const double alias_hz =
        pulp::test::audio::fold_frequency(alias_harmonic * worst_fundamental, kSampleRate);
    constexpr double injected_db = -60.0;
    const auto injected = analyze_aliases(
        bandlimited_control(worst_fundamental, alias_hz, std::pow(10.0, injected_db / 20.0)),
        worst_fundamental);
    REQUIRE_FALSE(injected.has_unresolved_in_band_alias);
    CHECK(injected.worst_alias_index == alias_harmonic);
    CHECK_THAT(injected.worst_alias_db, WithinAbs(injected_db, 1.0));

    INFO("worst authored alias bound=" << worst << " dBc at f0=" << worst_fundamental
                                       << "; proven local floor bound=" << proven_floor_bound
                                       << " dBc");
    CHECK(proven_floor_bound < -120.0);
    CHECK(worst < -90.0);
}

TEST_CASE("authored bands preserve Wavetable realtime allocation safety",
          "[audio][wavetable-authoring][rt]") {
    constexpr std::size_t period = 64;
    auto source = periodic_source(period, 6, 8);
    auto recipe = recipe_for(period);
    recipe.explicit_cycle = pulp::audio::WavetableCycleSelection{0, period};
    auto result = pulp::audio::compile_wavetable(std::as_const(source).view(), kSampleRate, recipe);
    REQUIRE(result.valid());

    pulp::signal::Wavetable table(std::move(result.bands));
    table.set_sample_rate(static_cast<float>(kSampleRate));
    table.set_frequency_immediate(440.0f);
    table.reset();
    pulp::test::RtAllocationProbe probe;
    float checksum = 0.0f;
    for (int i = 0; i < 4096; ++i)
        checksum += table.next();
    CHECK(std::isfinite(checksum));
    CHECK_FALSE(probe.saw_allocation());
}
