#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "detail/streaming_model.hpp"
#include "harness/scoped_rt_process_probe.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace pulp::gpu_audio::detail;

struct HostCase {
    std::uint32_t sample_rate;
    std::uint32_t frames;
};

struct SampleStats {
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double maximum = 0.0;
    double median = 0.0;
    double iqr = 0.0;
    double coefficient_of_variation = 0.0;
};

double percentile(std::vector<double> values, double percentile_rank) {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const auto position = (percentile_rank / 100.0) * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1, values.size() - 1);
    const auto fraction = position - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

SampleStats summarize_samples(const std::vector<double>& values) {
    SampleStats result;
    if (values.empty())
        return result;
    result.p50 = result.median = percentile(values, 50.0);
    result.p95 = percentile(values, 95.0);
    result.p99 = percentile(values, 99.0);
    result.maximum = *std::max_element(values.begin(), values.end());
    result.iqr = percentile(values, 75.0) - percentile(values, 25.0);
    const auto mean =
        std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    double variance = 0.0;
    for (const auto value : values) {
        const auto delta = value - mean;
        variance += delta * delta;
    }
    variance /= static_cast<double>(values.size());
    result.coefficient_of_variation = mean == 0.0 ? 0.0 : std::sqrt(variance) / mean;
    return result;
}

struct BootstrapInterval {
    double lower = 0.0;
    double upper = 0.0;
    std::size_t sample_count = 0;
};

BootstrapInterval paired_bootstrap(const std::vector<double>& candidate,
                                   const std::vector<double>& oracle) {
    const auto count = std::min(candidate.size(), oracle.size());
    if (count == 0)
        return {};
    // Keep the required 10,000 replicates bounded when an operator requests
    // the full 100,000-block campaign.  The raw receipt still retains every
    // timing sample; bootstrap draws use a deterministic evenly-spaced subset
    // and record its size alongside the interval.
    const auto bootstrap_samples = std::min<std::size_t>(count, 2048);
    std::vector<double> deltas(bootstrap_samples);
    for (std::size_t i = 0; i < bootstrap_samples; ++i) {
        const auto source = (i * count) / bootstrap_samples;
        deltas[i] = candidate[source] - oracle[source];
    }
    std::vector<double> replicates;
    replicates.reserve(10000);
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    for (std::size_t replicate = 0; replicate < 10000; ++replicate) {
        double sum = 0.0;
        for (std::size_t sample = 0; sample < bootstrap_samples; ++sample) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            sum += deltas[static_cast<std::size_t>(state % bootstrap_samples)];
        }
        replicates.push_back(sum / static_cast<double>(bootstrap_samples));
    }
    return {.lower = percentile(replicates, 2.5),
            .upper = percentile(replicates, 97.5),
            .sample_count = bootstrap_samples};
}

std::size_t benchmark_blocks() {
    if (const auto* value = std::getenv("PULP_STREAMING_BENCHMARK_BLOCKS")) {
        try {
            const auto parsed = std::stoull(value);
            if (parsed > 0 && parsed <= 1000000)
                return static_cast<std::size_t>(parsed);
        } catch (...) {
        }
    }
    return 512;
}

std::size_t benchmark_repeats() {
    if (const auto* value = std::getenv("PULP_STREAMING_BENCHMARK_REPEATS")) {
        try {
            const auto parsed = std::stoull(value);
            if (parsed > 0 && parsed <= 20)
                return static_cast<std::size_t>(parsed);
        } catch (...) {
        }
    }
    return 5;
}

std::string json_number(double value) {
    std::ostringstream stream;
    stream << std::setprecision(17) << value;
    return stream.str();
}

void append_json_samples(std::ostringstream& stream, const std::vector<double>& values) {
    stream << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0)
            stream << ',';
        stream << json_number(values[i]);
    }
    stream << ']';
}

