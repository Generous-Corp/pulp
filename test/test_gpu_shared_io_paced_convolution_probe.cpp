#include "detail/gpu_convolver_trial_config.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "support/audio_test_signals.hpp"
#include "support/unique_temp_dir.hpp"

#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/runtime/crypto.hpp>
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
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {
#ifndef PULP_GPU_AUDIO_PROVIDER_ASSET_SHA256
#define PULP_GPU_AUDIO_PROVIDER_ASSET_SHA256 ""
#endif
#ifndef PULP_GPU_AUDIO_DAWN_ARCHIVE_SHA256
#define PULP_GPU_AUDIO_DAWN_ARCHIVE_SHA256 ""
#endif
#ifndef PULP_GPU_AUDIO_PROVIDER_MANIFEST_SHA256
#define PULP_GPU_AUDIO_PROVIDER_MANIFEST_SHA256 ""
#endif

using Clock = std::chrono::steady_clock;
using pulp::gpu_audio::GpuAudioTransport;
using pulp::gpu_audio::GpuConvolver;

struct Config {
    std::uint32_t frames = 32;
    std::uint32_t lead = 2;
    std::uint32_t blocks = 128;
    std::uint32_t warmup = 64;
    std::uint32_t slots = GpuConvolver::kSharedIoSlots;
    bool wake = false;
    bool corrupt_output = false;
    bool expect_failure = false;
    bool self_test = false;
    std::string run_kind = "cold";
    std::filesystem::path directory;
    std::filesystem::path raw_jsonl;
    std::filesystem::path executable;
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
            number("--blocks=", config.blocks) || number("--warmup=", config.warmup) ||
            number("--slots=", config.slots))
            continue;
        if (argument == "--wake-on-write")
            config.wake = true;
        else if (argument == "--negative-control")
            config.corrupt_output = true;
        else if (argument == "--expect-failure")
            config.expect_failure = true;
        else if (argument == "--self-test")
            config.self_test = true;
        else if (argument.starts_with("--run-kind="))
            config.run_kind = argument.substr(11);
        else if (argument.starts_with("--output-dir="))
            config.directory = argument.substr(13);
        else if (argument.starts_with("--raw-jsonl="))
            config.raw_jsonl = argument.substr(12);
        else
            return false;
    }
    return (config.frames == 32 || config.frames == 64 || config.frames == 128) &&
           (config.lead == 1 || config.lead == 2 || config.lead == 4 || config.lead == 8) &&
           (config.slots == 2 || config.slots == 4 || config.slots == 8 || config.slots == 16) &&
           (config.run_kind == "cold" || config.run_kind == "steady") &&
           (!config.expect_failure || config.corrupt_output) && config.blocks > 0 &&
           config.blocks <= kMaximumMeasuredBlocks && config.warmup <= 4096;
}

std::uint64_t nanoseconds(Clock::duration value) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
}

std::uint64_t process_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

struct Record {
    std::uint64_t scheduled = 0;
    std::uint64_t callback_begin = 0;
    std::uint64_t callback_end = 0;
    std::uint64_t miss_delta = 0;
    double max_error = 0;
    bool finite = true;
};

void json_string(std::ostream& stream, std::string_view value) {
    stream << '"';
    for (const char character : value) {
        switch (character) {
        case '"':
            stream << "\\\"";
            break;
        case '\\':
            stream << "\\\\";
            break;
        case '\n':
            stream << "\\n";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            stream << character;
            break;
        }
    }
    stream << '"';
}

using TraceRecord = pulp::gpu_audio::detail::SharedIoTraceRecord;
using TraceAdmission = pulp::gpu_audio::detail::SharedIoTraceAdmission;

std::string trace_identity(std::uint64_t engine, std::uint64_t generation, std::uint64_t sequence) {
    return std::to_string(engine) + ":" + std::to_string(generation) + ":" +
           std::to_string(sequence);
}

struct RawCensus {
    bool valid = true;
    std::unordered_set<std::string> admission_identities;
    std::unordered_set<std::string> delivery_identities;
    std::unordered_set<std::string> terminal_identities;
    std::vector<const TraceRecord*> lifecycle_records;
};

