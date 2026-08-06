#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/true_peak_limiter.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

double sinc(double x) {
    if (std::abs(x) < 1.0e-15)
        return 1.0;
    return std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
}

double bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k <= 32; ++k) {
        term *= x * x * 0.25 / static_cast<double>(k * k);
        sum += term;
    }
    return sum;
}

// Independent, non-streaming Kaiser-sinc reconstruction oracle. Its 257 taps
// and beta differ from the limiter's 8x/129-tap detector. The broad matrix uses
// 32x, while a compact confirmation below doubles the phase density to 64x.
double oracle_true_peak(std::span<const double> signal, std::size_t begin = 0, int factor = 32) {
    constexpr int radius = 128;
    constexpr double beta = 14.0;
    const double denominator = bessel_i0(beta);
    double peak = 0.0;
    for (std::size_t frame = begin; frame + 1 < signal.size(); ++frame) {
        for (int phase = 0; phase < factor; ++phase) {
            const double position =
                static_cast<double>(frame) + static_cast<double>(phase) / factor;
            double value = 0.0;
            double normalization = 0.0;
            const int first = std::max(0, static_cast<int>(frame) - radius);
            const int last =
                std::min(static_cast<int>(signal.size()) - 1, static_cast<int>(frame) + radius);
            for (int sample = first; sample <= last; ++sample) {
                const double distance = position - sample;
                const double normalized = distance / radius;
                const double window =
                    std::abs(normalized) <= 1.0
                        ? bessel_i0(beta *
                                    std::sqrt(std::max(0.0, 1.0 - normalized * normalized))) /
                              denominator
                        : 0.0;
                const double coefficient = sinc(distance) * window;
                value += signal[static_cast<std::size_t>(sample)] * coefficient;
                normalization += coefficient;
            }
            if (std::abs(normalization) > 1.0e-12)
                value /= normalization;
            peak = std::max(peak, std::abs(value));
        }
    }
    return peak;
}

// Exact least-squares amplitude oracle for a bandlimited single sine. This is
// independent of both reconstruction grids and remains exact when the tested
// window does not contain an integer number of periods.
double sine_amplitude_oracle(std::span<const double> signal, std::size_t begin,
                             double frequency_cycles_per_sample) {
    double ss = 0.0, cc = 0.0, sc = 0.0, sy = 0.0, cy = 0.0;
    for (std::size_t i = begin; i < signal.size(); ++i) {
        const double angle =
            2.0 * std::numbers::pi * frequency_cycles_per_sample * static_cast<double>(i);
        const double s = std::sin(angle);
        const double c = std::cos(angle);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        sy += s * signal[i];
        cy += c * signal[i];
    }
    const double determinant = ss * cc - sc * sc;
    const double a = (sy * cc - cy * sc) / determinant;
    const double b = (cy * ss - sy * sc) / determinant;
    return std::hypot(a, b);
}

double detector_peak(std::span<const double> signal) {
    pulp::signal::TruePeakLimiter64 limiter;
    REQUIRE(limiter.prepare(48000.0, 1));
    std::array<double, pulp::signal::TruePeakLimiter64::interpolation_taps()> window{};
    double peak = 0.0;
    for (std::size_t frame = window.size() - 1; frame < signal.size(); ++frame) {
        for (std::size_t tap = 0; tap < window.size(); ++tap)
            window[tap] = signal[frame - tap];
        const auto phases = limiter.detector_phases(window);
        for (const double phase : phases)
            peak = std::max(peak, std::abs(phase));
    }
    return peak;
}

template <typename Limiter>
std::vector<double> render(Limiter& limiter, std::span<const double> input,
                           std::size_t block_size) {
    std::vector<double> output(input.size());
    std::vector<typename std::conditional_t<std::is_same_v<Limiter, pulp::signal::TruePeakLimiter>,
                                            float, double>>
        converted(input.size());
    using T = typename decltype(converted)::value_type;
    for (std::size_t i = 0; i < input.size(); ++i)
        converted[i] = static_cast<T>(input[i]);
    std::vector<T> rendered(input.size());
    for (std::size_t offset = 0; offset < input.size(); offset += block_size) {
        const auto count = std::min(block_size, input.size() - offset);
        REQUIRE(limiter.process_interleaved(converted.data() + offset, rendered.data() + offset,
                                            count, 1));
    }
    for (std::size_t i = 0; i < output.size(); ++i)
        output[i] = rendered[i];
    return output;
}

