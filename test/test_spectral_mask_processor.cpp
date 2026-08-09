#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/spectral_mask_processor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::SpectralBandEdgePolicy;
using pulp::signal::SpectralBandLayout;
using pulp::signal::SpectralBandSpacing;
using pulp::signal::SpectralMaskBoundaryKernel;
using pulp::signal::SpectralMaskProcessor;
using pulp::signal::SpectralMaskProcessorConfig;
using pulp::signal::SpectralMaskTable;

extern std::atomic<long> g_alloc_count;

namespace {

constexpr float kSampleRate = 48000.0f;
constexpr int kFftSize = 1024;
constexpr int kHopSize = 256;
constexpr int kLatency = kFftSize + kHopSize;
constexpr double kPi = 3.14159265358979323846264338327950288;

SpectralMaskProcessorConfig config(int channels = 1) {
    SpectralMaskProcessorConfig result;
    result.frame.fft_size = kFftSize;
    result.frame.analysis_hop = kHopSize;
    result.frame.channels = channels;
    result.frame.max_block = 512;
    result.frame.window = pulp::signal::WindowFunction::Type::hann;
    result.sample_rate = kSampleRate;
    result.initial_mix = 1.0f;
    result.mix_ramp_samples = 0;
    return result;
}

SpectralBandLayout layout(std::uint32_t bands = 8) {
    SpectralBandLayout result;
    result.active_bands = bands;
    result.min_hz = 20.0f;
    result.max_hz = 20000.0f;
    result.spacing = SpectralBandSpacing::logarithmic;
    result.edge_policy = SpectralBandEdgePolicy::extend_edge_band;
    result.boundary_kernel = SpectralMaskBoundaryKernel::hard;
    result.transition_fraction = 0.0f;
    result.transition_frames = 0;
    return result;
}

std::vector<float> signal(std::size_t count) {
    std::vector<float> result(count);
    for (std::size_t i = 0; i < count; ++i) {
        result[i] = static_cast<float>(
            0.2 * std::sin(2.0 * kPi * 997.0 * static_cast<double>(i)
                           / kSampleRate)
            + 0.1 * std::cos(2.0 * kPi * 3125.0 * static_cast<double>(i)
                             / kSampleRate));
    }
    return result;
}

std::vector<float> render(SpectralMaskProcessor& processor,
                          const std::vector<float>& input,
                          const std::vector<int>& partitions) {
    std::vector<float> output(input.size());
    std::size_t position = 0;
    std::size_t partition = 0;
    while (position < input.size()) {
        const int requested = partitions[partition++ % partitions.size()];
        const int count = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(requested), input.size() - position));
        const float* in[] = {input.data() + position};
        float* out[] = {output.data() + position};
        REQUIRE(processor.process(in, out, count));
        position += static_cast<std::size_t>(count);
    }
    return output;
}

float maximum_error(const std::vector<float>& a,
                    const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    float result = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i)
        result = std::max(result, std::abs(a[i] - b[i]));
    return result;
}

} // namespace

TEST_CASE("SpectralMaskProcessor identity reconstructs at exact fixed latency",
          "[signal][spectral-mask-processor][latency]") {
    SpectralMaskProcessor processor;
    REQUIRE(processor.prepare(config()));
    REQUIRE(processor.latency_samples() == kLatency);
    REQUIRE(processor.maximum_tail_samples() == kLatency + kFftSize);
    REQUIRE(processor.num_bins() == kFftSize / 2 + 1);
    REQUIRE(processor.channels() == 1);
    REQUIRE(processor.retained_bytes() > 0);

    const auto input = signal(8192);
    const auto output = render(processor, input, {512});
    for (int i = 0; i < kLatency; ++i)
        REQUIRE(output[static_cast<std::size_t>(i)] == 0.0f);
    for (std::size_t i = kLatency + kFftSize; i < output.size(); ++i) {
        REQUIRE_THAT(output[i], WithinAbs(input[i - kLatency], 2.0e-5f));
    }
}

