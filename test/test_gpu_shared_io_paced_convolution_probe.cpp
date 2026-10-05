#include "detail/gpu_convolver_trial_config.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "support/audio_test_signals.hpp"
#include "support/unique_temp_dir.hpp"

#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/runtime/trace.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using pulp::gpu_audio::GpuAudioTransport;
using pulp::gpu_audio::GpuConvolver;

struct Config {
    std::uint32_t frames = 32;
    std::uint32_t lead = 2;
    std::uint32_t blocks = 128;
    std::uint32_t warmup = 64;
    bool wake = false;
    bool corrupt_output = false;
    std::filesystem::path directory;
};

constexpr auto kPostCallbackDrainTimeout = std::chrono::seconds{2};
// Keep long-tail campaigns bounded while allowing a 100,000-block run at the
// smallest supported block size. The probe retains input, output, and per-block
// records in memory, so this is deliberately below the point where a routine
// campaign becomes a multi-hundred-megabyte allocation.
constexpr std::uint32_t kMaximumMeasuredBlocks = 100'000;

bool parse(int argc, char** argv, Config& config) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        const auto number = [&](std::string_view prefix, std::uint32_t& result) {
            if (!argument.starts_with(prefix))
                return false;
            const auto value = argument.substr(prefix.size());
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
        };
        if (number("--frames=", config.frames) || number("--lead=", config.lead) ||
            number("--blocks=", config.blocks) || number("--warmup=", config.warmup))
            continue;
        if (argument == "--wake-on-write")
            config.wake = true;
        else if (argument == "--negative-control")
            config.corrupt_output = true;
        else if (argument.starts_with("--output-dir="))
            config.directory = argument.substr(13);
        else
            return false;
    }
    return (config.frames == 32 || config.frames == 64 || config.frames == 128) &&
           (config.lead == 1 || config.lead == 2 || config.lead == 4 || config.lead == 8) &&
           config.blocks > 0 && config.blocks <= kMaximumMeasuredBlocks && config.warmup <= 4096;
}

std::uint64_t nanoseconds(Clock::duration value) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
}

struct Record {
    std::uint64_t scheduled = 0;
    std::uint64_t callback_begin = 0;
    std::uint64_t callback_end = 0;
    std::uint64_t miss_delta = 0;
    double max_error = 0;
    bool finite = true;
};

