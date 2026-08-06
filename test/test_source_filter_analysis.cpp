#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/source_filter_analysis.hpp>
#include <pulp/signal/spectral_envelope_shifter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

using namespace pulp::signal;

namespace {

#if defined(__has_feature)
#if __has_feature(undefined_behavior_sanitizer)
inline constexpr bool kUndefinedBehaviorSanitizerBuild = true;
#else
inline constexpr bool kUndefinedBehaviorSanitizerBuild = false;
#endif

#else
inline constexpr bool kUndefinedBehaviorSanitizerBuild = false;
#endif

#if defined(NDEBUG)
inline constexpr bool kOptimizedBuild = true;
#else
inline constexpr bool kOptimizedBuild = false;
#endif

template <typename SampleType> class FrozenEnvelopeShifter {
  public:
    void prepare(const SpectralEnvelopeShifterConfig& config) {
        config_ = config;
        if (config_.order <= 0)
            config_.order = config_.fft_size / 16;
        bins_ = config_.fft_size / 2 + 1;
        fft_ = FftT<SampleType>(config_.fft_size);
        log_magnitude_.assign(static_cast<std::size_t>(bins_), SampleType{0});
        envelope_.assign(static_cast<std::size_t>(bins_), SampleType{0});
        smooth_input_.assign(static_cast<std::size_t>(bins_), SampleType{0});
        cepstrum_.assign(static_cast<std::size_t>(config_.fft_size),
                         std::complex<SampleType>{SampleType{0}, SampleType{0}});
        max_gain_ln_ = static_cast<SampleType>(config_.max_gain_db * 0.1151293f);
    }

    void process(std::complex<SampleType>* const* frames, int channels, SampleType warp) {
        SampleType max_power = SampleType{0};
        double energy_before = 0.0;
        for (int k = 0; k < bins_; ++k) {
            SampleType power = SampleType{0};
            for (int channel = 0; channel < channels; ++channel) {
                const auto real = frames[channel][k].real();
                const auto imaginary = frames[channel][k].imag();
                power += real * real + imaginary * imaginary;
            }
            energy_before += static_cast<double>(power);
            log_magnitude_[static_cast<std::size_t>(k)] = power / static_cast<SampleType>(channels);
            max_power = std::max(max_power, log_magnitude_[static_cast<std::size_t>(k)]);
        }
        const auto floor_power =
            std::max(max_power * static_cast<SampleType>(1e-4), static_cast<SampleType>(1e-24));
        for (int k = 0; k < bins_; ++k)
            log_magnitude_[static_cast<std::size_t>(k)] =
                static_cast<SampleType>(0.5) *
                std::log(std::max(log_magnitude_[static_cast<std::size_t>(k)], floor_power));

        smooth(log_magnitude_.data(), envelope_.data());
        for (int iteration = 0; iteration < config_.true_envelope_iterations; ++iteration) {
            for (int k = 0; k < bins_; ++k)
                smooth_input_[static_cast<std::size_t>(k)] =
                    std::max(log_magnitude_[static_cast<std::size_t>(k)],
                             envelope_[static_cast<std::size_t>(k)]);
            smooth(smooth_input_.data(), envelope_.data());
        }

        double energy_after = 0.0;
        for (int k = 0; k < bins_; ++k) {
            const auto position =
                std::min(static_cast<SampleType>(k) * warp, static_cast<SampleType>(bins_ - 1));
            const int low = static_cast<int>(position);
            const int high = std::min(low + 1, bins_ - 1);
            const auto fraction = position - static_cast<SampleType>(low);
            const auto warped =
                envelope_[static_cast<std::size_t>(low)] * (SampleType{1} - fraction) +
                envelope_[static_cast<std::size_t>(high)] * fraction;
            auto gain_ln = warped - envelope_[static_cast<std::size_t>(k)];
            gain_ln = std::clamp(gain_ln, -max_gain_ln_, max_gain_ln_);
            const auto gain = std::exp(gain_ln);
            for (int channel = 0; channel < channels; ++channel) {
                frames[channel][k] *= gain;
                energy_after += static_cast<double>(std::norm(frames[channel][k]));
            }
        }
        constexpr double energy_floor = 1e-9;
        if (energy_after > energy_floor && energy_before > energy_floor) {
            auto normalization = static_cast<SampleType>(std::sqrt(energy_before / energy_after));
            normalization = std::clamp(normalization, static_cast<SampleType>(1.0 / 256.0),
                                       static_cast<SampleType>(256.0));
            if (std::isfinite(static_cast<double>(normalization))) {
                for (int k = 0; k < bins_; ++k)
                    for (int channel = 0; channel < channels; ++channel)
                        frames[channel][k] *= normalization;
            }
        }
    }

  private:
    void smooth(const SampleType* input, SampleType* output) {
        const int size = config_.fft_size;
        for (int k = 0; k < bins_; ++k)
            cepstrum_[static_cast<std::size_t>(k)] = {input[k], SampleType{0}};
        for (int k = bins_; k < size; ++k)
            cepstrum_[static_cast<std::size_t>(k)] = {input[size - k], SampleType{0}};
        fft_.inverse(cepstrum_.data());
        for (int q = config_.order + 1; q < size - config_.order; ++q)
            cepstrum_[static_cast<std::size_t>(q)] = {SampleType{0}, SampleType{0}};
        fft_.forward(cepstrum_.data());
        for (int k = 0; k < bins_; ++k)
            output[k] = cepstrum_[static_cast<std::size_t>(k)].real();
    }

    SpectralEnvelopeShifterConfig config_{};
    FftT<SampleType> fft_{2048};
    int bins_ = 0;
    SampleType max_gain_ln_ = static_cast<SampleType>(6.9);
    std::vector<SampleType> log_magnitude_;
    std::vector<SampleType> envelope_;
    std::vector<SampleType> smooth_input_;
    std::vector<std::complex<SampleType>> cepstrum_;
};

template <typename SampleType>
std::vector<std::vector<std::complex<SampleType>>> make_spectral_fixture(int bins) {
    std::vector<std::vector<std::complex<SampleType>>> frames(
        2, std::vector<std::complex<SampleType>>(static_cast<std::size_t>(bins)));
    for (int channel = 0; channel < 2; ++channel) {
        for (int bin = 0; bin < bins; ++bin) {
            const auto real = static_cast<SampleType>(
                0.03 + 0.7 * std::sin(0.071 * bin + 0.31 * channel) + 0.002 * (bin % 19));
            const auto imaginary = static_cast<SampleType>(
                0.2 * std::cos(0.043 * bin + 0.17 * channel) - 0.001 * (bin % 7));
            frames[static_cast<std::size_t>(channel)][static_cast<std::size_t>(bin)] = {real,
                                                                                        imaginary};
        }
    }
    return frames;
}

template <typename SampleType> void require_legacy_parity(SampleType warp) {
    SpectralEnvelopeShifterConfig config;
    config.fft_size = 256;
    config.order = 23;
    config.true_envelope_iterations = 5;
    config.max_gain_db = 37.0f;

    auto expected = make_spectral_fixture<SampleType>(129);
    auto actual = expected;
    std::array<std::complex<SampleType>*, 2> expected_views{expected[0].data(), expected[1].data()};
    std::array<std::complex<SampleType>*, 2> actual_views{actual[0].data(), actual[1].data()};

    FrozenEnvelopeShifter<SampleType> frozen;
    frozen.prepare(config);
    frozen.process(expected_views.data(), 2, warp);
    SpectralEnvelopeShifterT<SampleType> extracted;
    REQUIRE(extracted.prepare(config) == SourceFilterAnalysisStatus::Ok);
    extracted.process_group(actual_views.data(), 2, 129, warp);

    for (std::size_t channel = 0; channel < actual.size(); ++channel)
        for (std::size_t bin = 0; bin < actual[channel].size(); ++bin) {
            // Accelerate's float FFT may select differently aligned SIMD
            // paths for the frozen and extracted owners, producing last-bit
            // variation. The scalar double Release path remains exact.
            if constexpr (std::is_same_v<SampleType, float> || kUndefinedBehaviorSanitizerBuild ||
                          !kOptimizedBuild) {
                REQUIRE(actual[channel][bin].real() ==
                        Catch::Approx(expected[channel][bin].real()).epsilon(2e-6));
                REQUIRE(actual[channel][bin].imag() ==
                        Catch::Approx(expected[channel][bin].imag()).epsilon(2e-6));
            } else {
                REQUIRE(actual[channel][bin] == expected[channel][bin]);
            }
        }
}

std::vector<float> deterministic_noise(std::size_t count) {
    std::vector<float> output(count);
    std::uint32_t state = 0x12345678u;
    for (auto& sample : output) {
        state = state * 1664525u + 1013904223u;
        sample = static_cast<float>(static_cast<double>(state) / 4294967296.0 * 2.0 - 1.0);
    }
    return output;
}

std::vector<float> stable_ar2(std::size_t count, float scale = 1.0f) {
    auto excitation = deterministic_noise(count);
    std::vector<float> output(count, 0.0f);
    for (std::size_t i = 2; i < count; ++i)
        output[i] = scale * 0.2f * excitation[i] + 0.75f * output[i - 1] - 0.5f * output[i - 2];
    return output;
}

} // namespace