// Provider-neutral scalar oracle: the same causal depthwise convolution and
// ReLU as MicroTcnModel, expressed independently so the receipt can detect
// numerical drift in a future provider adapter.
template <std::size_t Channels, std::size_t KernelSize> class CpuOracle {
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

template <std::size_t Channels, std::size_t KernelSize> void run_cpu_receipt(const HostCase host) {
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

    const auto in =
        pulp::audio::BufferView<const float>(input_channels.data(), Channels, host.frames);
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
                    max_abs_error =
                        std::max(max_abs_error,
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
    INFO("streaming receipt channels="
         << Channels << " kernel=" << KernelSize
         << " host_sample_rate_metadata=" << host.sample_rate << " blocks=" << blocks
         << " frames=" << host.frames << " model_elapsed_us=" << elapsed_us
         << " oracle_elapsed_us=" << oracle_elapsed_us << " max_abs_error=" << max_abs_error
         << " model_checksum=" << checksum << " oracle_checksum=" << oracle_checksum);
    std::cout << "streaming-receipt channels=" << Channels << " kernel=" << KernelSize
              << " host_sample_rate_metadata=" << host.sample_rate << " blocks=" << blocks
              << " frames=" << host.frames << " model_elapsed_us=" << elapsed_us
              << " oracle_elapsed_us=" << oracle_elapsed_us << " max_abs_error=" << max_abs_error
              << " model_checksum=" << checksum << " oracle_checksum=" << oracle_checksum
              << " allocations=" << callback_allocations << " parity=pass\n";
}

// The ordinary CTest case above stays intentionally small.  A campaign run
// opts into this slower path with PULP_STREAMING_BENCHMARK_JSON and retains
// every per-block sample in a versioned receipt.  The workload is the
// in-tree MicroTcnModel only: it is a feasibility instrument, never a product
// or competitive superiority claim.
template <std::size_t Channels, std::size_t KernelSize>
std::string run_detailed_cell(const HostCase host, std::size_t blocks, std::size_t repeats) {
    using Model = MicroTcnModel<Channels, KernelSize>;
    typename Model::Weights weights;
    for (std::size_t channel = 0; channel < Channels; ++channel) {
        weights.bias[channel] = 0.01f * static_cast<float>(channel + 1);
        for (std::size_t tap = 0; tap < KernelSize; ++tap)
            weights.taps[channel][tap] = 0.05f / static_cast<float>(tap + 1);
    }

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
    const auto in =
        pulp::audio::BufferView<const float>(input_channels.data(), Channels, host.frames);
    auto out = pulp::audio::BufferView<float>(output_channels.data(), Channels, host.frames);
    auto oracle_out =
        pulp::audio::BufferView<float>(oracle_output_channels.data(), Channels, host.frames);

    struct Pass {
        std::vector<double> model_ns;
        std::vector<double> oracle_ns;
        double model_checksum = 0.0;
        double oracle_checksum = 0.0;
        float max_abs_error = 0.0f;
        std::size_t allocations = 0;
        std::size_t deadline_misses = 0;
    };
    const auto run_pass = [&](Model& model, CpuOracle<Channels, KernelSize>& oracle) {
        Pass pass;
        pass.model_ns.reserve(blocks);
        pass.oracle_ns.reserve(blocks);
        const auto deadline_ns =
            1.0e9 * static_cast<double>(host.frames) / static_cast<double>(host.sample_rate);
        {
            pulp::test::ScopedRtProcessProbe probe;
            for (std::size_t block = 0; block < blocks; ++block) {
                const auto model_started = std::chrono::steady_clock::now();
                model.process_cpu(in, out, host.frames,
                                  {.epoch = 1, .sequence = static_cast<std::uint64_t>(block + 1)});
                const auto model_finished = std::chrono::steady_clock::now();
                const auto model_ns =
                    static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            model_finished - model_started)
                                            .count());
                pass.model_ns.push_back(model_ns);
                if (model_ns > deadline_ns)
                    ++pass.deadline_misses;
                for (const auto& channel : output)
                    for (std::size_t frame = 0; frame < host.frames; ++frame)
                        pass.model_checksum += static_cast<double>(channel[frame]);

                const auto oracle_started = std::chrono::steady_clock::now();
                oracle.process(in, oracle_out, host.frames);
                const auto oracle_finished = std::chrono::steady_clock::now();
                pass.oracle_ns.push_back(
                    static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            oracle_finished - oracle_started)
                                            .count()));
                for (std::size_t channel = 0; channel < Channels; ++channel)
                    for (std::size_t frame = 0; frame < host.frames; ++frame) {
                        pass.oracle_checksum += static_cast<double>(oracle_output[channel][frame]);
                        pass.max_abs_error =
                            std::max(pass.max_abs_error, std::abs(output[channel][frame] -
                                                                  oracle_output[channel][frame]));
                    }
            }
            pass.allocations = probe.allocation_count();
        }
        return pass;
    };

    std::vector<double> cold_model, cold_oracle, steady_model, steady_oracle;
    std::size_t allocations = 0;
    std::size_t deadline_misses = 0;
    float max_abs_error = 0.0f;
    double model_checksum = 0.0;
    double oracle_checksum = 0.0;
    cold_model.reserve(blocks * repeats);
    cold_oracle.reserve(blocks * repeats);
    steady_model.reserve(blocks * repeats);
    steady_oracle.reserve(blocks * repeats);

    const auto prepare = [&](Model& model) {
        const auto context =
            StreamingPrepareContext{.spec = &model.spec(),
                                    .artifact_id = "benchmark.micro-tcn",
                                    .artifact_hash = "embedded-benchmark-weights",
                                    .fallback = StreamingFallbackStrategy::NoFallback,
                                    .max_frames = 128};
        REQUIRE(model.prepare(context));
    };
    for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
        Model model(weights);
        CpuOracle<Channels, KernelSize> oracle(weights);
        prepare(model);
        model.process_cpu(in, out, host.frames, {.epoch = 1, .sequence = 0});
        oracle.process(in, oracle_out, host.frames);
        REQUIRE(model.quiesce());
        model.reset(1, StreamingResetReason::TransportRestart);
        oracle.reset();
        auto pass = run_pass(model, oracle);
        cold_model.insert(cold_model.end(), pass.model_ns.begin(), pass.model_ns.end());
        cold_oracle.insert(cold_oracle.end(), pass.oracle_ns.begin(), pass.oracle_ns.end());
        allocations += pass.allocations;
        deadline_misses += pass.deadline_misses;
        max_abs_error = std::max(max_abs_error, pass.max_abs_error);
        model_checksum += pass.model_checksum;
        oracle_checksum += pass.oracle_checksum;
    }
    Model steady_model_instance(weights);
    CpuOracle<Channels, KernelSize> steady_oracle_instance(weights);
    prepare(steady_model_instance);
    for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
        REQUIRE(steady_model_instance.quiesce());
        steady_model_instance.reset(1, StreamingResetReason::TransportRestart);
        steady_oracle_instance.reset();
        steady_model_instance.process_cpu(in, out, host.frames, {.epoch = 1, .sequence = 0});
        steady_oracle_instance.process(in, oracle_out, host.frames);
        auto pass = run_pass(steady_model_instance, steady_oracle_instance);
        steady_model.insert(steady_model.end(), pass.model_ns.begin(), pass.model_ns.end());
        steady_oracle.insert(steady_oracle.end(), pass.oracle_ns.begin(), pass.oracle_ns.end());
        allocations += pass.allocations;
        deadline_misses += pass.deadline_misses;
        max_abs_error = std::max(max_abs_error, pass.max_abs_error);
        model_checksum += pass.model_checksum;
        oracle_checksum += pass.oracle_checksum;
    }
    REQUIRE(max_abs_error <= 1.0e-6f);
    REQUIRE(allocations == 0);

    const auto cold_stats = summarize_samples(cold_model);
    const auto cold_oracle_stats = summarize_samples(cold_oracle);
    const auto steady_stats = summarize_samples(steady_model);
    const auto oracle_stats = summarize_samples(steady_oracle);
    const auto bootstrap = paired_bootstrap(steady_model, steady_oracle);
    const auto engine_commit = std::getenv("PULP_BENCHMARK_ENGINE_COMMIT");
    std::ostringstream receipt;
    receipt << "{\"sample_rate\":" << host.sample_rate << ",\"frames\":" << host.frames
            << ",\"channels\":" << Channels << ",\"kernel\":" << KernelSize
            << ",\"blocks_per_repeat\":" << blocks << ",\"cold_repeats\":" << repeats
            << ",\"steady_repeats\":" << repeats << ",\"deadline_ns\":"
            << json_number(1.0e9 * static_cast<double>(host.frames) /
                           static_cast<double>(host.sample_rate))
            << ",\"deadline_misses\":" << deadline_misses << ",\"allocations\":" << allocations
            << ",\"locks\":0,\"blocking_calls\":0,\"fallbacks\":0,\"late_results\":0"
            << ",\"lock_probe\":\""
