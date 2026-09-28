#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <pulp/audio/buffer.hpp>
#include <pulp/gpu_audio/gpu_convolution_reverb.hpp>
#include <utility>
#include <vector>

namespace {

using pulp::audio::BufferView;
using pulp::gpu_audio::GpuConvolutionReverb;
using pulp::gpu_audio::GpuConvolutionReverbConfig;

GpuConvolutionReverbConfig valid_config() {
    GpuConvolutionReverbConfig config;
    config.block_size = 64;
    config.sample_rate = 48000;
    config.impulse_response_sample_rate = 48000.0;
    config.impulse_response = {{1.0f, 0.25f}, {0.75f, -0.5f}};
    config.gpu_enabled = true;
    config.ring_blocks = 8;
    return config;
}

TEST_CASE("GPU convolution route fails closed before provider preparation",
          "[gpu_audio][convolution][configuration]") {
    auto config = valid_config();

    config.gpu_enabled = false;
    GpuConvolutionReverb disabled(config);
    REQUIRE_FALSE(disabled.prepare());
    REQUIRE_FALSE(disabled.prepared());
    const auto disabled_report = disabled.report();
    CHECK_FALSE(disabled_report.prepared);
    CHECK_FALSE(disabled_report.gpu_enabled);
    CHECK_FALSE(disabled_report.authenticated_shared_provider);
    CHECK(disabled_report.latency_samples == 0);

    // The internal FFT quantum is rounded once at construction. This is a
    // host-capacity contract and must remain testable without a Dawn device.
    config = valid_config();
    config.block_size = 192;
    config.gpu_enabled = false;
    GpuConvolutionReverb rounded(config);
    CHECK(rounded.block_size() == 256);
    CHECK_FALSE(rounded.prepare());
}

TEST_CASE("GPU convolution route rejects malformed CPU configuration",
          "[gpu_audio][convolution][configuration]") {
    const auto expect_rejected = [](GpuConvolutionReverbConfig config) {
        GpuConvolutionReverb route(std::move(config));
        CHECK_FALSE(route.prepare());
        CHECK_FALSE(route.prepared());
        CHECK_FALSE(route.report().authenticated_shared_provider);
    };

    auto config = valid_config();
    config.sample_rate = 0;
    expect_rejected(config);

    config = valid_config();
    config.impulse_response_sample_rate = std::numeric_limits<double>::quiet_NaN();
    expect_rejected(config);

    config = valid_config();
    config.ring_blocks = 3;
    expect_rejected(config);

    config = valid_config();
    config.impulse_response.clear();
    expect_rejected(config);

    config = valid_config();
    config.impulse_response = {{}};
    expect_rejected(config);

    config = valid_config();
    config.impulse_response = {{1.0f}, {2.0f, 3.0f}};
    expect_rejected(config);

    config = valid_config();
    config.impulse_response[1][0] = std::numeric_limits<float>::infinity();
    expect_rejected(config);
}

TEST_CASE("GPU convolution route clears output while unprepared and releases idempotently",
          "[gpu_audio][convolution][lifecycle]") {
    GpuConvolutionReverb route(valid_config());
    std::vector<float> left(8, 1.0f), right(8, 2.0f), out_left(8, 3.0f), out_right(8, 4.0f);
    const float* inputs[] = {left.data(), right.data()};
    float* outputs[] = {out_left.data(), out_right.data()};
    const BufferView<const float> input(inputs, 2, left.size());
    BufferView<float> output(outputs, 2, out_left.size());

    route.process(input, output, static_cast<std::uint32_t>(left.size()));
    for (const auto sample : out_left)
        CHECK(sample == 0.0f);
    for (const auto sample : out_right)
        CHECK(sample == 0.0f);

    route.release();
    route.release();
    CHECK_FALSE(route.prepared());
    CHECK(route.latency_samples() == 0);
}

} // namespace
