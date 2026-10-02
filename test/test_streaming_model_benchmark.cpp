#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "detail/streaming_model.hpp"
#include "harness/scoped_rt_process_probe.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

using namespace pulp::gpu_audio::detail;

struct HostCase {
    std::uint32_t sample_rate;
    std::uint32_t frames;
};

// Provider-neutral scalar oracle: the same causal depthwise convolution and
// ReLU as MicroTcnModel, expressed independently so the receipt can detect
// numerical drift in a future provider adapter.
template <std::size_t Channels, std::size_t KernelSize>
class CpuOracle {
  public:
    using Weights = MicroTcnWeights<Channels, KernelSize>;

    explicit CpuOracle(const Weights& weights) : weights_(weights) {}

    void reset() noexcept {
        for (auto& channel : state_)
            channel.fill(0.0f);
        cursor_ = 0;
    }

    void process(const pulp::audio::BufferView<const float>& input,
                 pulp::audio::BufferView<float>& output, std::uint32_t frames) noexcept {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            for (std::size_t channel = 0; channel < Channels; ++channel) {
                float value = weights_.bias[channel];
                const auto sample = input.channel_ptr(channel)[frame];
                value += weights_.taps[channel][0] * sample;
                for (std::size_t tap = 1; tap < KernelSize; ++tap) {
                    const auto index = (cursor_ + KernelSize - tap) % KernelSize;
                    value += weights_.taps[channel][tap] * state_[channel][index];
                }
                output.channel_ptr(channel)[frame] = value > 0.0f ? value : 0.0f;
                state_[channel][cursor_] = sample;
            }
            cursor_ = (cursor_ + 1) % KernelSize;
        }
    }

  private:
    Weights weights_;
    std::array<std::array<float, KernelSize>, Channels> state_{};
    std::size_t cursor_ = 0;
};