TEST_CASE("SpectralMaskProcessor exact mute and nonadjacent frame islands",
          "[signal][spectral-mask-processor][isolation]") {
    SpectralMaskProcessor processor;
    REQUIRE(processor.prepare(config()));
    auto mute = layout();
    for (auto& band : mute.bands) band.muted = true;
    mute.version = 17;
    REQUIRE(processor.publish_layout(mute));
    REQUIRE(processor.table_publication_pending());

    const auto output = render(processor, signal(4096), {64, 7, 31});
    REQUIRE(std::all_of(output.begin(), output.end(),
                        [](float value) { return value == 0.0f; }));
    REQUIRE_FALSE(processor.table_publication_pending());
    REQUIRE(processor.active_table_version() == 17);

    auto islands = layout(8);
    for (auto& band : islands.bands) band.muted = true;
    islands.bands[1].muted = false;
    islands.bands[6].muted = false;
    islands.version = 18;
    REQUIRE(processor.publish_layout(islands));

    std::array<std::complex<float>, kFftSize / 2 + 1> frame{};
    std::fill(frame.begin(), frame.end(), std::complex<float>{1.0f, -0.5f});
    std::complex<float>* frames[] = {frame.data()};
    REQUIRE(processor.process_frame(frames, static_cast<int>(frame.size())));
    REQUIRE(frame[0] == std::complex<float>{});
    REQUIRE(frame[2] != std::complex<float>{});
    REQUIRE(frame[20] == std::complex<float>{});
    REQUIRE(frame[100] != std::complex<float>{});
    REQUIRE(frame.back() == std::complex<float>{});
}

TEST_CASE("SpectralMaskProcessor transitions exactly over requested frames",
          "[signal][spectral-mask-processor][transition]") {
    SpectralMaskProcessor processor;
    REQUIRE(processor.prepare(config(2)));
    auto muted = layout(1);
    muted.bands[0].muted = true;
    muted.transition_frames = 4;
    muted.version = 44;
    REQUIRE(processor.publish_layout(muted));

    for (int step = 1; step <= 4; ++step) {
        std::array<std::complex<float>, kFftSize / 2 + 1> left{};
        std::array<std::complex<float>, kFftSize / 2 + 1> right{};
        std::fill(left.begin(), left.end(), std::complex<float>{1.0f, 2.0f});
        std::fill(right.begin(), right.end(), std::complex<float>{-3.0f, 4.0f});
        std::complex<float>* frames[] = {left.data(), right.data()};
        REQUIRE(processor.process_frame(frames, static_cast<int>(left.size())));
        const float expected = 1.0f - static_cast<float>(step) / 4.0f;
        REQUIRE_THAT(left[100].real(), WithinAbs(expected, 1.0e-6f));
        REQUIRE_THAT(left[100].imag(), WithinAbs(2.0f * expected, 1.0e-6f));
        REQUIRE_THAT(right[100].real(), WithinAbs(-3.0f * expected, 1.0e-6f));
        REQUIRE_THAT(right[100].imag(), WithinAbs(4.0f * expected, 1.0e-6f));
    }
    REQUIRE(processor.active_table_version() == 44);
}

TEST_CASE("SpectralMaskProcessor latest publication replaces an in-flight target",
          "[signal][spectral-mask-processor][publication]") {
    SpectralMaskProcessor processor;
    REQUIRE(processor.prepare(config()));
    auto first = layout(1);
    first.bands[0].gain_db = -6.0f;
    first.transition_frames = 8;
    first.version = 1;
    auto latest = layout(1);
    latest.bands[0].gain_db = 6.0f;
    latest.transition_frames = 0;
    latest.version = 2;
    REQUIRE(processor.publish_layout(first));
    REQUIRE(processor.publish_layout(latest));

    std::array<std::complex<float>, kFftSize / 2 + 1> frame{};
    std::fill(frame.begin(), frame.end(), std::complex<float>{1.0f, 0.0f});
    std::complex<float>* frames[] = {frame.data()};
    REQUIRE(processor.process_frame(frames, static_cast<int>(frame.size())));
    REQUIRE_THAT(frame[100].real(), WithinAbs(std::pow(10.0f, 0.3f), 1.0e-6f));
    REQUIRE(processor.active_table_version() == 2);
}