int run(Config config) {
    const auto run_identity = nanoseconds(Clock::now().time_since_epoch());
    constexpr std::uint32_t sample_rate = 48000;
    constexpr std::uint32_t channels = 2;
    constexpr std::size_t ir_frames = 257;
    const auto total_blocks = config.warmup + config.blocks + config.lead;
    const auto total_frames = static_cast<std::size_t>(total_blocks) * config.frames;
    auto input = pulp::test::audio::make_sine(channels, static_cast<int>(total_frames), 731.0f,
                                              sample_rate, 0.2f);
    input.channel(0)[0] += 0.5f;
    for (std::size_t i = 0; i < total_frames; ++i)
        input.channel(1)[i] *= -0.7f;
    input.channel(1)[7] -= 0.4f;
    std::vector<float> ir(ir_frames);
    for (std::size_t i = 0; i < ir.size(); ++i)
        ir[i] = static_cast<float>(0.05 * std::cos(0.031 * i) * std::exp(-0.009 * i));
    std::vector<std::vector<float>> output(channels, std::vector<float>(total_frames));
    std::vector<Record> records(total_blocks);

    GpuConvolver node(channels, config.frames, sample_rate, ir, config.lead);
    if (!node.set_provider_policy(GpuConvolver::ProviderPolicy::SharedRequired) ||
        !node.configure_trace({.enabled = true,
                               .capture_admissions = true,
                               .capture_callback_timing = false,
                               .success_stride = 1}) ||
        !node.prepare() || !pulp::gpu_audio::detail::realtime_gpu_node_path(&node).active()) {
        std::cout << "{\"schema\":\"pulp.gpu-audio-paced-convolution.v1\","
                     "\"status\":\"unavailable\",\"reason\":\"shared_provider_not_active\"}\n";
        return 2;
    }
    GpuAudioTransport transport;
    if (!transport.prepare(&node, {.ring_blocks = std::max(8u, config.lead + 2u),
                                   .run_worker_thread = true,
                                   .wake_on_write = config.wake}))
        return 2;

    // Only the transport worker owns provider progress. Pacing this thread
    // must never delay its completion service until the next audio block.
    const auto start = Clock::now() + std::chrono::milliseconds(10);
    std::uint64_t previous_misses = 0;
    for (std::uint32_t block = 0; block < total_blocks; ++block) {
        const auto scheduled =
            static_cast<std::uint64_t>(block) * config.frames * 1'000'000'000ull / sample_rate;
        std::this_thread::sleep_until(start + std::chrono::nanoseconds(scheduled));
        const auto offset = static_cast<std::size_t>(block) * config.frames;
        const float* inputs[channels] = {input.channel(0).data() + offset,
                                         input.channel(1).data() + offset};
        float* outputs[channels] = {output[0].data() + offset, output[1].data() + offset};
        pulp::audio::BufferView<const float> in(inputs, channels, config.frames);
        pulp::audio::BufferView<float> out(outputs, channels, config.frames);
        auto& record = records[block];
        record.scheduled = scheduled;
        record.callback_begin = nanoseconds(Clock::now() - start);
        transport.process(in, out, config.frames);
        record.callback_end = nanoseconds(Clock::now() - start);
        const auto misses = transport.stats().miss_blocks;
        record.miss_delta = misses - previous_misses;
        previous_misses = misses;
    }
    // Let the non-RT worker retire the callback backlog before taking the
    // receipt snapshot. release() also joins and performs a final drain, but
    // it resets the transport state afterward, so a pre-release snapshot is
    // otherwise liable to report only the first provider-slot completions.
    const auto settle_deadline = Clock::now() + kPostCallbackDrainTimeout;
    while (Clock::now() < settle_deadline && transport.stats().produced_blocks < total_blocks) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto transport_stats = transport.stats();
    const auto delivery_stats = transport.delivery_snapshot();
    transport.release();

    std::vector<pulp::gpu_audio::detail::SharedIoTraceRecord> trace_records;
    (void)pulp::gpu_audio::detail::drain_gpu_convolver_trial_records(node, trace_records);
    const auto trace_stats = trace_records.empty() ? pulp::gpu_audio::detail::SharedIoTraceRecord{}
                                                   : trace_records.front();
    std::uint64_t high_water_in_flight = 0;
    std::uint64_t retired_success = 0;
    std::uint64_t retired_failure = 0;
    std::uint64_t late_completions = 0;
    std::uint64_t authenticated_terminal_records = 0;
    std::uint64_t terminal_record_count = 0;
    std::unordered_set<std::uint64_t> terminal_sequences;
    for (const auto& record : trace_records) {
        if (record.kind == pulp::gpu_audio::detail::SharedIoTraceKind::Terminal)
            ++terminal_record_count;
        const bool authenticated =
            record.valid() && record.kind == pulp::gpu_audio::detail::SharedIoTraceKind::Terminal &&
            record.gpu_work_admitted && record.admission_identity_matched &&
            record.gpu_terminal != pulp::gpu_audio::detail::SharedIoGpuTerminalDisposition::None &&
            terminal_sequences.insert(record.sequence).second;
        if (authenticated)
            ++authenticated_terminal_records;
        high_water_in_flight = std::max(high_water_in_flight, record.high_water_in_flight);
        if (record.gpu_terminal ==
            pulp::gpu_audio::detail::SharedIoGpuTerminalDisposition::CompletedAccepted)
            ++retired_success;
        else if (record.gpu_terminal !=
                 pulp::gpu_audio::detail::SharedIoGpuTerminalDisposition::None)
            ++retired_failure;
        if (record.gpu_terminal ==
            pulp::gpu_audio::detail::SharedIoGpuTerminalDisposition::LateRejected)
            ++late_completions;
    }

    if (config.corrupt_output)
        output[0][static_cast<std::size_t>(config.warmup + config.lead) * config.frames] += 1.0f;

    // Independent direct-double convolution runs after the timed phase. It
    // checks the actual delivered stream, including continuously primed fallback.
    const auto latency_frames = static_cast<std::size_t>(config.lead) * config.frames;
    std::uint64_t oracle_failed_blocks = 0;
    std::uint64_t measured_misses = 0;
    std::uint64_t callback_overruns = 0;
    std::uint64_t late_callback_starts = 0;
    const auto period_ns =
        static_cast<std::uint64_t>(config.frames) * 1'000'000'000ull / sample_rate;
    for (std::uint32_t block = 0; block < total_blocks; ++block) {
        auto& record = records[block];
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            for (std::uint32_t frame = 0; frame < config.frames; ++frame) {
                const auto position = static_cast<std::size_t>(block) * config.frames + frame;
                double expected = 0;
                if (position >= latency_frames) {
                    const auto source = position - latency_frames;
                    for (std::size_t tap = 0; tap < ir.size() && tap <= source; ++tap)
                        expected +=
                            static_cast<double>(ir[tap]) * input.channel(channel)[source - tap];
                }
                const double actual = output[channel][position];
                record.finite = record.finite && std::isfinite(actual);
                if (std::isfinite(actual))
                    record.max_error = std::max(record.max_error, std::abs(actual - expected));
            }
        }
        if (!record.finite || record.max_error > 2.0e-3)
            ++oracle_failed_blocks;
        if (block >= config.warmup + config.lead) {
            measured_misses += record.miss_delta;
            callback_overruns += record.callback_end - record.callback_begin > period_ns;
            late_callback_starts += record.callback_begin > record.scheduled + period_ns;
        }
    }

    if (config.directory.empty()) {
        config.directory = pulp::test::make_unique_temp_dir("pulp-paced-convolution");
    } else if (!std::filesystem::create_directory(config.directory)) {
        std::cerr << "output directory must be new: " << config.directory << '\n';
        return 2;
    }
    std::ofstream blocks(config.directory / "blocks.csv");
    blocks << "callback_sequence,measured,source_sequence,scheduled_ns,callback_begin_ns,"
              "callback_end_ns,miss_counter_delta,max_absolute_error,finite\n";
    blocks << std::setprecision(17);
    for (std::uint32_t block = 0; block < total_blocks; ++block) {
        const auto& r = records[block];
        blocks << block << ',' << (block >= config.warmup + config.lead) << ',';
        if (block >= config.lead)
            blocks << block - config.lead;
        blocks << ',' << r.scheduled << ',' << r.callback_begin << ',' << r.callback_end << ','
               << r.miss_delta << ',' << r.max_error << ',' << r.finite << '\n';
    }
    blocks.flush();
    if (!blocks)
        return 2;

    const bool correct = oracle_failed_blocks == 0;
    const bool gpu_progress =
        authenticated_terminal_records > 0 && high_water_in_flight > 0 && retired_success > 0 &&
        authenticated_terminal_records == terminal_record_count &&
        terminal_record_count == retired_success + retired_failure &&
        terminal_record_count == trace_stats.admissions_enqueued &&
        trace_stats.admissions_dropped == 0 && trace_stats.trace_dropped == 0 &&
        trace_stats.admissions_attempted ==
            trace_stats.admissions_enqueued + trace_stats.admissions_dropped &&
        trace_stats.trace_attempted == trace_stats.trace_enqueued + trace_stats.trace_dropped +
                                           trace_stats.trace_sampled_out +
                                           trace_stats.trace_invalid;
    const auto emit = [&](std::ostream& stream) {
        stream << "{\"schema\":\"pulp.gpu-audio-paced-convolution.v1\",\"status\":\""
               << (correct && gpu_progress ? "completed" : "failed")
               << "\",\"performance_verdict\":\"unassigned\",\"path\":\"shared_async\","
                  "\"completion_service\":\"process_events\",\"callback_driver\":\"sleep_until_non_"
                  "rt\","
                  "\"gpu_timestamps\":\"unavailable\",\"clock\":\"steady_clock\","
                  "\"timing_scope\":\"external_callback_envelope\",\"sample_rate_hz\":"
               << sample_rate << ",\"channels\":" << channels << ",\"ir_frames\":" << ir.size()
               << ",\"frames\":" << config.frames << ",\"lead_blocks\":"
               << config.lead
               // This is configured physical capacity, not an observed
               // delivery count. Keep the legacy key and expose the meaning.
               << ",\"provider_slots\":" << GpuConvolver::kSharedIoSlots
               << ",\"configured_provider_slots\":" << GpuConvolver::kSharedIoSlots
               << ",\"declared_slots\":" << GpuConvolver::kSharedIoSlots
               << ",\"declared_lead_blocks\":" << config.lead
               << ",\"high_water_in_flight\":" << high_water_in_flight
               << ",\"retired_success\":" << retired_success
               << ",\"retired_failure\":" << retired_failure
               << ",\"terminal_records\":" << (retired_success + retired_failure)
               << ",\"authenticated_terminal_records\":" << authenticated_terminal_records
               << ",\"terminal_record_count\":" << terminal_record_count
               << ",\"admissions_attempted\":" << trace_stats.admissions_attempted
               << ",\"admissions_enqueued\":" << trace_stats.admissions_enqueued
               << ",\"admissions_dropped\":" << trace_stats.admissions_dropped
               << ",\"trace_attempted\":" << trace_stats.trace_attempted
               << ",\"trace_enqueued\":" << trace_stats.trace_enqueued
               << ",\"trace_dropped\":" << trace_stats.trace_dropped
               << ",\"trace_sampled_out\":" << trace_stats.trace_sampled_out
               << ",\"trace_invalid\":" << trace_stats.trace_invalid
               << ",\"gpu_receipt_authenticated\":" << (gpu_progress ? "true" : "false")
               << ",\"fallback_blocks\":" << delivery_stats.cpu_fallback_blocks
               << ",\"miss_blocks\":" << transport_stats.miss_blocks
               << ",\"late_completions\":" << late_completions << ",\"run_identity\":\""
               << run_identity << "\""
               << ",\"logical_pipeline_capacity\":" << std::max(8u, config.lead + 2u)
               << ",\"warmup_blocks\":" << config.warmup << ",\"measured_blocks\":" << config.blocks
               << ",\"total_callbacks\":" << total_blocks
               << ",\"measured_miss_counter_delta\":" << measured_misses
               << ",\"callback_overruns\":" << callback_overruns
               << ",\"late_callback_starts\":" << late_callback_starts
               << ",\"oracle_failed_blocks\":" << oracle_failed_blocks
               << ",\"produced_blocks_before_stop\":" << transport_stats.produced_blocks
               << ",\"wake_on_write\":" << (config.wake ? "true" : "false")
               << ",\"tracing_compiled\":" << (pulp::runtime::kTracingEnabled ? "true" : "false")
               << ",\"negative_control\":" << (config.corrupt_output ? "true" : "false")
               << ",\"records_file\":\"blocks.csv\"}\n";
    };
    std::ofstream receipt(config.directory / "receipt.json");
    emit(receipt);
    receipt.flush();
    if (!receipt)
        return 2;
    emit(std::cout);
    std::cerr << "artifacts: " << config.directory << '\n';
    return correct && gpu_progress ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    Config config;
    if (!parse(argc, argv, config))
        return 2;
    try {
        return run(config);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