std::vector<double> adversarial_program(std::size_t frames, double sample_rate) {
    std::vector<double> result(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        const double multitone =
            0.43 * std::sin(2.0 * std::numbers::pi * 0.477 * sample_rate * t + 0.31) +
            0.31 * std::sin(2.0 * std::numbers::pi * 0.231 * sample_rate * t + 1.17) +
            0.22 * std::sin(2.0 * std::numbers::pi * 997.0 * t + 2.03);
        result[i] = multitone;
    }
    if (frames > 4096) {
        result[2048] += 1.7;
        result[4096] -= 1.8;
    }
    return result;
}

} // namespace

TEST_CASE("True-peak limiter reports exact latency and delayed impulse",
          "[signal][limiter][latency]") {
    pulp::signal::TruePeakLimiter64 limiter;
    pulp::signal::TruePeakLimiter64::Params params;
    params.lookahead_ms = 3.0;
    REQUIRE(limiter.prepare(48000.0, 1, params));
    REQUIRE(limiter.latency_samples() == 64 + 144);
    REQUIRE(limiter.tail_samples() == limiter.latency_samples());

    std::vector<double> input(1024, 0.0), output(1024, 0.0);
    input[0] = 0.25;
    REQUIRE(limiter.process_interleaved(input.data(), output.data(), input.size(), 1));
    const auto peak = std::max_element(output.begin(), output.end());
    REQUIRE(std::distance(output.begin(), peak) == limiter.latency_samples());
    REQUIRE_THAT(*peak, WithinAbs(0.25, 1.0e-15));
}

TEST_CASE("True-peak limiter beats a planted sample-peak clamp", "[signal][limiter][oracle]") {
    constexpr double ceiling_db = -1.0;
    const double ceiling = std::pow(10.0, ceiling_db / 20.0);
    std::vector<double> planted(8192);
    for (std::size_t i = 0; i < planted.size(); ++i) {
        constexpr std::array<double, 4> cell{0.88, 0.88, -0.88, -0.88};
        planted[i] = cell[i & 3u];
    }
    std::vector<double> sample_clamped = planted;
    for (double& sample : sample_clamped)
        sample = std::clamp(sample, -ceiling, ceiling);
    const double failed_peak = oracle_true_peak(sample_clamped, 512);
    INFO("sample-clamp true peak=" << failed_peak << " ceiling=" << ceiling);
    REQUIRE(failed_peak > ceiling * 1.03);

    pulp::signal::TruePeakLimiter64 limiter;
    pulp::signal::TruePeakLimiter64::Params params;
    params.ceiling_dbtp = ceiling_db;
    params.lookahead_ms = 5.0;
    REQUIRE(limiter.prepare(48000.0, 1, params));
    auto output = render(limiter, planted, 113);
    const double limited_peak = oracle_true_peak(output, 1024);
    INFO("limited true peak=" << limited_peak << " ceiling=" << ceiling);
    REQUIRE(limited_peak <= ceiling * 1.002);
    const double limited_peak_64x =
        oracle_true_peak(std::span<const double>(output).subspan(2048, 2048), 128, 64);
    INFO("64x confirmation true peak=" << limited_peak_64x << " ceiling=" << ceiling);
    REQUIRE(limited_peak_64x <= ceiling * 1.002);
    REQUIRE(limiter.gain_reduction_db() > 0.0);

    pulp::signal::TruePeakLimiter single_precision;
    pulp::signal::TruePeakLimiter::Params float_params;
    float_params.ceiling_dbtp = ceiling_db;
    float_params.lookahead_ms = 5.0;
    REQUIRE(single_precision.prepare(48000.0, 1, float_params));
    auto float_output = render(single_precision, planted, 127);
    const double float_peak = oracle_true_peak(float_output, 1024);
    INFO("float limited true peak=" << float_peak << " ceiling=" << ceiling);
    REQUIRE(float_peak <= ceiling * 1.0025);
}

