#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/dither.hpp>
#include <pulp/signal/lofi_chain.hpp>
#include <pulp/signal/velvet_noise.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <type_traits>
#include <vector>

namespace {

using pulp::signal::DitherMode;
using pulp::signal::DitherQuantizer64;
using pulp::signal::NoiseShapingOrder;

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr std::uint32_t kSeed = 0x12345678u;

std::uint32_t oracle_xorshift(std::uint32_t& state) {
    if (state == 0u)
        state = pulp::signal::Xorshift32::kDefaultSeed;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

long double oracle_unit(std::uint32_t& state) {
    return static_cast<long double>(oracle_xorshift(state)) /
           static_cast<long double>(std::uint64_t{1} << 32);
}

double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    REQUIRE(a.size() == b.size());
    const double ma = std::accumulate(a.begin(), a.end(), 0.0) / a.size();
    const double mb = std::accumulate(b.begin(), b.end(), 0.0) / b.size();
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double da = a[i] - ma;
        const double db = b[i] - mb;
        ab += da * db;
        aa += da * da;
        bb += db * db;
    }
    return ab / std::sqrt(aa * bb);
}

double band_energy(const std::vector<double>& x, double lo, double hi) {
    constexpr int kBins = 24;
    double total = 0.0;
    for (int bin = 0; bin < kBins; ++bin) {
        const double frequency = lo + (hi - lo) * (bin + 0.5) / kBins;
        double re = 0.0;
        double im = 0.0;
        for (std::size_t n = 0; n < x.size(); ++n) {
            const double phase = 2.0 * kPi * frequency * static_cast<double>(n);
            re += x[n] * std::cos(phase);
            im -= x[n] * std::sin(phase);
        }
        total += re * re + im * im;
    }
    return total / kBins;
}

std::vector<double> render_noise(NoiseShapingOrder order, double sample_rate) {
    constexpr std::size_t kFrames = 8192;
    DitherQuantizer64 quantizer;
    quantizer.set_bits(10.0);
    quantizer.set_dither_mode(DitherMode::tpdf);
    quantizer.set_noise_shaping(order);
    quantizer.set_seed(kSeed);
    quantizer.reset();

    std::vector<double> output(kFrames);
    const double dc = 0.137 + 0.001 * (sample_rate / 48000.0 - 1.0);
    for (double& sample : output)
        sample = quantizer.process(dc) - dc;
    return output;
}

} // namespace

static_assert(std::is_trivially_copyable_v<pulp::signal::NoiseShaperStateT<double>>);

TEST_CASE("TPDF transform has the expected zero mean and triangular variance",
          "[signal][dither][statistics]") {
    constexpr int kDraws = 200000;
    pulp::signal::Xorshift32 rng_a(kSeed);
    pulp::signal::Xorshift32 rng_b(0x9ABCDEF0u);
    long double sum = 0.0;
    long double sum_sq = 0.0;
    std::array<int, 10> histogram{};
    for (int i = 0; i < kDraws; ++i) {
        const double draw =
            pulp::signal::tpdf_difference(rng_a.next_unit<double>(), rng_b.next_unit<double>());
        sum += draw;
        sum_sq += draw * draw;
        const int bucket = std::clamp(static_cast<int>((draw + 1.0) * 5.0), 0, 9);
        ++histogram[static_cast<std::size_t>(bucket)];
    }
    const double mean = static_cast<double>(sum / kDraws);
    const double variance = static_cast<double>(sum_sq / kDraws - mean * mean);
    INFO("mean=" << mean << " variance=" << variance);
    CHECK(std::abs(mean) < 0.004);
    CHECK(variance == Catch::Approx(1.0 / 6.0).margin(0.003));
    CHECK(histogram[4] > histogram[0] * 4);
    CHECK(histogram[5] > histogram[9] * 4);
}

TEST_CASE("Dithered quantization matches an independent long-double oracle",
          "[signal][dither][oracle]") {
    constexpr long double kBits = 9.0L;
    constexpr long double kStep = 1.0L / 256.0L;
    DitherQuantizer64 quantizer;
    quantizer.set_bits(static_cast<double>(kBits));
    quantizer.set_dither_mode(DitherMode::tpdf);
    quantizer.set_noise_shaping(NoiseShapingOrder::second);
    quantizer.set_seed(kSeed);
    quantizer.reset();

    std::uint32_t oracle_rng = kSeed;
    long double error_1 = 0.0L;
    long double error_2 = 0.0L;
    for (int n = 0; n < 4096; ++n) {
        const long double input =
            0.4L * std::sin(2.0L * static_cast<long double>(kPi) * 997.0L * n / 48000.0L);
        const long double dither = (oracle_unit(oracle_rng) - oracle_unit(oracle_rng)) * kStep;
        const long double shaped = input - 2.0L * error_1 + error_2 + dither;
        const long double expected = std::round(shaped / kStep) * kStep;
        const long double error = expected - input - (-2.0L * error_1 + error_2);
        error_2 = error_1;
        error_1 = std::clamp(error, -4.0L * kStep, 4.0L * kStep);
        CHECK(quantizer.process(static_cast<double>(input)) ==
              Catch::Approx(static_cast<double>(expected)).margin(1.0e-15));
    }
}

