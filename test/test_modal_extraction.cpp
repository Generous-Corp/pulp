#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/analysis/modal_extraction.hpp>
#include <pulp/signal/modal_bank.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {
struct ModeSeed {
    double frequency;
    double t60;
    double gain;
};

std::vector<float> impulse(double fs, double f, double t60, double gain, int n) {
    std::vector<float> out(static_cast<std::size_t>(n));
    const double r = std::pow(10.0, -3.0 / (t60 * fs));
    const double w = 2.0 * 3.14159265358979323846 * f / fs;
    double env = 1.0;
    for (int i = 0; i < n; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<float>(gain * env *
            std::sin((static_cast<double>(i) + 1.0) * w));
        env *= r;
    }
    return out;
}

std::vector<float> mixture(double fs, std::span<const ModeSeed> modes, int n) {
    std::vector<float> out(static_cast<std::size_t>(n), 0.0f);
    for (const auto& mode : modes) {
        const auto component = impulse(fs, mode.frequency, mode.t60, mode.gain, n);
        for (std::size_t i = 0; i < out.size(); ++i) out[i] += component[i];
    }
    return out;
}

double cents_between(double actual, double expected) {
    return 1200.0 * std::log2(actual / expected);
}

void check_atomic_error(const pulp::test::audio::ModalExtractionResult& result,
                        pulp::test::audio::ModalExtractionError expected) {
    CHECK(result.error == expected);
    CHECK_FALSE(result.ok);
    CHECK(result.spec.modes.empty());
    CHECK(result.candidates.empty());
}
}

TEST_CASE("modal extraction compiles an independently prescribed single mode",
          "[audio-analysis][modal-extraction]") {
    const auto input = impulse(48000.0, 440.0, 0.8, 0.9, 96000);
    pulp::test::audio::ModalExtractionOptions options;
    options.analysis.fft_length = 32768;
    options.analysis.refine_window = 8192;
    const auto result = pulp::test::audio::compile_modal_spec(input, 48000.0, options);
    REQUIRE(result.ok);
    REQUIRE(result.spec.modes.size() == 1);
    CHECK(std::abs(cents_between(result.spec.modes[0].freq_hz, 440.0)) < 10.0);
    CHECK(std::abs((result.spec.modes[0].t60_s - 0.8f) / 0.8f) < 0.10);

    pulp::signal::ModalBank bank;
    bank.prepare(48000.0, 1);
    bank.set_modes(result.spec.modes);
    std::vector<float> input_ir(input.size(), 0.0f), rendered(input.size(), 0.0f);
    input_ir[0] = 1.0f;
    bank.process_add(input_ir.data(), rendered.data(), static_cast<int>(rendered.size()));
    REQUIRE(std::isfinite(rendered.back()));
    const auto measured = pulp::test::audio::measure_mode(
        rendered, 48000.0, result.spec.modes[0].freq_hz, options.analysis);
    CHECK(std::abs(cents_between(measured.freq_hz, 440.0)) < 20.0);
    CHECK(std::abs((measured.t60_s - 0.8) / 0.8) < 0.20);
}