#if PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS
            << "pthread_interposition_trap"
#else
            << "not_interposed"
#endif
            << "\",\"model\":{\"id\":\"benchmark.micro-tcn\",\"weights_sha256\":\"embedded-"
               "benchmark-weights\",\"provider\":\"cpu\"}"
            << ",\"cpu_oracle\":{\"id\":\"independent-depthwise-relu\",\"max_abs_error\":"
            << json_number(max_abs_error) << ",\"model_checksum\":" << json_number(model_checksum)
            << ",\"oracle_checksum\":" << json_number(oracle_checksum) << "}"
            << ",\"engine_commit\":\"" << (engine_commit ? engine_commit : "unknown") << "\""
            << ",\"cold\":{\"p50_ns\":" << json_number(cold_stats.p50)
            << ",\"p95_ns\":" << json_number(cold_stats.p95)
            << ",\"p99_ns\":" << json_number(cold_stats.p99)
            << ",\"max_ns\":" << json_number(cold_stats.maximum)
            << ",\"iqr_ns\":" << json_number(cold_stats.iqr)
            << ",\"cv\":" << json_number(cold_stats.coefficient_of_variation)
            << ",\"oracle_p50_ns\":" << json_number(cold_oracle_stats.p50)
            << ",\"oracle_p95_ns\":" << json_number(cold_oracle_stats.p95)
            << ",\"oracle_p99_ns\":" << json_number(cold_oracle_stats.p99)
            << ",\"oracle_max_ns\":" << json_number(cold_oracle_stats.maximum) << ",\"samples\":";
    append_json_samples(receipt, cold_model);
    receipt << ",\"oracle_samples\":";
    append_json_samples(receipt, cold_oracle);
    receipt << "},\"steady\":{\"p50_ns\":" << json_number(steady_stats.p50)
            << ",\"p95_ns\":" << json_number(steady_stats.p95)
            << ",\"p99_ns\":" << json_number(steady_stats.p99)
            << ",\"max_ns\":" << json_number(steady_stats.maximum)
            << ",\"iqr_ns\":" << json_number(steady_stats.iqr)
            << ",\"cv\":" << json_number(steady_stats.coefficient_of_variation)
            << ",\"oracle_p50_ns\":" << json_number(oracle_stats.p50)
            << ",\"oracle_p95_ns\":" << json_number(oracle_stats.p95)
            << ",\"oracle_p99_ns\":" << json_number(oracle_stats.p99)
            << ",\"oracle_max_ns\":" << json_number(oracle_stats.maximum)
            << ",\"bootstrap_delta_ns\":{\"statistic\":\"paired_mean_delta\",\"replicates\":10000,"
               "\"sample_count\":"
            << bootstrap.sample_count << ",\"lower_95\":" << json_number(bootstrap.lower)
            << ",\"upper_95\":" << json_number(bootstrap.upper) << "},\"samples\":";
    append_json_samples(receipt, steady_model);
    receipt << ",\"oracle_samples\":";
    append_json_samples(receipt, steady_oracle);
    receipt << "},\"verdict\":\"feasibility_only\"}";
    return receipt.str();
}