TEST_CASE("Extracted spectral envelope kernel matches the legacy shifter",
          "[signal][source-filter][parity]") {
    require_legacy_parity<float>(0.73f);
    require_legacy_parity<float>(1.37f);
    require_legacy_parity<double>(0.73);
    require_legacy_parity<double>(1.37);
}

TEST_CASE("Cepstral envelope preserves retained low-quefrency content",
          "[signal][source-filter][cepstral]") {
    CepstralEnvelopeAnalyzer64 analyzer;
    CepstralEnvelopeConfig64 config;
    config.fft_size = 256;
    config.order = 8;
    config.true_envelope_iterations = 0;
    REQUIRE(analyzer.prepare(config) == SourceFilterAnalysisStatus::Ok);

    std::vector<double> input(129);
    std::vector<double> output(129, -99.0);
    constexpr double pi = 3.14159265358979323846;
    for (std::size_t bin = 0; bin < input.size(); ++bin)
        input[bin] = 0.4 + 0.25 * std::cos(2.0 * pi * 5.0 * static_cast<double>(bin) / 256.0);
    const auto result = analyzer.estimate(input, output);
    REQUIRE(result.ok());
    REQUIRE(result.iterations_performed == 0);
    for (std::size_t bin = 0; bin < input.size(); ++bin)
        REQUIRE(output[bin] == Catch::Approx(input[bin]).margin(1e-12));

    for (std::size_t bin = 0; bin < input.size(); ++bin)
        input[bin] = 0.4 + 0.25 * std::cos(2.0 * pi * 27.0 * static_cast<double>(bin) / 256.0);
    REQUIRE(analyzer.estimate(input, output).ok());
    for (const auto value : output)
        REQUIRE(value == Catch::Approx(0.4).margin(1e-12));
}