TEST_CASE("modal extraction rejects malformed input atomically", "[audio-analysis][modal-extraction]") {
    pulp::test::audio::ModalExtractionOptions options;
    check_atomic_error(pulp::test::audio::compile_modal_spec({}, 48000.0, options),
                       pulp::test::audio::ModalExtractionError::empty_input);
    const std::vector<float> short_input(4, 0.0f);
    auto result = pulp::test::audio::compile_modal_spec(short_input, 48000.0, options);
    check_atomic_error(result, pulp::test::audio::ModalExtractionError::input_too_short);
    const std::vector<float> silence(4096, 0.0f);
    check_atomic_error(pulp::test::audio::compile_modal_spec(silence, 48000.0, options),
                       pulp::test::audio::ModalExtractionError::silent_input);
    auto bad = impulse(48000.0, 440.0, 0.8, 1.0, 4096);
    bad[17] = std::numeric_limits<float>::quiet_NaN();
    result = pulp::test::audio::compile_modal_spec(bad, 48000.0, options);
    check_atomic_error(result, pulp::test::audio::ModalExtractionError::non_finite_input);
    const auto valid = impulse(48000.0, 440.0, 0.8, 1.0, 4096);
    check_atomic_error(pulp::test::audio::compile_modal_spec(valid, 0.0, options),
                       pulp::test::audio::ModalExtractionError::invalid_sample_rate);
    check_atomic_error(pulp::test::audio::compile_modal_spec(
                           valid, std::numeric_limits<double>::infinity(), options),
                       pulp::test::audio::ModalExtractionError::invalid_sample_rate);

    const auto require_bad_options = [&](auto mutate) {
        auto invalid = options;
        mutate(invalid);
        check_atomic_error(pulp::test::audio::compile_modal_spec(valid, 48000.0, invalid),
                           pulp::test::audio::ModalExtractionError::invalid_options);
    };
    require_bad_options([](auto& o) { o.max_modes = 0; });
    require_bad_options([](auto& o) { o.max_input_samples = 127; });
    require_bad_options([](auto& o) { o.max_input_samples = (1 << 22) + 1; });
    require_bad_options([](auto& o) { o.confidence_floor = 1.1; });
    require_bad_options([](auto& o) { o.confidence_floor =
        std::numeric_limits<double>::quiet_NaN(); });
    require_bad_options([](auto& o) { o.min_separation_cents = -1.0; });
    require_bad_options([](auto& o) { o.min_separation_cents =
        std::numeric_limits<double>::infinity(); });
    require_bad_options([](auto& o) { o.snr_floor_db =
        std::numeric_limits<double>::quiet_NaN(); });
    require_bad_options([](auto& o) { o.snr_floor_db = 1.0; });
    require_bad_options([](auto& o) { o.recipe.clear(); });
    require_bad_options([](auto& o) { o.recipe.assign(257, 'x'); });
    require_bad_options([](auto& o) { o.analysis.fft_length = 1000; });
    require_bad_options([](auto& o) { o.analysis.fft_length = 1 << 21; });
    require_bad_options([](auto& o) { o.analysis.search_low_hz = -1.0; });
    require_bad_options([](auto& o) { o.analysis.search_high_hz = 24001.0; });
    require_bad_options([](auto& o) { o.analysis.peak_floor_db = 1.0; });
    require_bad_options([](auto& o) { o.analysis.min_separation_hz = -1.0; });
    require_bad_options([](auto& o) { o.analysis.refine_span_cents =
        std::numeric_limits<double>::infinity(); });
    require_bad_options([](auto& o) { o.analysis.refine_step_cents = 0.0; });
    require_bad_options([](auto& o) { o.analysis.refine_window = 32; });
    require_bad_options([](auto& o) { o.analysis.fit_start_db = 1.0; });
    require_bad_options([](auto& o) { o.analysis.fit_end_db = -2.0; });
    require_bad_options([](auto& o) { o.analysis.fit_offset_s = -1.0; });
    require_bad_options([](auto& o) { o.analysis.envelope_window = 8; });
    require_bad_options([](auto& o) { o.analysis.envelope_hop = 0; });
    require_bad_options([](auto& o) { o.analysis.min_fit_points = 1; });
    require_bad_options([](auto& o) { o.analysis.channel = -1; });
}