TEST_CASE("True-peak limiter contains adversarial high-frequency material",
          "[signal][limiter][oracle]") {
    for (const double sample_rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
        auto input = adversarial_program(18000, sample_rate);
        const double reference_input_peak = oracle_true_peak(input, 512);
        const double detector_input_peak = detector_peak(input);
        const double under_read_db = 20.0 * std::log10(reference_input_peak / detector_input_peak);
        INFO("sample_rate=" << sample_rate << " detector under-read dB=" << under_read_db);
        REQUIRE(under_read_db <= pulp::signal::TruePeakLimiter64::detector_guard_db());
        pulp::signal::TruePeakLimiter64 limiter;
        pulp::signal::TruePeakLimiter64::Params params;
        params.ceiling_dbtp = -0.5;
        params.lookahead_ms = 4.0;
        params.release_ms = 80.0;
        REQUIRE(limiter.prepare(sample_rate, 1, params));
        auto output = render(limiter, input, 257);
        const double oracle = oracle_true_peak(output, 2048);
        const double ceiling = std::pow(10.0, params.ceiling_dbtp / 20.0);
        INFO("sample_rate=" << sample_rate << " oracle=" << oracle << " ceiling=" << ceiling);
        REQUIRE(oracle <= ceiling * 1.003);
    }
}

TEST_CASE("True-peak ceiling survives frequency phase rate and realization matrix",
          "[signal][limiter][oracle][matrix]") {
    constexpr std::array<double, 6> sample_rates{8000.0,  44100.0,  48000.0,
                                                 96000.0, 192000.0, 384000.0};
    constexpr std::array<double, 3> lookaheads{0.0, 5.0, 10.0};
    constexpr std::array<double, 3> frequencies{0.173, 0.4, 0.45};
    constexpr std::array<double, 4> phases{0.0, 0.19, 0.43, 0.77};
    constexpr double ceiling_db = -1.0;
    const double ceiling = std::pow(10.0, ceiling_db / 20.0);
    double worst_ratio = 0.0;

    const auto exercise = [&](auto precision_tag) {
        using Limiter = decltype(precision_tag);
        for (const double sample_rate : sample_rates) {
            for (const double lookahead : lookaheads) {
                for (const double frequency : frequencies) {
                    for (const double phase : phases) {
                        Limiter limiter;
                        typename Limiter::Params params;
                        params.ceiling_dbtp = ceiling_db;
                        params.lookahead_ms = lookahead;
                        params.release_ms = 20.0;
                        REQUIRE(limiter.prepare(sample_rate, 1, params));
                        const std::size_t frames =
                            static_cast<std::size_t>(limiter.latency_samples()) + 1024;
                        std::vector<double> input(frames);
                        for (std::size_t i = 0; i < frames; ++i) {
                            input[i] = 1.2 * std::sin(2.0 * std::numbers::pi *
                                                      (frequency * static_cast<double>(i) + phase));
                        }
                        const auto output = render(limiter, input, 137);
                        const double reconstructed =
                            sine_amplitude_oracle(output, frames - 512, frequency);
                        CAPTURE(sample_rate, lookahead, frequency, phase, reconstructed, ceiling);
                        worst_ratio = std::max(worst_ratio, reconstructed / ceiling);
                        REQUIRE(reconstructed <= ceiling * 1.0002);
                    }
                }
            }
        }
    };

    exercise(pulp::signal::TruePeakLimiter64{});
    exercise(pulp::signal::TruePeakLimiter{});
    INFO("worst reconstructed/ceiling ratio=" << worst_ratio);
    REQUIRE(worst_ratio <= 1.0002);
}