TEST_CASE("True-envelope refinement is bounded convergent and transactional",
          "[signal][source-filter][cepstral]") {
    CepstralEnvelopeAnalyzer analyzer;
    CepstralEnvelopeConfig config;
    config.fft_size = 256;
    config.order = 6;
    config.true_envelope_iterations = 12;
    REQUIRE(analyzer.prepare(config) == SourceFilterAnalysisStatus::Ok);

    std::vector<float> sparse(129, -8.0f);
    for (std::size_t bin = 4; bin < sparse.size(); bin += 8)
        sparse[bin] = 0.5f + 0.25f * std::sin(static_cast<float>(bin) * 0.08f);
    std::vector<float> refined(129);
    const auto result = analyzer.estimate(sparse, refined);
    REQUIRE(result.ok());
    REQUIRE(result.iterations_performed == 12);

    CepstralEnvelopeAnalyzer plain;
    config.true_envelope_iterations = 0;
    REQUIRE(plain.prepare(config) == SourceFilterAnalysisStatus::Ok);
    std::vector<float> initial(129);
    REQUIRE(plain.estimate(sparse, initial).ok());
    auto maximum_residual = [&](const std::vector<float>& envelope) {
        float residual = 0.0f;
        for (std::size_t bin = 0; bin < sparse.size(); ++bin)
            residual = std::max(residual, sparse[bin] - envelope[bin]);
        return residual;
    };
    REQUIRE(maximum_residual(refined) < maximum_residual(initial));

    auto sentinel = refined;
    sparse[17] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(analyzer.estimate(sparse, refined).status ==
            SourceFilterAnalysisStatus::NonFiniteInput);
    REQUIRE(refined == sentinel);

    std::fill(sparse.begin(), sparse.end(), std::numeric_limits<float>::max());
    REQUIRE(analyzer.estimate(sparse, refined).status ==
            SourceFilterAnalysisStatus::NumericalOverflow);
    REQUIRE(refined == sentinel);

    CepstralEnvelopeAnalyzer convergent;
    config.true_envelope_iterations = 20;
    config.convergence_tolerance = 1e-5f;
    REQUIRE(convergent.prepare(config) == SourceFilterAnalysisStatus::Ok);
    std::vector<float> flat(129, 0.25f);
    REQUIRE(convergent.estimate(flat, refined).iterations_performed == 1);
}