template <std::size_t Channels, std::size_t KernelSize>
void run_cpu_receipt(const HostCase host) {
    using Model = MicroTcnModel<Channels, KernelSize>;
    typename Model::Weights weights;
    for (std::size_t channel = 0; channel < Channels; ++channel) {
        weights.bias[channel] = 0.01f * static_cast<float>(channel + 1);
        for (std::size_t tap = 0; tap < KernelSize; ++tap)
            weights.taps[channel][tap] = 0.05f / static_cast<float>(tap + 1);
    }

    Model model(weights);
    CpuOracle<Channels, KernelSize> oracle(weights);
    const auto context = StreamingPrepareContext{.spec = &model.spec(),
                                                 .artifact_id = "benchmark.micro-tcn",
                                                 .artifact_hash = "embedded-benchmark-weights",
                                                 .fallback = StreamingFallbackStrategy::NoFallback,
                                                 .max_frames = 128};
    REQUIRE(model.prepare(context));

    constexpr std::size_t blocks = 512;
    // The largest host buffer is 128 frames. All backing storage is owned before
    // the probe, so callback execution cannot grow it or take a heap lock.
    constexpr std::size_t storage_frames = 128;
    REQUIRE(host.frames <= storage_frames);
    std::array<std::array<float, storage_frames>, Channels> input{};
    std::array<std::array<float, storage_frames>, Channels> output{};
    std::array<std::array<float, storage_frames>, Channels> oracle_output{};
    std::array<const float*, Channels> input_channels{};
    std::array<float*, Channels> output_channels{};
    std::array<float*, Channels> oracle_output_channels{};
    for (std::size_t channel = 0; channel < Channels; ++channel) {
        input_channels[channel] = input[channel].data();
        output_channels[channel] = output[channel].data();
        oracle_output_channels[channel] = oracle_output[channel].data();
        for (std::size_t frame = 0; frame < storage_frames; ++frame)
            input[channel][frame] = static_cast<float>((frame + channel * 3) % 17) / 17.0f;
    }

    const auto in = pulp::audio::BufferView<const float>(input_channels.data(), Channels,
                                                         host.frames);
    auto out = pulp::audio::BufferView<float>(output_channels.data(), Channels, host.frames);
    auto oracle_out =
        pulp::audio::BufferView<float>(oracle_output_channels.data(), Channels, host.frames);

    // Warm up outside the receipt. The probe traps allocations and blocking
    // pthread mutex/rwlock calls on UNIX; other platforms count allocations.
    model.process_cpu(in, out, host.frames, {.epoch = 1, .sequence = 0});
    oracle.process(in, oracle_out, host.frames);
    REQUIRE(model.quiesce());
    model.reset(1, StreamingResetReason::TransportRestart);
    oracle.reset();
    double checksum = 0.0;
    double oracle_checksum = 0.0;
    double parity_model_checksum = 0.0;
    float max_abs_error = 0.0f;
    std::size_t callback_allocations = 0;
    std::int64_t elapsed_us = 0;
    std::int64_t oracle_elapsed_us = 0;
    bool reset_ok = false;
    double oracle_timed_checksum = 0.0;
    {
        pulp::test::ScopedRtProcessProbe probe;
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t block = 0; block < blocks; ++block) {
            model.process_cpu(in, out, host.frames,
                              {.epoch = 1, .sequence = static_cast<std::uint64_t>(block + 1)});
            for (const auto& channel : output)
                for (std::size_t frame = 0; frame < host.frames; ++frame)
                    checksum += static_cast<double>(channel[frame]);
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
        // Rewind both state machines before the parity pass. The timed model
        // pass above is kept separate from parity and oracle timing.
        reset_ok = model.quiesce();
        if (reset_ok)
            model.reset(1, StreamingResetReason::TransportRestart);
        oracle.reset();
        for (std::size_t block = 0; block < blocks; ++block) {
            model.process_cpu(in, out, host.frames,
                              {.epoch = 1, .sequence = static_cast<std::uint64_t>(block + 1)});
            oracle.process(in, oracle_out, host.frames);
            for (std::size_t channel = 0; channel < Channels; ++channel)
                for (std::size_t frame = 0; frame < host.frames; ++frame) {
                    parity_model_checksum += static_cast<double>(output[channel][frame]);
                    oracle_checksum += static_cast<double>(oracle_output[channel][frame]);
                    max_abs_error = std::max(
                        max_abs_error,
                        std::abs(output[channel][frame] - oracle_output[channel][frame]));
                }
        }
        const auto oracle_started = std::chrono::steady_clock::now();
        oracle.reset();
        for (std::size_t block = 0; block < blocks; ++block) {
            oracle.process(in, oracle_out, host.frames);
            for (const auto& channel : oracle_output)
                for (std::size_t frame = 0; frame < host.frames; ++frame)
                    oracle_timed_checksum += static_cast<double>(channel[frame]);
        }
        const auto oracle_elapsed = std::chrono::steady_clock::now() - oracle_started;
        oracle_elapsed_us =
            std::chrono::duration_cast<std::chrono::microseconds>(oracle_elapsed).count();
        callback_allocations = probe.allocation_count();
    }

    REQUIRE(callback_allocations == 0);
    REQUIRE(reset_ok);
    REQUIRE(max_abs_error <= 1.0e-6f);
    REQUIRE(parity_model_checksum == Catch::Approx(oracle_checksum).margin(1.0e-4));
    REQUIRE(checksum == Catch::Approx(oracle_checksum).margin(1.0e-4));
    REQUIRE(oracle_timed_checksum == Catch::Approx(oracle_checksum).margin(1.0e-4));
    REQUIRE(std::isfinite(checksum));
    REQUIRE(model.last_stamp() ==
            StreamingBlockStamp{.epoch = 1, .sequence = static_cast<std::uint64_t>(blocks)});
    INFO("streaming receipt channels=" << Channels << " kernel=" << KernelSize
                                        << " host_sample_rate_metadata=" << host.sample_rate
                                        << " blocks=" << blocks << " frames=" << host.frames
                                        << " model_elapsed_us=" << elapsed_us
                                        << " oracle_elapsed_us=" << oracle_elapsed_us
                                        << " max_abs_error=" << max_abs_error
                                        << " model_checksum=" << checksum
                                        << " oracle_checksum=" << oracle_checksum);
    std::cout << "streaming-receipt channels=" << Channels << " kernel=" << KernelSize
              << " host_sample_rate_metadata=" << host.sample_rate << " blocks=" << blocks
              << " frames=" << host.frames << " model_elapsed_us=" << elapsed_us
              << " oracle_elapsed_us=" << oracle_elapsed_us
              << " max_abs_error=" << max_abs_error << " model_checksum=" << checksum
              << " oracle_checksum=" << oracle_checksum
              << " allocations=" << callback_allocations << " parity=pass\n";
}

} // namespace

TEST_CASE("streaming CPU matrix is callback-safe and allocation-free",
          "[gpu_audio][streaming_model][benchmark][rt_safety]") {
    constexpr HostCase host_matrix[] = {{44100, 32}, {48000, 64}, {96000, 128}};
    for (const auto host : host_matrix) {
        run_cpu_receipt<1, 3>(host);
        run_cpu_receipt<2, 5>(host);
        run_cpu_receipt<4, 7>(host);
    }
}