RawCensus collect_raw_census(const std::vector<TraceRecord>& records,
                             const std::vector<TraceAdmission>& admissions, std::uint64_t engine_id,
                             const std::unordered_set<std::string>& authenticated_terminals) {
    RawCensus census;
    for (const auto& admission : admissions) {
        if (!census.admission_identities
                 .insert(trace_identity(engine_id, admission.generation, admission.sequence))
                 .second)
            census.valid = false;
    }
    census.lifecycle_records.reserve(records.size());
    for (const auto& record : records) {
        if (record.kind != pulp::gpu_audio::detail::SharedIoTraceKind::Terminal &&
            record.kind != pulp::gpu_audio::detail::SharedIoTraceKind::Delivery)
            continue;
        const auto identity = trace_identity(engine_id, record.generation, record.sequence);
        census.lifecycle_records.push_back(&record);
        if (record.kind == pulp::gpu_audio::detail::SharedIoTraceKind::Terminal) {
            if (!census.admission_identities.contains(identity) || !record.valid() ||
                !record.admission_identity_matched ||
                record.gpu_terminal ==
                    pulp::gpu_audio::detail::SharedIoGpuTerminalDisposition::None ||
                !census.terminal_identities.insert(identity).second)
                census.valid = false;
        } else if (record.delivery == pulp::gpu_audio::detail::SharedIoDeliveryDisposition::None ||
                   !record.callback_timing_available ||
                   !census.delivery_identities.insert(identity).second) {
            census.valid = false;
        }
    }
    std::size_t admitted_delivery_count = 0;
    for (const auto& identity : census.delivery_identities)
        if (census.admission_identities.contains(identity))
            ++admitted_delivery_count;
    census.valid = census.valid &&
                   authenticated_terminals.size() == census.admission_identities.size() &&
                   admitted_delivery_count == census.admission_identities.size();
    return census;
}

void emit_raw_provenance(std::ostream& raw, std::string_view run_kind, std::uint64_t run_identity,
                         std::uint64_t engine_id,
                         const pulp::gpu_audio::detail::SharedIoProviderIdentity& provider_identity,
                         std::string_view executable_sha, std::string_view manifest_digest) {
    raw << "{\"kind\":\"provenance\",\"schema\":\"pulp.gpu-audio.p2.raw.v1\","
           "\"run_kind\":";
    json_string(raw, run_kind);
    raw << ",\"same_process_resident\":true,"
           "\"steady_semantics\":\"same_process_resident\",\"process_id\":"
        << process_id() << ",\"residency_session_id\":\"" << run_identity
        << "\",\"prepared_sessions\":1,\"reprepare_count\":0,\"engine_id\":" << engine_id
        << ",\"provider_identity_status\":\""
        << (provider_identity.authenticated ? "passed" : "failed")
        << "\",\"provider_observed_identity\":\""
        << (provider_identity.authenticated ? "passed" : "failed") << "\",\"provider_revision\":";
    json_string(raw, provider_identity.provider_revision);
    raw << ",\"adapter_name\":";
    json_string(raw, provider_identity.adapter_name);
    raw << ",\"adapter_backend\":";
    json_string(raw, provider_identity.adapter_backend);
    raw << ",\"adapter_vendor_id\":" << provider_identity.adapter_vendor_id
        << ",\"adapter_device_id\":" << provider_identity.adapter_device_id
        << ",\"native_runtime_identity_status\":\""
        << (provider_identity.native_runtime_authenticated ? "passed" : "failed")
        << "\",\"native_runtime_name\":";
    json_string(raw, provider_identity.native_runtime_name);
    raw << ",\"native_runtime_backend\":";
    json_string(raw, provider_identity.native_runtime_backend);
    raw << ",\"executable_observed_sha256\":";
    json_string(raw, executable_sha);
    raw << ",\"provider_asset_sha256\":";
    json_string(raw, PULP_GPU_AUDIO_PROVIDER_ASSET_SHA256);
    raw << ",\"dawn_archive_sha256\":";
    json_string(raw, PULP_GPU_AUDIO_DAWN_ARCHIVE_SHA256);
    raw << ",\"manifest_bindings\":{\"dawn_archive_manifest_sha256\":";
    json_string(raw, PULP_GPU_AUDIO_PROVIDER_MANIFEST_SHA256);
    raw << ",\"dawn_archive_sha256\":";
    json_string(raw, PULP_GPU_AUDIO_DAWN_ARCHIVE_SHA256);
    raw << ",\"provider_asset_manifest_sha256\":";
    json_string(raw, PULP_GPU_AUDIO_PROVIDER_MANIFEST_SHA256);
    raw << ",\"provider_asset_sha256\":";
    json_string(raw, PULP_GPU_AUDIO_PROVIDER_ASSET_SHA256);
    raw << "},\"provenance_manifest_sha256\":";
    json_string(raw, manifest_digest);
    raw << "}\n";
}