TEST_CASE("SpectralMaskProcessor is invariant to host block partitioning",
          "[signal][spectral-mask-processor][block]") {
    SpectralMaskProcessor a;
    SpectralMaskProcessor b;
    REQUIRE(a.prepare(config()));
    REQUIRE(b.prepare(config()));
    auto shaped = layout(32);
    for (std::uint32_t band = 0; band < shaped.active_bands; ++band) {
        shaped.bands[band].gain_db = static_cast<float>(band % 7) - 3.0f;
        shaped.bands[band].muted = (band % 5) == 0;
    }
    shaped.transition_frames = 6;
    REQUIRE(a.publish_layout(shaped));
    REQUIRE(b.publish_layout(shaped));
    const auto input = signal(16384);
    const auto uniform = render(a, input, {512});
    const auto irregular = render(b, input, {1, 7, 13, 31, 64, 127, 269});
    REQUIRE(maximum_error(uniform, irregular) <= 2.0e-5f);
}

TEST_CASE("SpectralMaskProcessor latency-aligns the dry path",
          "[signal][spectral-mask-processor][mix]") {
    auto cfg = config();
    cfg.initial_mix = 0.0f;
    SpectralMaskProcessor processor;
    REQUIRE(processor.prepare(cfg));
    auto muted = layout(1);
    muted.bands[0].muted = true;
    REQUIRE(processor.publish_layout(muted));

    const auto input = signal(4096);
    const auto output = render(processor, input, {37, 211, 512});
    for (int i = 0; i < kLatency; ++i)
        REQUIRE(output[static_cast<std::size_t>(i)] == 0.0f);
    for (std::size_t i = kLatency; i < output.size(); ++i)
        REQUIRE(output[i] == input[i - kLatency]);
}

TEST_CASE("SpectralMaskProcessor publication and process are allocation-free",
          "[signal][spectral-mask-processor][rt]") {
    SpectralMaskProcessor processor;
    REQUIRE(processor.prepare(config()));
    auto shaped = layout(64);
    shaped.bands[3].muted = true;
    SpectralMaskTable table;
    REQUIRE(pulp::signal::build_spectral_mask(
        shaped, kFftSize, kSampleRate, table));
    std::array<float, 64> input{};
    std::array<float, 64> output{};
    const float* in[] = {input.data()};
    float* out[] = {output.data()};

    const auto before = g_alloc_count.load();
    REQUIRE(processor.publish_table(table));
    REQUIRE(processor.process(in, out, static_cast<int>(input.size())));
    REQUIRE(g_alloc_count.load() == before);
}

TEST_CASE("SpectralMaskProcessor rejects bad preparation and tables atomically",
          "[signal][spectral-mask-processor][fault]") {
    SpectralMaskProcessor processor;
    REQUIRE(processor.prepare(config()));
    const auto bytes = processor.retained_bytes();
    auto tiny = config();
    tiny.max_retained_bytes = bytes - 1;
    REQUIRE_FALSE(processor.prepare(tiny));
    REQUIRE(processor.retained_bytes() == bytes);
    auto bad_curve = config();
    bad_curve.mix_curve = static_cast<pulp::signal::MixCurve>(255);
    REQUIRE_FALSE(processor.prepare(bad_curve));
    REQUIRE(processor.retained_bytes() == bytes);

    auto valid = layout(1);
    valid.version = 9;
    REQUIRE(processor.publish_layout(valid));
    std::array<std::complex<float>, kFftSize / 2 + 1> frame{};
    std::fill(frame.begin(), frame.end(), std::complex<float>{1.0f, 0.0f});
    std::complex<float>* frames[] = {frame.data()};
    REQUIRE(processor.process_frame(frames, static_cast<int>(frame.size())));
    REQUIRE(processor.active_table_version() == 9);

    SpectralMaskTable malformed;
    REQUIRE(pulp::signal::build_spectral_mask(
        valid, kFftSize, kSampleRate, malformed));
    malformed.gain_linear[4] = -1.0f;
    malformed.version = 10;
    REQUIRE_FALSE(processor.publish_table(malformed));
    REQUIRE(processor.active_table_version() == 9);

    std::array<std::complex<float>, kFftSize / 2 + 2> wrong_geometry{};
    std::fill(wrong_geometry.begin(), wrong_geometry.end(),
              std::complex<float>{1.0f, -1.0f});
    std::complex<float>* wrong_frames[] = {wrong_geometry.data()};
    REQUIRE_FALSE(processor.process_frame(
        wrong_frames, static_cast<int>(wrong_geometry.size())));
    REQUIRE(std::all_of(wrong_geometry.begin(), wrong_geometry.end(),
                        [](const auto value) {
                            return value == std::complex<float>{};
                        }));
}