TEST_CASE("TPDF dither decorrelates low-level quantization error",
          "[signal][dither][correlation]") {
    constexpr std::size_t kFrames = 32768;
    constexpr double kAmplitude = 0.003;
    std::vector<double> input(kFrames);
    for (std::size_t n = 0; n < kFrames; ++n)
        input[n] = kAmplitude * std::sin(2.0 * kPi * 997.0 * n / 48000.0);

    auto errors = [&](DitherMode mode) {
        DitherQuantizer64 quantizer;
        quantizer.set_bits(8.0);
        quantizer.set_dither_mode(mode);
        quantizer.set_seed(kSeed);
        quantizer.reset();
        std::vector<double> result(kFrames);
        for (std::size_t n = 0; n < kFrames; ++n)
            result[n] = quantizer.process(input[n]) - input[n];
        return result;
    };

    const double undithered = std::abs(correlation(input, errors(DitherMode::none)));
    const double dithered = std::abs(correlation(input, errors(DitherMode::tpdf)));
    INFO("undithered correlation=" << undithered << " dithered=" << dithered);
    CHECK(undithered > 0.5);
    CHECK(dithered < 0.03);
}

TEST_CASE("Error feedback moves quantization noise toward high frequencies",
          "[signal][dither][spectrum]") {
    constexpr double kFirstOrderMinimumTiltGain = 1.5;
    constexpr double kSecondOverFirstMinimumTiltGain = 2.0;
    for (double sample_rate : {44100.0, 48000.0, 96000.0}) {
        const auto flat = render_noise(NoiseShapingOrder::none, sample_rate);
        const auto first = render_noise(NoiseShapingOrder::first, sample_rate);
        const auto second = render_noise(NoiseShapingOrder::second, sample_rate);
        auto ratio = [](const std::vector<double>& x) {
            return band_energy(x, 0.30, 0.47) / band_energy(x, 0.02, 0.10);
        };
        const double flat_ratio = ratio(flat);
        const double first_ratio = ratio(first);
        const double second_ratio = ratio(second);
        INFO("sample_rate=" << sample_rate << " flat=" << flat_ratio << " first=" << first_ratio
                            << " second=" << second_ratio);
        CHECK(first_ratio > flat_ratio * kFirstOrderMinimumTiltGain);
        CHECK(second_ratio > first_ratio * kSecondOverFirstMinimumTiltGain);
    }
}

TEST_CASE("Dither state is deterministic, partition invariant, and opt-in",
          "[signal][dither][determinism]") {
    std::vector<double> input(4097);
    for (std::size_t n = 0; n < input.size(); ++n)
        input[n] = 0.2 * std::sin(2.0 * kPi * 733.0 * n / 48000.0);

    DitherQuantizer64 quantizer;
    quantizer.set_bits(7.0);
    quantizer.set_seed(kSeed);
    quantizer.reset();
    const auto initial_rng = quantizer.rng_state();
    for (double sample : input)
        (void)quantizer.process(sample);
    CHECK(quantizer.rng_state() == initial_rng);

    quantizer.set_dither_mode(DitherMode::tpdf);
    quantizer.set_noise_shaping(NoiseShapingOrder::first);
    quantizer.reset();
    std::vector<double> one_block = input;
    quantizer.process(one_block.data(), static_cast<int>(one_block.size()));

    quantizer.reset();
    std::vector<double> partitioned = input;
    constexpr std::array<int, 7> kBlocks{1, 17, 64, 3, 511, 128, 29};
    std::size_t offset = 0;
    std::size_t block = 0;
    while (offset < partitioned.size()) {
        const auto count =
            std::min<std::size_t>(kBlocks[block++ % kBlocks.size()], partitioned.size() - offset);
        quantizer.process(partitioned.data() + offset, static_cast<int>(count));
        offset += count;
    }
    CHECK(partitioned == one_block);

    quantizer.reset();
    std::vector<double> repeated = input;
    quantizer.process(repeated.data(), static_cast<int>(repeated.size()));
    CHECK(repeated == one_block);
    quantizer.set_seed(kSeed + 1u);
    quantizer.reset();
    std::vector<double> different = input;
    quantizer.process(different.data(), static_cast<int>(different.size()));
    CHECK(different != one_block);
}