TEST_CASE("Scaled autocorrelation distinguishes order and input geometry",
          "[signal][source-filter][fault]") {
    std::array<float, 2> samples{1.0f, -1.0f};
    std::array<float, 3> output{};
    REQUIRE(scaled_autocorrelation<float>(samples, 2, output).status ==
            SourceFilterAnalysisStatus::InvalidInputSize);
    REQUIRE(scaled_autocorrelation<float>(samples, -1, std::span<float>{}).status ==
            SourceFilterAnalysisStatus::InvalidOrder);
}

TEST_CASE("Scaled autocorrelation normalizes finite subnormal doubles",
          "[signal][source-filter][numeric]") {
    const auto subnormal = std::numeric_limits<double>::denorm_min();
    std::array<double, 3> samples{subnormal, -subnormal, subnormal};
    std::array<double, 2> output{};
    const auto result = scaled_autocorrelation<double>(samples, 1, output);
    REQUIRE(result.status == SourceFilterAnalysisStatus::Ok);
    REQUIRE(result.input_scale == subnormal);
    REQUIRE(output[0] == 1.0);
    REQUIRE(output[1] == Catch::Approx(-2.0 / 3.0));
}

TEST_CASE("Invalid reprepare preserves the last prepared analyzers",
          "[signal][source-filter][fault]") {
    CepstralEnvelopeAnalyzer envelope;
    CepstralEnvelopeConfig envelope_config;
    envelope_config.fft_size = 256;
    envelope_config.order = 8;
    REQUIRE(envelope.prepare(envelope_config) == SourceFilterAnalysisStatus::Ok);
    std::array<float, 129> log_magnitude{};
    std::array<float, 129> before{};
    REQUIRE(envelope.estimate(log_magnitude, before).ok());
    auto invalid_envelope_config = envelope_config;
    invalid_envelope_config.order = -1;
    REQUIRE(envelope.prepare(invalid_envelope_config) == SourceFilterAnalysisStatus::InvalidOrder);
    REQUIRE(envelope.prepared());
    REQUIRE(envelope.order() == 8);
    std::array<float, 129> after{};
    REQUIRE(envelope.estimate(log_magnitude, after).ok());
    REQUIRE(after == before);

    LpcAnalyzer lpc;
    REQUIRE(lpc.prepare(2) == SourceFilterAnalysisStatus::Ok);
    const auto samples = stable_ar2(4096);
    REQUIRE(lpc.analyze(samples).ok());
    const std::array<float, 2> coefficients{lpc.coefficients()[0], lpc.coefficients()[1]};
    REQUIRE(lpc.prepare(0) == SourceFilterAnalysisStatus::InvalidOrder);
    REQUIRE(lpc.prepared());
    REQUIRE(lpc.order() == 2);
    REQUIRE(lpc.result().ok());
    REQUIRE(lpc.coefficients()[0] == coefficients[0]);
    REQUIRE(lpc.coefficients()[1] == coefficients[1]);
}