TEST_CASE("A four-phase detector with a 0.20 dB guard has a planted grid breach",
          "[signal][limiter][oracle][mutation]") {
    double four_phase_peak = 0.0;
    for (int point = 0; point < 40; ++point) {
        const double position = static_cast<double>(point) / 4.0;
        four_phase_peak =
            std::max(four_phase_peak, std::abs(std::sin(2.0 * std::numbers::pi * 0.4 * position)));
    }
    const double guarded = four_phase_peak * std::pow(10.0, 0.20 / 20.0);
    INFO("4x grid peak=" << four_phase_peak << " after old guard=" << guarded);
    REQUIRE(20.0 * std::log10(1.0 / four_phase_peak) > 0.43);
    REQUIRE(guarded < 1.0);
}

TEST_CASE("True-peak limiter is block-partition deterministic", "[signal][limiter][determinism]") {
    const auto input = adversarial_program(12000, 48000.0);
    pulp::signal::TruePeakLimiter64 scalar;
    pulp::signal::TruePeakLimiter64 chunked;
    REQUIRE(scalar.prepare(48000.0, 1));
    REQUIRE(chunked.prepare(48000.0, 1));
    REQUIRE(render(scalar, input, 1) == render(chunked, input, 511));
}

TEST_CASE("True-peak limiter channel linking is explicit", "[signal][limiter][channels]") {
    std::vector<double> input(4096 * 2, 0.0), linked_output(input.size()),
        independent_output(input.size());
    for (std::size_t frame = 0; frame < 4096; ++frame) {
        input[2 * frame] = frame == 1000 ? 2.0 : 0.1;
        input[2 * frame + 1] = 0.2;
    }
    pulp::signal::TruePeakLimiter64 linked;
    pulp::signal::TruePeakLimiter64 independent;
    pulp::signal::TruePeakLimiter64::Params linked_params;
    pulp::signal::TruePeakLimiter64::Params independent_params;
    independent_params.channel_link = pulp::signal::TruePeakLimiter64::ChannelLink::independent;
    REQUIRE(linked.prepare(48000.0, 2, linked_params));
    REQUIRE(independent.prepare(48000.0, 2, independent_params));
    REQUIRE(linked.process_interleaved(input.data(), linked_output.data(), 4096, 2));
    REQUIRE(independent.process_interleaved(input.data(), independent_output.data(), 4096, 2));
    REQUIRE(linked.gain_reduction_db(0) == linked.gain_reduction_db(1));
    REQUIRE(independent.gain_reduction_db(0) > independent.gain_reduction_db(1));
}

TEST_CASE(
    "True-peak limiter reset, finite recovery, controls, precision and RT storage are bounded",
    "[signal][limiter][rt-safety]") {
    pulp::signal::TruePeakLimiter limiter;
    REQUIRE_FALSE(limiter.prepare(0.0, 1));
    REQUIRE_FALSE(limiter.prepare(48000.0, 0));
    REQUIRE(limiter.prepare(384000.0, 1));
    REQUIRE_FALSE(limiter.set_ceiling_dbtp(std::numeric_limits<double>::quiet_NaN()));
    REQUIRE_FALSE(limiter.set_release_ms(std::numeric_limits<double>::infinity()));
    REQUIRE(limiter.set_ceiling_dbtp(-3.0));
    REQUIRE(limiter.set_release_ms(250.0));

    std::array<float, 1> input{std::numeric_limits<float>::infinity()};
    std::array<float, 1> output{1.0f};
    REQUIRE_FALSE(limiter.process_frame(input, output));
    REQUIRE(output[0] == 0.0f);
    REQUIRE(limiter.fault_count() == 1);
    input[0] = 0.25f;
    REQUIRE(limiter.process_frame(input, output));
    REQUIRE(std::isfinite(output[0]));

    pulp::test::RtAllocationProbe allocation_probe;
    for (int i = 0; i < 4096; ++i) {
        input[0] = static_cast<float>(0.8 * std::sin(0.17 * i));
        REQUIRE(limiter.process_frame(input, output));
    }
    REQUIRE(allocation_probe.allocation_count() == 0);

    limiter.reset();
    pulp::signal::TruePeakLimiter fresh;
    REQUIRE(fresh.prepare(384000.0, 1));
    REQUIRE(fresh.set_ceiling_dbtp(-3.0));
    REQUIRE(fresh.set_release_ms(250.0));
    for (int i = 0; i < 512; ++i) {
        input[0] = i == 0 ? 0.5f : 0.0f;
        std::array<float, 1> reference{};
        REQUIRE(limiter.process_frame(input, output));
        REQUIRE(fresh.process_frame(input, reference));
        REQUIRE(output == reference);
    }
}