TEST_CASE("Dithered silence and DC remain finite and unbiased", "[signal][dither][silence][dc]") {
    constexpr int kFrames = 131072;
    constexpr double kMeanErrorToleranceLsb = 0.02;
    const double step = pulp::signal::quantization_step(8.0);

    DitherQuantizer64 quantizer;
    quantizer.set_bits(8.0);
    quantizer.set_seed(kSeed);
    quantizer.reset();
    for (int n = 0; n < 1024; ++n)
        CHECK(quantizer.process(0.0) == 0.0);

    quantizer.set_dither_mode(DitherMode::tpdf);
    quantizer.reset();
    long double silence_sum = 0.0;
    int nonzero = 0;
    for (int n = 0; n < kFrames; ++n) {
        const double output = quantizer.process(0.0);
        REQUIRE(std::isfinite(output));
        silence_sum += output;
        nonzero += output != 0.0;
    }
    CHECK(nonzero > kFrames / 5);
    CHECK(std::abs(static_cast<double>(silence_sum / kFrames)) < kMeanErrorToleranceLsb * step);

    quantizer.reset();
    constexpr double kDc = 0.12345;
    long double dc_error_sum = 0.0;
    for (int n = 0; n < kFrames; ++n)
        dc_error_sum += quantizer.process(kDc) - kDc;
    CHECK(std::abs(static_cast<double>(dc_error_sum / kFrames)) < kMeanErrorToleranceLsb * step);
}

TEST_CASE("Dither quantizer clears non-finite and overload-poisoned state",
          "[signal][dither][recovery]") {
    DitherQuantizer64 quantizer;
    quantizer.set_bits(8.0);
    quantizer.set_dither_mode(DitherMode::tpdf);
    quantizer.set_noise_shaping(NoiseShapingOrder::second);
    quantizer.set_seed(kSeed);
    quantizer.reset();
    for (int i = 0; i < 100; ++i)
        (void)quantizer.process(0.1);

    CHECK(quantizer.process(std::numeric_limits<double>::quiet_NaN()) == 0.0);
    CHECK(quantizer.state().error_1 == 0.0);
    CHECK(quantizer.state().error_2 == 0.0);
    CHECK(quantizer.process(std::numeric_limits<double>::max()) == 0.0);
    CHECK(quantizer.state().error_1 == 0.0);
    CHECK(quantizer.state().error_2 == 0.0);
    CHECK(std::isfinite(quantizer.process(0.125)));
    CHECK(std::abs(quantizer.state().error_1) <= 4.0 * pulp::signal::quantization_step(8.0));
}

TEST_CASE("Lo-fi dither policy preserves the legacy default exactly",
          "[signal][lofi][dither][legacy]") {
    pulp::signal::LofiChain64 chain;
    chain.set_sample_rate(48000.0);
    chain.set_bits(6.25);
    chain.set_seed(kSeed);
    chain.reset();
    for (int n = 0; n < 8192; ++n) {
        const double input = 0.93 * std::sin(2.0 * kPi * 997.0 * n / 48000.0);
        CHECK(chain.process(input) == pulp::signal::quantize_bits(input, 6.25));
    }
}

TEST_CASE("Dither and velvet generators allocate nothing in real-time use",
          "[signal][dither][velvet-noise][rt-safety]") {
    DitherQuantizer64 quantizer;
    quantizer.set_bits(9.0);
    quantizer.set_dither_mode(DitherMode::tpdf);
    quantizer.set_noise_shaping(NoiseShapingOrder::second);
    quantizer.reset();
    pulp::signal::VelvetNoiseGrid64 velvet;
    velvet.set_seed(123u);
    velvet.reset();

    double sink = 0.0;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int n = 0; n < 100000; ++n) {
            sink += quantizer.process(std::sin(0.01 * n));
            const auto draw = velvet.next();
            sink += draw.jitter * static_cast<double>(draw.sign);
        }
        allocations = probe.allocation_count();
    }
    INFO("sink=" << sink);
    CHECK(std::isfinite(sink));
    CHECK(allocations == 0);
}

TEST_CASE("Velvet draws are coordinate keyed and reset exactly",
          "[signal][velvet-noise][determinism]") {
    constexpr std::uint64_t kVelvetSeed = 0xCAFEBABE12345678ull;
    pulp::signal::VelvetNoiseGrid64 generator;
    generator.set_seed(kVelvetSeed);
    generator.reset();
    std::vector<pulp::signal::VelvetNoiseDrawT<double>> first(2048);
    for (auto& draw : first)
        draw = generator.next();
    CHECK(generator.index() == first.size());

    generator.reset();
    int positive = 0;
    int negative = 0;
    long double jitter_sum = 0.0;
    for (std::size_t index = 0; index < first.size(); ++index) {
        const auto sequential = generator.next();
        const auto keyed = pulp::signal::velvet_noise_draw<double>(kVelvetSeed, index);
        CHECK(sequential.jitter == first[index].jitter);
        CHECK(sequential.sign == first[index].sign);
        CHECK(sequential.jitter == keyed.jitter);
        CHECK(sequential.sign == keyed.sign);
        jitter_sum += sequential.jitter;
        positive += sequential.sign > 0;
        negative += sequential.sign < 0;
    }
    const double jitter_mean = static_cast<double>(jitter_sum / first.size());
    INFO("jitter mean=" << jitter_mean << " positive=" << positive << " negative=" << negative);
    CHECK(jitter_mean == Catch::Approx(0.5).margin(0.025));
    CHECK(positive > 900);
    CHECK(negative > 900);
}