int self_test() {
    constexpr std::uint64_t engine_id = 7;
    std::vector<TraceAdmission> admissions{{1, 2}};
    TraceRecord terminal;
    terminal.kind = pulp::gpu_audio::detail::SharedIoTraceKind::Terminal;
    terminal.generation = 1;
    terminal.sequence = 2;
    terminal.gpu_work_admitted = true;
    terminal.admission_identity_matched = true;
    terminal.gpu_terminal =
        pulp::gpu_audio::detail::SharedIoGpuTerminalDisposition::CompletedAccepted;
    terminal.outcome = pulp::gpu_audio::detail::SharedIoTraceOutcome::Success;
    terminal.set(pulp::gpu_audio::detail::SharedIoTraceStage::Scheduled, 1);
    TraceRecord delivery;
    delivery.kind = pulp::gpu_audio::detail::SharedIoTraceKind::Delivery;
    delivery.generation = 1;
    delivery.sequence = 2;
    delivery.output_eligible = true;
    delivery.delivery = pulp::gpu_audio::detail::SharedIoDeliveryDisposition::GpuDelivered;
    delivery.callback_timing_available = true;
    std::vector<TraceRecord> records{terminal, delivery};
    std::unordered_set<std::string> authenticated{trace_identity(engine_id, 1, 2)};
    if (!collect_raw_census(records, admissions, engine_id, authenticated).valid) {
        std::cerr << "self-test: valid raw census rejected\n";
        return 1;
    }
    records.push_back(terminal);
    if (collect_raw_census(records, admissions, engine_id, authenticated).valid) {
        std::cerr << "self-test: duplicate terminal accepted\n";
        return 1;
    }

    pulp::gpu_audio::detail::SharedIoProviderIdentity provider;
    provider.authenticated = true;
    provider.provider_revision = "revision\"\\\n";
    provider.adapter_name = "adapter\"\\\n";
    provider.adapter_backend = "backend\"\\\n";
    provider.native_runtime_authenticated = true;
    provider.native_runtime_name = "runtime\"\\\n";
    provider.native_runtime_backend = "runtime-backend\"\\\n";
    std::ostringstream serialized;
    emit_raw_provenance(serialized, "cold\"\\\n", 42, engine_id, provider, "exe\"\\\n",
                        "manifest\"\\\n");
    const auto json = serialized.str();
    if (json.find("\\\"") == std::string::npos || json.find("\\\\") == std::string::npos ||
        json.find("\\n") == std::string::npos) {
        std::cerr << "self-test: provenance strings were not escaped\n";
        return 1;
    }
    std::cout << json;
    return 0;
}

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
    pulp::gpu_audio::detail::GpuConvolverTrialConfig trial_config;
    trial_config.requested_path =
        pulp::gpu_audio::detail::SharedIoRequest::RequireSharedHostPointer;
    trial_config.enable_trace = true;
    trial_config.capture_admissions = true;
    trial_config.capture_callback_timing = true;
    trial_config.success_stride = 1;
    trial_config.slots = config.slots;
    if (!pulp::gpu_audio::detail::configure_gpu_convolver_trial(node, trial_config))
        return 2;
    if (!node.set_provider_policy(GpuConvolver::ProviderPolicy::SharedRequired) ||
        !node.configure_trace({.enabled = true,
                               .capture_admissions = true,
                               .capture_callback_timing = true,
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
    const auto provider_identity = pulp::gpu_audio::detail::realtime_gpu_provider_identity(&node);
    const auto engine_id = pulp::gpu_audio::detail::gpu_convolver_trial_engine_id(node);
    // Stop new GPU admissions, then advance the callback timeline through the
    // configured lead window so the final admitted sequence receives its typed
    // delivery disposition before the session is released. These CPU-only
    // flush positions are deliberately excluded from measured blocks.
    const auto realtime_path = pulp::gpu_audio::detail::realtime_gpu_node_path(&node);
    if (!realtime_path.active() || !realtime_path.fence(realtime_path.context))
        return 2;
    std::vector<float> flush_input(static_cast<std::size_t>(channels) * config.frames, 0.0f);
    std::vector<float> flush_output(flush_input.size(), 0.0f);
    const float* flush_inputs[channels] = {flush_input.data(), flush_input.data() + config.frames};
    float* flush_outputs[channels] = {flush_output.data(), flush_output.data() + config.frames};
    pulp::audio::BufferView<const float> flush_in(flush_inputs, channels, config.frames);
    pulp::audio::BufferView<float> flush_out(flush_outputs, channels, config.frames);
    for (std::uint32_t block = 0; block < config.lead; ++block)
        transport.process(flush_in, flush_out, config.frames);
    transport.release();

    std::vector<pulp::gpu_audio::detail::SharedIoTraceRecord> trace_records;
    (void)pulp::gpu_audio::detail::drain_gpu_convolver_trial_records(node, trace_records);
    std::vector<pulp::gpu_audio::detail::SharedIoTraceAdmission> trace_admissions;
    (void)pulp::gpu_audio::detail::drain_gpu_convolver_trial_admissions(node, trace_admissions);
    const auto trace_stats = trace_records.empty() ? pulp::gpu_audio::detail::SharedIoTraceRecord{}
                                                   : trace_records.front();
    std::uint64_t high_water_in_flight = 0;
    std::uint64_t retired_success = 0;
    std::uint64_t retired_failure = 0;
    std::uint64_t late_completions = 0;
    std::uint64_t authenticated_terminal_records = 0;
    std::uint64_t terminal_record_count = 0;
    const auto terminal_identity = [engine_id](const auto& record) {
        return std::to_string(engine_id) + ":" + std::to_string(record.generation) + ":" +
               std::to_string(record.sequence);
    };
    std::unordered_set<std::string> terminal_identities;
    for (const auto& record : trace_records) {
        if (record.kind == pulp::gpu_audio::detail::SharedIoTraceKind::Terminal)
            ++terminal_record_count;
        const bool authenticated =
            record.valid() && record.kind == pulp::gpu_audio::detail::SharedIoTraceKind::Terminal &&
            record.gpu_work_admitted && record.admission_identity_matched &&
            record.gpu_terminal != pulp::gpu_audio::detail::SharedIoGpuTerminalDisposition::None &&
            terminal_identities.insert(terminal_identity(record)).second;
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

    // Preserve every terminal and delivery row in the raw census, including
    // callback-only priming and CPU-fallback positions. The admission-linked
    // subset is checked for one terminal and one delivery per admission, while
    // the complete delivery stream remains available for fallback accounting.
    const auto raw_census =
        collect_raw_census(trace_records, trace_admissions, engine_id, terminal_identities);
    const auto& admission_identities = raw_census.admission_identities;
    const auto& raw_lifecycle_records = raw_census.lifecycle_records;
    const bool raw_census_valid = raw_census.valid;

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
    const bool provider_authenticated = provider_identity.authenticated &&
                                        provider_identity.native_runtime_authenticated &&
                                        engine_id != 0;
    const bool receipt_authenticated = gpu_progress && provider_authenticated && raw_census_valid;
    const auto emit = [&](std::ostream& stream) {
        const auto executable_sha =
            pulp::runtime::sha256_file_hex(config.executable, 512ull * 1024ull * 1024ull)
                .value_or("");
        stream << "{\"schema\":\"pulp.gpu-audio-paced-convolution.v1\",\"status\":\""
               << (correct && receipt_authenticated ? "completed" : "failed")
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
               << ",\"provider_slots\":" << config.slots
               << ",\"configured_provider_slots\":" << config.slots
               << ",\"declared_slots\":" << config.slots
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
               << ",\"gpu_receipt_authenticated\":" << (receipt_authenticated ? "true" : "false")
               << ",\"provider_identity_status\":";
        json_string(stream, provider_identity.authenticated ? "passed" : "failed");
        stream << ",\"provider_observed_identity\":";
        json_string(stream, provider_identity.authenticated ? "passed" : "failed");
        stream << ",\"provider_revision\":";
        json_string(stream, provider_identity.provider_revision);
        stream << ",\"adapter_name\":";
        json_string(stream, provider_identity.adapter_name);
        stream << ",\"adapter_backend\":";
        json_string(stream, provider_identity.adapter_backend);
        stream << ",\"adapter_vendor_id\":" << provider_identity.adapter_vendor_id
               << ",\"adapter_device_id\":" << provider_identity.adapter_device_id
               << ",\"native_runtime_identity_status\":";
        json_string(stream, provider_identity.native_runtime_authenticated ? "passed" : "failed");
        stream << ",\"native_runtime_name\":";
        json_string(stream, provider_identity.native_runtime_name);
        stream << ",\"native_runtime_backend\":";
        json_string(stream, provider_identity.native_runtime_backend);
        stream << ",\"fallback_blocks\":" << delivery_stats.cpu_fallback_blocks
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
               << ",\"run_kind\":\"" << config.run_kind << "\""
               << ",\"process_id\":" << process_id() << ",\"same_process_resident\":true"
               << ",\"steady_semantics\":\"same_process_resident\""
               << ",\"residency_session_id\":\"" << run_identity << "\""
               << ",\"prepared_sessions\":1,\"reprepare_count\":0"
               << ",\"engine_id\":" << engine_id << ",\"executable_observed_sha256\":\""
               << executable_sha << "\""
               << ",\"records_file\":\"blocks.csv\"}\n";
    };
    std::ofstream receipt(config.directory / "receipt.json");
    emit(receipt);
    receipt.flush();
    if (!receipt)
        return 2;
    if (!config.raw_jsonl.empty()) {
        std::ofstream raw(config.raw_jsonl);
        if (!raw)
            return 2;
        const auto executable_sha =
            pulp::runtime::sha256_file_hex(config.executable, 512ull * 1024ull * 1024ull)
                .value_or("");
        const auto manifest_digest = pulp::runtime::sha256_hex(
            std::string("{\"dawn_archive_manifest_sha256\":\"") +
            PULP_GPU_AUDIO_PROVIDER_MANIFEST_SHA256 + "\",\"dawn_archive_sha256\":\"" +
            PULP_GPU_AUDIO_DAWN_ARCHIVE_SHA256 + "\",\"provider_asset_manifest_sha256\":\"" +
            PULP_GPU_AUDIO_PROVIDER_MANIFEST_SHA256 + "\",\"provider_asset_sha256\":\"" +
            PULP_GPU_AUDIO_PROVIDER_ASSET_SHA256 + "\"}");
        emit_raw_provenance(raw, config.run_kind, run_identity, engine_id, provider_identity,
                            executable_sha, manifest_digest);
        for (const auto& admission : trace_admissions) {
            raw << "{\"kind\":\"admission\",\"engine_id\":" << engine_id
                << ",\"generation\":" << admission.generation
                << ",\"sequence\":" << admission.sequence << "}\n";
        }
        for (const auto* record : raw_lifecycle_records) {
            const auto identity = trace_identity(engine_id, record->generation, record->sequence);
            raw << "{\"kind\":\"record\",\"trace_kind\":" << static_cast<unsigned>(record->kind)
                << ",\"engine_id\":" << engine_id << ",\"generation\":" << record->generation
                << ",\"sequence\":" << record->sequence
                << ",\"valid_stages\":" << record->valid_stages
                << ",\"gpu_terminal\":" << static_cast<unsigned>(record->gpu_terminal)
                << ",\"admission_identity_matched\":"
                << (record->admission_identity_matched ? "true" : "false")
                << ",\"admitted\":" << (admission_identities.contains(identity) ? "true" : "false")
                << ",\"callback_only\":"
                << (admission_identities.contains(identity) ? "false" : "true")
                << ",\"delivery\":" << static_cast<unsigned>(record->delivery)
                << ",\"callback_timing_available\":"
                << (record->callback_timing_available ? "true" : "false")
                << ",\"callback_end_ns\":" << record->callback_end_ns
                << ",\"result_visible_ns\":" << record->result_visible_ns << "}\n";
        }
        raw.flush();
        if (!raw)
            return 2;
    }
    emit(std::cout);
    std::cerr << "artifacts: " << config.directory << '\n';
    const bool passed = correct && receipt_authenticated;
    if (config.expect_failure)
        return !passed && config.corrupt_output ? 0 : 1;
    return passed ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    Config config;
    if (argc > 0)
        config.executable = argv[0];
    if (!parse(argc, argv, config))
        return 2;
    try {
        if (config.self_test)
            return self_test();
        return run(config);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
