#include "detail/gpu_convolver_trial_config.hpp"
#include "detail/realtime_gpu_audio_path.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_convolver.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using pulp::audio::BufferView;
using pulp::gpu_audio::GpuAudioTransport;
using pulp::gpu_audio::GpuConvolver;
using pulp::gpu_audio::detail::GpuConvolverTrialConfig;
using pulp::gpu_audio::detail::GpuConvolverTrialPath;
using pulp::gpu_audio::detail::SharedIoRequest;
using pulp::gpu_audio::detail::SharedIoTraceRecord;

constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kFrames = 32;
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kLeadBlocks = 2;
constexpr std::uint32_t kBlocks = 12;
constexpr std::uint64_t kPairId = 1;
constexpr std::uint64_t kGeneration = 1;
constexpr std::chrono::milliseconds kTerminalDrainTimeout{500};

struct TrialResult {
    GpuConvolverTrialPath path = GpuConvolverTrialPath::SharedAsync;
    std::vector<SharedIoTraceRecord> records;
    std::uint64_t misses = 0;
    bool available = false;
    bool records_valid = false;
};

std::vector<float> make_ir() {
    std::vector<float> ir(257);
    for (std::size_t i = 0; i < ir.size(); ++i)
        ir[i] = static_cast<float>(0.05 * std::cos(0.031 * i) * std::exp(-0.009 * i));
    return ir;
}

std::vector<std::vector<float>> make_input() {
    std::vector<std::vector<float>> input(
        kChannels, std::vector<float>(static_cast<std::size_t>(kBlocks) * kFrames));
    for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
        for (std::size_t i = 0; i < input[channel].size(); ++i) {
            input[channel][i] = static_cast<float>(0.23 * std::sin(0.019 * i + channel * 0.73) +
                                                   0.11 * std::cos(0.083 * i + channel * 0.31));
        }
        input[channel][0] += channel == 0 ? 0.71f : -0.43f;
        input[channel][97u + 19u * channel] += channel == 0 ? -0.37f : 0.59f;
    }
    return input;
}

std::uint64_t digest(std::span<const float> values) {
    // A stable context fingerprint, not a cryptographic receipt digest. The
    // strict raw campaign will replace this with its source/kernel/plan hashes.
    std::uint64_t value = 1469598103934665603ull;
    for (const auto sample : values) {
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(sample));
        std::memcpy(&bits, &sample, sizeof(bits));
        value ^= bits;
        value *= 1099511628211ull;
    }
    return value;
}

const char* path_name(GpuConvolverTrialPath path) {
    return path == GpuConvolverTrialPath::StagedAsync ? "staged_async" : "shared_async";
}

void emit_record(const TrialResult& result, std::size_t ordinal, const SharedIoTraceRecord& record,
                 std::uint64_t input_digest, std::uint64_t ir_digest) {
    const auto encode = pulp::gpu_audio::detail::shared_io_trace_duration(
        record, pulp::gpu_audio::detail::SharedIoTraceStage::EncodeBegin,
        pulp::gpu_audio::detail::SharedIoTraceStage::EncodeEnd);
    const auto completion = pulp::gpu_audio::detail::shared_io_trace_duration(
        record, pulp::gpu_audio::detail::SharedIoTraceStage::SubmitBegin,
        pulp::gpu_audio::detail::SharedIoTraceStage::CompletionObserved);
    std::cout << "{\"schema\":\"pulp.gpu-audio.p4.matched.v1\",\"record_kind\":\"block\""
              << ",\"pair_id\":" << kPairId << ",\"path\":\"" << path_name(result.path)
              << "\",\"block_ordinal\":" << ordinal << ",\"generation\":" << record.generation
              << ",\"sequence\":" << record.sequence << ",\"input_digest\":" << input_digest
              << ",\"ir_digest\":" << ir_digest << ",\"gpu_terminal\":\""
              << pulp::gpu_audio::detail::shared_io_gpu_terminal_name(record.gpu_terminal)
              << "\",\"delivery\":\""
              << pulp::gpu_audio::detail::shared_io_delivery_name(record.delivery)
              << "\",\"timings\":{\"callback_cpu\":{\"availability\":\"unavailable\"}"
              << ",\"encode_cpu\":{\"availability\":\""
              << (encode.available ? "available" : "unavailable") << "\"";
    if (encode.available)
        std::cout << ",\"value_ns\":" << encode.ns;
    std::cout << "},\"submit_to_completion\":{\"availability\":\""
              << (completion.available ? "available" : "unavailable") << "\"";
    if (completion.available)
        std::cout << ",\"value_ns\":" << completion.ns;
    std::cout << "},\"publish_to_consumable\":{\"availability\":\"unavailable\"}}"
                 ",\"provenance\":{\"transfer_counters\":\"unavailable\",\"timings\":\"worker_"
                 "direct\"}}\n";
}