TEST_CASE("modal extraction preserves recipe identity and deterministic mode ordering",
          "[audio-analysis][modal-extraction]") {
    auto first = impulse(48000.0, 440.0, 0.8, 0.9, 96000);
    const auto second = impulse(48000.0, 880.0, 0.6, 0.5, 96000);
    for (std::size_t i = 0; i < first.size(); ++i) first[i] += 0.5f * second[i];
    pulp::test::audio::ModalExtractionOptions options;
    options.recipe = "m5-modal-test-v1";
    options.max_modes = 4;
    const auto a = pulp::test::audio::compile_modal_spec(first, 48000.0, options);
    const auto b = pulp::test::audio::compile_modal_spec(first, 48000.0, options);
    REQUIRE(a.ok);
    REQUIRE(a.recipe == options.recipe);
    CHECK(a.message == b.message);
    CHECK(a.recipe == b.recipe);
    CHECK(a.spec.name == b.spec.name);
    CHECK(a.spec.description == b.spec.description);
    REQUIRE(a.spec.modes.size() == b.spec.modes.size());
    REQUIRE(a.candidates.size() == b.candidates.size());
    for (std::size_t i = 0; i < a.spec.modes.size(); ++i) {
        CHECK(a.spec.modes[i].freq_hz == b.spec.modes[i].freq_hz);
        CHECK(a.spec.modes[i].t60_s == b.spec.modes[i].t60_s);
        CHECK(a.spec.modes[i].gain == b.spec.modes[i].gain);
        CHECK(a.spec.modes[i].freq_hz >= (i == 0 ? 0.0f : a.spec.modes[i - 1].freq_hz));
    }
    for (std::size_t i = 0; i < a.candidates.size(); ++i) {
        CHECK(a.candidates[i].mode.freq_hz == b.candidates[i].mode.freq_hz);
        CHECK(a.candidates[i].mode.t60_s == b.candidates[i].mode.t60_s);
        CHECK(a.candidates[i].mode.gain == b.candidates[i].mode.gain);
        CHECK(a.candidates[i].confidence == b.candidates[i].confidence);
        CHECK(a.candidates[i].prominence_db == b.candidates[i].prominence_db);
        CHECK(a.candidates[i].ranking_score == b.candidates[i].ranking_score);
        CHECK(a.candidates[i].merged == b.candidates[i].merged);
        CHECK(a.candidates[i].kept == b.candidates[i].kept);
    }
}

TEST_CASE("modal extraction merges a near pair and rejects a seeded buried mode",
          "[audio-analysis][modal-extraction]") {
    constexpr ModeSeed modes[] = {
        {440.0, 0.9, 0.9}, {448.0, 0.9, 0.55},
        {880.0, 0.65, 0.45}, {1400.0, 0.7, 0.00005},
    };
    const auto input = mixture(48000.0, modes, 96000);
    pulp::test::audio::ModalExtractionOptions options;
    options.analysis.fft_length = 65536;
    options.analysis.min_separation_hz = 0.0;
    options.analysis.peak_floor_db = -90.0;
    options.min_separation_cents = 40.0;
    options.snr_floor_db = -70.0;
    const auto result = pulp::test::audio::compile_modal_spec(input, 48000.0, options);
    REQUIRE(result.ok);
    REQUIRE(result.spec.modes.size() == 2);
    CHECK(std::abs(cents_between(result.spec.modes[0].freq_hz, 440.0)) < 10.0);
    CHECK(std::abs(cents_between(result.spec.modes[1].freq_hz, 880.0)) < 10.0);
    CHECK(std::none_of(result.spec.modes.begin(), result.spec.modes.end(), [](const auto& mode) {
        return std::abs(cents_between(mode.freq_hz, 1400.0)) < 20.0;
    }));
    CHECK(std::count_if(result.candidates.begin(), result.candidates.end(),
                        [](const auto& candidate) { return candidate.merged; }) == 1);
}

TEST_CASE("modal extraction capacity keeps the independent highest-ranked modes",
          "[audio-analysis][modal-extraction]") {
    constexpr ModeSeed modes[] = {
        {220.0, 0.9, 0.95}, {337.0, 0.9, 0.82}, {499.0, 0.9, 0.70},
        {731.0, 0.9, 0.58}, {1013.0, 0.9, 0.46}, {1409.0, 0.9, 0.34},
        {1901.0, 0.9, 0.22},
    };
    const auto input = mixture(48000.0, modes, 96000);
    pulp::test::audio::ModalExtractionOptions options;
    options.max_modes = 3;
    options.analysis.fft_length = 65536;
    options.analysis.min_separation_hz = 0.0;
    options.analysis.peak_floor_db = -80.0;
    options.snr_floor_db = -80.0;
    options.confidence_floor = 0.5;
    const auto result = pulp::test::audio::compile_modal_spec(input, 48000.0, options);
    REQUIRE(result.ok);
    REQUIRE(result.candidates.size() == options.max_modes + 4u);
    REQUIRE(result.spec.modes.size() == static_cast<std::size_t>(options.max_modes));
    for (int i = 0; i < options.max_modes; ++i)
        CHECK(std::abs(cents_between(result.spec.modes[static_cast<std::size_t>(i)].freq_hz,
                                     modes[static_cast<std::size_t>(i)].frequency)) < 10.0);
}