TEST_CASE("Finite maximum inputs cannot overflow the detector or output",
          "[signal][limiter][overflow][mutation]") {
    pulp::signal::TruePeakLimiter64 limiter;
    pulp::signal::TruePeakLimiter64::Params params;
    params.ceiling_dbtp = -1.0;
    params.lookahead_ms = 0.0;
    REQUIRE(limiter.prepare(48000.0, 2, params));
    const double ceiling = std::pow(10.0, params.ceiling_dbtp / 20.0);
    std::array<double, 2> input{}, output{};
    for (int frame = 0; frame < limiter.latency_samples() + 512; ++frame) {
        input[0] = (frame & 1) == 0 ? std::numeric_limits<double>::max()
                                    : -std::numeric_limits<double>::max();
        input[1] = -input[0];
        REQUIRE(limiter.process_frame(input, output));
        REQUIRE(std::isfinite(output[0]));
        REQUIRE(std::isfinite(output[1]));
        REQUIRE(std::abs(output[0]) <= ceiling);
        REQUIRE(std::abs(output[1]) <= ceiling);
    }
    REQUIRE(limiter.fault_count() == 0);

    input[0] = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(limiter.process_frame(input, output));
    REQUIRE(limiter.fault_count() == 1);
    input = {0.25, -0.25};
    REQUIRE(limiter.process_frame(input, output));
    REQUIRE(std::isfinite(output[0]));
}

TEST_CASE("Precomputed controls remain RT safe under sample-accurate changes",
          "[signal][limiter][controls][mutation]") {
    pulp::signal::TruePeakLimiter limiter;
    REQUIRE(limiter.prepare(48000.0, 1));
    std::array<double, 64> safe_ceilings{}, release_retains{};
    for (std::size_t i = 0; i < safe_ceilings.size(); ++i) {
        const double unit = static_cast<double>(i) / static_cast<double>(safe_ceilings.size() - 1);
        const double ceiling = -24.0 + 24.0 * unit;
        const double release = 5.0 + 1995.0 * unit;
        safe_ceilings[i] =
            std::pow(10.0, (ceiling - pulp::signal::TruePeakLimiter::detector_guard_db()) / 20.0);
        release_retains[i] = pulp::signal::dynamics::one_pole_retain(release * 0.001, 48000.0);
    }
    std::array<float, 1> input{0.9f}, output{};
    pulp::test::RtAllocationProbe allocation_probe;
    for (int frame = 0; frame < 4096; ++frame) {
        const std::size_t index = static_cast<std::size_t>(frame) % safe_ceilings.size();
        const double unit =
            static_cast<double>(index) / static_cast<double>(safe_ceilings.size() - 1);
        REQUIRE(limiter.set_realtime_control_coefficients(-24.0 + 24.0 * unit, safe_ceilings[index],
                                                          5.0 + 1995.0 * unit,
                                                          release_retains[index]));
        REQUIRE(limiter.process_frame(input, output));
        REQUIRE(std::isfinite(output[0]));
    }
    REQUIRE(allocation_probe.allocation_count() == 0);
}

TEST_CASE("Advertised 384 kHz stereo envelope has Release headroom",
          "[.benchmark][signal][limiter]") {
    pulp::signal::TruePeakLimiter limiter;
    REQUIRE(limiter.prepare(384000.0, 2));
    std::vector<float> input(384000 * 2, 0.8f), output(input.size());
    const auto begin = std::chrono::steady_clock::now();
    REQUIRE(limiter.process_interleaved(input.data(), output.data(), 384000, 2));
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    INFO("384 kHz stereo seconds-per-second=" << elapsed);
#if defined(NDEBUG)
    REQUIRE(elapsed < 0.80);
#endif
}