TEST_CASE("Envelope shifter reports invalid configuration without losing prepared state",
          "[signal][source-filter][fault]") {
    SpectralEnvelopeShifter shifter;
    SpectralEnvelopeShifterConfig config;
    config.fft_size = 256;
    REQUIRE(shifter.prepare(config) == SourceFilterAnalysisStatus::Ok);
    REQUIRE(shifter.num_bins() == 129);

    config.true_envelope_iterations = kSourceFilterMaximumEnvelopeIterations + 1;
    REQUIRE(shifter.prepare(config) == SourceFilterAnalysisStatus::InvalidIterationCount);
    REQUIRE(shifter.num_bins() == 129);

    config.true_envelope_iterations = 3;
    config.fft_size = 255;
    REQUIRE(shifter.prepare(config) == SourceFilterAnalysisStatus::InvalidFftSize);
    REQUIRE(shifter.num_bins() == 129);

    config.fft_size = 256;
    for (const auto invalid_gain :
         {-1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        config.max_gain_db = invalid_gain;
        REQUIRE(shifter.prepare(config) == SourceFilterAnalysisStatus::InvalidGain);
        REQUIRE(shifter.num_bins() == 129);
    }
}

TEST_CASE("Envelope shifter rejects nonpositive and nonfinite warp without mutation",
          "[signal][source-filter][fault]") {
    SpectralEnvelopeShifter shifter;
    SpectralEnvelopeShifterConfig config;
    config.fft_size = 256;
    REQUIRE(shifter.prepare(config) == SourceFilterAnalysisStatus::Ok);

    std::array<std::complex<float>, 129> frame{};
    for (std::size_t bin = 0; bin < frame.size(); ++bin)
        frame[bin] = {0.1f + static_cast<float>(bin) * 0.001f, -0.2f};
    const auto original = frame;
    std::complex<float>* frames[] = {frame.data()};
    for (const auto invalid_warp : {-1.0f, 0.0f, std::numeric_limits<float>::infinity(),
                                    std::numeric_limits<float>::quiet_NaN()}) {
        shifter.process_group(frames, 1, 129, invalid_warp);
        REQUIRE(frame == original);
    }
}

TEST_CASE("FFT exposes backend readiness", "[signal][source-filter][fault]") {
    Fft empty;
    REQUIRE_FALSE(empty.ready());
    Fft prepared(256);
    REQUIRE(prepared.ready());
    Fft moved(std::move(prepared));
    REQUIRE(moved.ready());
    REQUIRE_FALSE(prepared.ready());
}

TEST_CASE("Autocorrelation and Levinson-Durbin recover known stable models",
          "[signal][source-filter][lpc]") {
    std::array<double, 4> autocorrelation{1.0, 0.8, 0.64, 0.512};
    std::array<double, 3> coefficients{};
    std::array<double, 3> reflections{};
    std::array<double, 3> workspace{};
    const auto result =
        levinson_durbin<double>(autocorrelation, coefficients, reflections, workspace, 1e-12);
    REQUIRE(result.ok());
    REQUIRE(coefficients[0] == Catch::Approx(-0.8).margin(1e-12));
    REQUIRE(coefficients[1] == Catch::Approx(0.0).margin(1e-12));
    REQUIRE(coefficients[2] == Catch::Approx(0.0).margin(1e-12));
    REQUIRE(reflections[0] == Catch::Approx(-0.8).margin(1e-12));
    REQUIRE(result.normalized_prediction_error == Catch::Approx(0.36).margin(1e-12));

    std::array<double, 3> singular{1.0, 1.0, 1.0};
    std::array<double, 2> singular_coefficients{9.0, 9.0};
    std::array<double, 2> singular_reflections{9.0, 9.0};
    std::array<double, 2> singular_workspace{};
    REQUIRE(levinson_durbin<double>(singular, singular_coefficients, singular_reflections,
                                    singular_workspace, 1e-12)
                .status == SourceFilterAnalysisStatus::RankDeficient);
    REQUIRE(singular_coefficients == std::array<double, 2>{});
    REQUIRE(singular_reflections == std::array<double, 2>{});
}

TEST_CASE("LPC analysis is deterministic and scale invariant over safe decades",
          "[signal][source-filter][lpc]") {
    const auto base = stable_ar2(32768);
    std::array<float, 3> scales{1e-6f, 1.0f, 1e6f};
    std::array<std::array<float, 2>, 3> coefficients{};
    std::array<float, 3> errors{};
    for (std::size_t index = 0; index < scales.size(); ++index) {
        std::vector<float> samples(base.size());
        for (std::size_t i = 0; i < base.size(); ++i)
            samples[i] = base[i] * scales[index];
        LpcAnalyzer analyzer;
        REQUIRE(analyzer.prepare(2) == SourceFilterAnalysisStatus::Ok);
        const auto first = analyzer.analyze(samples);
        REQUIRE(first.ok());
        coefficients[index] = {analyzer.coefficients()[0], analyzer.coefficients()[1]};
        errors[index] = first.prediction_error;
        const auto frozen = coefficients[index];
        REQUIRE(analyzer.analyze(samples).ok());
        REQUIRE(analyzer.coefficients()[0] == frozen[0]);
        REQUIRE(analyzer.coefficients()[1] == frozen[1]);
    }
    for (const auto& model : coefficients) {
        REQUIRE(model[0] == Catch::Approx(-0.75f).margin(0.02f));
        REQUIRE(model[1] == Catch::Approx(0.5f).margin(0.02f));
        REQUIRE(model[0] == Catch::Approx(coefficients[1][0]).margin(2e-6f));
        REQUIRE(model[1] == Catch::Approx(coefficients[1][1]).margin(2e-6f));
    }
    REQUIRE(errors[0] == Catch::Approx(errors[1] * 1e-12f).epsilon(2e-4f));
    REQUIRE(errors[2] == Catch::Approx(errors[1] * 1e12f).epsilon(2e-4f));
}

TEST_CASE("LPC analysis rejects degenerate nonfinite and overflowing results",
          "[signal][source-filter][fault]") {
    LpcAnalyzer analyzer;
    REQUIRE(analyzer.prepare(2) == SourceFilterAnalysisStatus::Ok);
    std::array<float, 8> silence{};
    REQUIRE(analyzer.analyze(silence).status == SourceFilterAnalysisStatus::DegenerateInput);
    REQUIRE(analyzer.coefficients().empty());

    auto nonfinite = silence;
    nonfinite[3] = std::numeric_limits<float>::infinity();
    REQUIRE(analyzer.analyze(nonfinite).status == SourceFilterAnalysisStatus::NonFiniteInput);
    REQUIRE(analyzer.coefficients().empty());

    auto huge = stable_ar2(4096, 1e30f);
    REQUIRE(analyzer.analyze(huge).status == SourceFilterAnalysisStatus::PredictionErrorOverflow);
    REQUIRE(analyzer.coefficients().empty());

    auto noise = deterministic_noise(32768);
    REQUIRE(analyzer.prepare(8) == SourceFilterAnalysisStatus::Ok);
    REQUIRE(analyzer.analyze(noise).ok());
    for (const auto coefficient : analyzer.coefficients())
        REQUIRE(std::abs(coefficient) < 0.03f);
}

TEST_CASE("All-pole response validates stability and matches analytic endpoints",
          "[signal][source-filter][response]") {
    std::array<double, 1> coefficients{-0.5};
    std::array<double, 1> workspace{};
    std::vector<double> response(129, -1.0);
    REQUIRE(all_pole_magnitude_response<double>(coefficients, 1.0, 48000.0, 256, response,
                                                workspace, 1e-12,
                                                1e-12) == SourceFilterAnalysisStatus::Ok);
    REQUIRE(response.front() == Catch::Approx(2.0).margin(1e-12));
    REQUIRE(response.back() == Catch::Approx(2.0 / 3.0).margin(1e-12));

    coefficients[0] = 1.01;
    const auto sentinel = response;
    REQUIRE(all_pole_magnitude_response<double>(coefficients, 1.0, 48000.0, 256, response,
                                                workspace, 1e-12, 1e-12) ==
            SourceFilterAnalysisStatus::UnstableModel);
    REQUIRE(response == sentinel);
}

TEST_CASE("Schur stability validates second-order models and controls",
          "[signal][source-filter][stability]") {
    std::array<double, 2> stable{-0.75, 0.5};
    std::array<double, 2> workspace{};
    REQUIRE(lpc_stability<double>(stable, workspace, 1e-12) == SourceFilterAnalysisStatus::Ok);

    std::array<double, 2> unstable{0.0, 1.01};
    REQUIRE(lpc_stability<double>(unstable, workspace, 1e-12) ==
            SourceFilterAnalysisStatus::UnstableModel);

    std::array<double, 1> short_workspace{};
    REQUIRE(lpc_stability<double>(stable, short_workspace, 1e-12) ==
            SourceFilterAnalysisStatus::InvalidWorkspaceSize);

    stable[1] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(lpc_stability<double>(stable, workspace, 1e-12) ==
            SourceFilterAnalysisStatus::NonFiniteInput);
}

TEST_CASE("Prepared source-filter analysis paths allocate nothing",
          "[signal][source-filter][rt-safety]") {
    CepstralEnvelopeAnalyzer envelope;
    CepstralEnvelopeConfig envelope_config;
    envelope_config.fft_size = 256;
    envelope_config.order = 16;
    REQUIRE(envelope.prepare(envelope_config) == SourceFilterAnalysisStatus::Ok);
    std::array<float, 129> log_magnitude{};
    std::array<float, 129> log_envelope{};

    LpcAnalyzer lpc;
    REQUIRE(lpc.prepare(8) == SourceFilterAnalysisStatus::Ok);
    auto samples = deterministic_noise(1024);
    std::array<float, 8> response_workspace{};
    std::array<float, 129> response{};

    pulp::test::RtAllocationProbe probe;
    REQUIRE(envelope.estimate(log_magnitude, log_envelope).ok());
    REQUIRE(lpc.analyze(samples).ok());
    REQUIRE(all_pole_magnitude_response<float>(
                lpc.coefficients(), std::sqrt(lpc.result().prediction_error), 48000.0f, 256,
                response, response_workspace) == SourceFilterAnalysisStatus::Ok);
    REQUIRE_FALSE(probe.saw_allocation());
}

TEST_CASE("Extracted envelope storage retains the legacy byte budget",
          "[signal][source-filter][memory]") {
    constexpr int fft_size = 2048;
    constexpr std::uint64_t ceiling = 1ull << 30;
    std::uint64_t reported = 0;
    REQUIRE(SpectralEnvelopeShifter::checked_retained_bytes(fft_size, ceiling, reported));

    const auto bins = static_cast<std::uint64_t>(fft_size / 2 + 1);
    std::uint64_t fft_bytes = 0;
    REQUIRE(checked_fft_retained_bytes<float>(fft_size, ceiling, fft_bytes));
    CheckedRetainedByteCharge legacy(ceiling);
    REQUIRE(legacy.add_repeated<float>(bins, 3));
    REQUIRE(legacy.add<std::complex<float>>(fft_size));
    REQUIRE(legacy.add_retained_bytes(fft_bytes));
    REQUIRE(reported == legacy.total());
}