TrialResult run_trial(GpuConvolverTrialPath path, const std::vector<std::vector<float>>& input,
                      const std::vector<float>& ir, std::uint64_t input_digest,
                      std::uint64_t ir_digest) {
    TrialResult result;
    result.path = path;
    GpuConvolver node(kChannels, kFrames, kSampleRate, ir, kLeadBlocks);
    GpuConvolverTrialConfig config;
    config.requested_path = path == GpuConvolverTrialPath::StagedAsync
                                ? SharedIoRequest::RequireStaged
                                : SharedIoRequest::RequireSharedHostPointer;
    config.generation = kGeneration;
    config.enable_trace = true;
    config.capture_admissions = true;
    config.success_stride = 1;
    if (!pulp::gpu_audio::detail::configure_gpu_convolver_trial(node, config) || !node.prepare())
        return result;

    // A shared trial must prove that the shared provider actually won path
    // selection; a staged trial must prove it did not silently use shared I/O.
    const bool shared_active = pulp::gpu_audio::detail::realtime_gpu_node_path(&node).active();
    if ((path == GpuConvolverTrialPath::SharedAsync) != shared_active || !node.gpu_available())
        return result;

    GpuAudioTransport transport;
    if (!transport.prepare(&node, {.ring_blocks = 8, .run_worker_thread = false}))
        return result;

    std::vector<float> output(static_cast<std::size_t>(kChannels) * kFrames);
    std::vector<const float*> input_ptrs(kChannels);
    std::vector<float*> output_ptrs(kChannels);
    for (std::uint32_t block = 0; block < kBlocks; ++block) {
        for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
            input_ptrs[channel] = input[channel].data() + static_cast<std::size_t>(block) * kFrames;
            output_ptrs[channel] = output.data() + static_cast<std::size_t>(channel) * kFrames;
        }
        BufferView<const float> input_view(input_ptrs.data(), kChannels, kFrames);
        BufferView<float> output_view(output_ptrs.data(), kChannels, kFrames);
        transport.process(input_view, output_view, kFrames);
        transport.pump(1);
    }

    // Shared-I/O completions arrive through Dawn's non-RT ProcessEvents
    // dispatcher. Keep servicing the provider until every submitted block has
    // a terminal record (or the bounded diagnostic timeout expires) before
    // stopping the transport and reading the authenticated trace queue. The
    // callback path is already quiescent here; this wait is not benchmark
    // timing evidence.
    const auto drain_deadline = std::chrono::steady_clock::now() + kTerminalDrainTimeout;
    while (transport.stats().produced_blocks < kBlocks &&
           std::chrono::steady_clock::now() < drain_deadline) {
        transport.pump(0);
        if (transport.stats().produced_blocks >= kBlocks)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    transport.pump(0);
    result.misses = transport.stats().miss_blocks;
    transport.release();

    result.available =
        pulp::gpu_audio::detail::drain_gpu_convolver_trial_records(node, result.records);
    result.records_valid = result.available && !result.records.empty() &&
                           std::all_of(result.records.begin(), result.records.end(),
                                       [](const auto& record) { return record.valid(); });
    if (result.records_valid) {
        for (std::size_t i = 0; i < result.records.size(); ++i)
            emit_record(result, i, result.records[i], input_digest, ir_digest);
    }
    return result;
}

std::size_t terminal_record_count(const TrialResult& result) {
    return static_cast<std::size_t>(std::count_if(
        result.records.begin(), result.records.end(),
        [](const auto& record) { return record.gpu_work_admitted; }));
}

} // namespace

int main() {
    const auto ir = make_ir();
    const auto input = make_input();
    std::vector<float> flattened;
    for (const auto& channel : input)
        flattened.insert(flattened.end(), channel.begin(), channel.end());
    const auto input_digest = digest(flattened);
    const auto ir_digest = digest(ir);

    const auto staged =
        run_trial(GpuConvolverTrialPath::StagedAsync, input, ir, input_digest, ir_digest);
    const auto shared =
        run_trial(GpuConvolverTrialPath::SharedAsync, input, ir, input_digest, ir_digest);
    if (!staged.available || !shared.available) {
        std::cout << "{\"schema\":\"pulp.gpu-audio.p4.matched.v1\",\"status\":\"unavailable\""
                     ",\"performance_verdict\":\"unassigned\",\"reason\":\"required_path_"
                     "unavailable\"}\n";
        return 77;
    }
    const auto staged_terminal_records = terminal_record_count(staged);
    const auto shared_terminal_records = terminal_record_count(shared);
    const bool matched = staged.records_valid && shared.records_valid &&
                         staged_terminal_records == kBlocks &&
                         shared_terminal_records == kBlocks;
    std::cout << "{\"schema\":\"pulp.gpu-audio.p4.matched.v1\",\"status\":\""
              << (matched ? "screening_complete" : "screening_failed")
              << "\",\"performance_verdict\":\"unassigned\",\"pair_id\":" << kPairId
              << ",\"geometry\":{\"sample_rate_hz\":" << kSampleRate
              << ",\"channels\":" << kChannels << ",\"block_frames\":" << kFrames
              << ",\"lead_blocks\":" << kLeadBlocks << ",\"ir_frames\":" << ir.size()
              << "},\"input_digest\":" << input_digest << ",\"ir_digest\":" << ir_digest
              << ",\"staged_records\":" << staged.records.size()
              << ",\"shared_records\":" << shared.records.size()
              << ",\"staged_terminal_records\":" << staged_terminal_records
              << ",\"shared_terminal_records\":" << shared_terminal_records
              << ",\"terminal_records_required\":" << kBlocks
              << ",\"staged_misses\":" << staged.misses << ",\"shared_misses\":" << shared.misses
              << ",\"raw_receipt\":\"not_emitted\"}\n";
    return matched ? 0 : 1;
}