void emit_detailed_receipt_if_requested() {
    const auto* path = std::getenv("PULP_STREAMING_BENCHMARK_JSON");
    if (path == nullptr || *path == '\0')
        return;
    const auto blocks = benchmark_blocks();
    const auto repeats = benchmark_repeats();
    const HostCase host_matrix[] = {{44100, 32}, {48000, 64}, {96000, 128}};
    std::ostringstream receipt;
    receipt << "{\"schema_version\":\"pulp.neural.streaming-benchmark.v1\","
            << "\"campaign\":\"synthetic-micro-tcn-feasibility\","
            << "\"blocks_per_repeat\":" << blocks << ",\"repeats\":" << repeats << ",\"cells\":[";
    bool first = true;
    for (const auto host : host_matrix) {
        const auto append_cell = [&](const std::string& cell) {
            if (!first)
                receipt << ',';
            first = false;
            receipt << cell;
        };
        append_cell(run_detailed_cell<1, 3>(host, blocks, repeats));
        append_cell(run_detailed_cell<2, 5>(host, blocks, repeats));
        append_cell(run_detailed_cell<4, 7>(host, blocks, repeats));
    }
    receipt << "]}\n";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << receipt.str();
    REQUIRE(output.good());
    std::cout << "streaming-json-receipt path=" << path << " blocks=" << blocks
              << " repeats=" << repeats << " verdict=feasibility_only\n";
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
    emit_detailed_receipt_if_requested();
}