TEST_CASE("SpectralMaskProcessor reset makes repeated renders deterministic",
          "[signal][spectral-mask-processor][reset]") {
    SpectralMaskProcessor processor;
    REQUIRE(processor.prepare(config()));
    auto shaped = layout(4);
    shaped.bands[1].muted = true;
    REQUIRE(processor.publish_layout(shaped));
    const auto input = signal(8192);
    const auto first = render(processor, input, {64});
    processor.reset();
    const auto second = render(processor, input, {64});
    REQUIRE(first == second);
}

TEST_CASE("SpectralMaskProcessor reset settles an in-flight target",
          "[signal][spectral-mask-processor][reset]") {
    SpectralMaskProcessor processor;
    REQUIRE(processor.prepare(config()));
    auto muted = layout(1);
    muted.bands[0].muted = true;
    muted.transition_frames = 8;
    REQUIRE(processor.publish_layout(muted));

    std::array<std::complex<float>, kFftSize / 2 + 1> first{};
    std::fill(first.begin(), first.end(), std::complex<float>{1.0f, 0.0f});
    std::complex<float>* first_frames[] = {first.data()};
    REQUIRE(processor.process_frame(first_frames, static_cast<int>(first.size())));
    REQUIRE_THAT(first[100].real(), WithinAbs(0.875f, 1.0e-6f));

    processor.reset();
    std::array<std::complex<float>, kFftSize / 2 + 1> settled{};
    std::fill(settled.begin(), settled.end(), std::complex<float>{1.0f, 0.0f});
    std::complex<float>* settled_frames[] = {settled.data()};
    REQUIRE(processor.process_frame(settled_frames,
                                    static_cast<int>(settled.size())));
    REQUIRE(settled[100] == std::complex<float>{});
}

TEST_CASE("SpectralMaskProcessor publishes concurrently without torn tables",
          "[signal][spectral-mask-processor][publication][thread]") {
    SpectralMaskProcessor processor;
    REQUIRE(processor.prepare(config()));
    auto low = layout(1);
    low.bands[0].gain_db = -12.0f;
    low.transition_frames = 0;
    auto high = layout(1);
    high.bands[0].gain_db = 6.0f;
    high.transition_frames = 0;

    std::atomic<bool> start{false};
    std::atomic<bool> publish_ok{true};
    std::thread publisher([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (std::uint64_t version = 1; version <= 2000; ++version) {
            auto& next = (version & 1u) == 0u ? high : low;
            next.version = version;
            if (!processor.publish_layout(next)) {
                publish_ok.store(false, std::memory_order_release);
                return;
            }
        }
    });

    start.store(true, std::memory_order_release);
    const float low_gain = std::pow(10.0f, -12.0f / 20.0f);
    const float high_gain = std::pow(10.0f, 6.0f / 20.0f);
    bool frames_ok = true;
    bool torn = false;
    for (int iteration = 0; iteration < 2000; ++iteration) {
        std::array<std::complex<float>, kFftSize / 2 + 1> frame{};
        std::fill(frame.begin(), frame.end(), std::complex<float>{1.0f, -1.0f});
        std::complex<float>* frames[] = {frame.data()};
        if (!processor.process_frame(frames, static_cast<int>(frame.size()))) {
            frames_ok = false;
            break;
        }
        const auto gain = frame[100].real();
        const auto identity = std::abs(gain - 1.0f) <= 1.0e-6f;
        const auto is_low = std::abs(gain - low_gain) <= 1.0e-6f;
        const auto is_high = std::abs(gain - high_gain) <= 1.0e-6f;
        if (!(identity || is_low || is_high)
            || std::abs(frame[100].imag() + gain) > 1.0e-6f) {
            torn = true;
            break;
        }
    }
    publisher.join();
    REQUIRE(publish_ok.load(std::memory_order_acquire));
    REQUIRE(frames_ok);
    REQUIRE_FALSE(torn);
}
