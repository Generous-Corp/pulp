#include "detail/gpu_audio_transport_trial_observer.hpp"
#include "detail/gpu_convolver_raw_trace_jsonl.hpp"
#include "detail/gpu_convolver_trial_config.hpp"
#include "detail/realtime_gpu_audio_path.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_convolver.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
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
using pulp::gpu_audio::detail::SharedIoTraceKind;
using pulp::gpu_audio::detail::SharedIoTraceRecord;

constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kFrames = 32;
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kLeadBlocks = 2;
constexpr std::uint32_t kBlocks = 12;
constexpr std::uint64_t kPairId = 1;
constexpr std::uint64_t kGeneration = 1;
constexpr std::chrono::milliseconds kTerminalDrainTimeout{500};
// The matched trial drives the callback from this process rather than from a
// host audio clock. Pace it at one real block period so the two-slot provider
// is screened under the declared realtime cadence instead of an artificial
// burst that fills the fixed ingress queue before GPU completions can arrive.
constexpr auto kCallbackPeriod =
    std::chrono::nanoseconds{(std::uint64_t{1'000'000'000} * kFrames) / kSampleRate};

struct TrialResult {
    GpuConvolverTrialPath path = GpuConvolverTrialPath::SharedAsync;
    std::vector<SharedIoTraceRecord> records;
    std::vector<SharedIoTraceRecord> delivery_records;
    std::uint64_t misses = 0;
    bool available = false;
    bool records_valid = false;
    std::uint64_t duration_ns = 0;
};

std::uint64_t monotonic_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

TrialResult run_staged_sync_trial(const std::vector<std::vector<float>>& input,
                                  const std::vector<float>& ir) {
    TrialResult result;
    const auto started = monotonic_ns();
    result.path = GpuConvolverTrialPath::StagedSync;
    GpuConvolver node(kChannels, kFrames, kSampleRate, ir, kLeadBlocks);
    GpuConvolverTrialConfig config;
    config.requested_path = SharedIoRequest::RequireStaged;
    config.generation = kGeneration;
    config.enable_trace = true;
    config.staged_sync_reference = true;
    if (!pulp::gpu_audio::detail::configure_gpu_convolver_trial(node, config) || !node.prepare() ||
        !node.gpu_available())
        return result;

    std::vector<float> output(static_cast<std::size_t>(kChannels) * kFrames, 0.0f);
    std::vector<const float*> input_ptrs(kChannels);
    std::vector<float*> output_ptrs(kChannels);
    for (std::uint32_t block = 0; block < kBlocks; ++block) {
        for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
            input_ptrs[channel] = input[channel].data() + static_cast<std::size_t>(block) * kFrames;
            output_ptrs[channel] = output.data() + static_cast<std::size_t>(channel) * kFrames;
        }
        BufferView<const float> input_view(input_ptrs.data(), kChannels, kFrames);
        BufferView<float> output_view(output_ptrs.data(), kChannels, kFrames);
        const auto start = monotonic_ns();
        node.process_block(input_view, output_view, kFrames);
        const auto end = monotonic_ns();
        SharedIoTraceRecord delivery;
        delivery.kind = SharedIoTraceKind::Delivery;
        delivery.generation = kGeneration;
        delivery.sequence = block;
        delivery.output_eligible = true;
        delivery.delivery = pulp::gpu_audio::detail::SharedIoDeliveryDisposition::GpuDelivered;
        delivery.callback_timing_available = end >= start;
        delivery.callback_start_ns = start;
        delivery.callback_end_ns = end;
        delivery.result_visible_ns = end;
        result.delivery_records.push_back(delivery);
    }
    result.available = pulp::gpu_audio::detail::drain_gpu_convolver_trial_records(node, result.records);
    result.records_valid = result.available && result.records.size() == kBlocks &&
                           std::all_of(result.records.begin(), result.records.end(),
                                       [](const auto& record) { return record.valid(); });
    result.duration_ns = monotonic_ns() - started;
    return result;
}

// The observer is called from the audio callback. Keep this capture object
// fixed-capacity and trivially writable so the callback never allocates, locks,
// or serializes JSON. The benchmark drains it only after transport quiescence.
struct DeliveryCapture {
    static constexpr std::size_t capacity = kBlocks + 4;
    std::array<SharedIoTraceRecord, capacity> records{};
    std::size_t count = 0;
};

void capture_delivery(void* context, std::uint64_t sequence, std::uint8_t disposition,
                      std::uint64_t callback_start_ns, std::uint64_t callback_end_ns,
                      std::uint64_t result_visible_ns) noexcept {
    auto* capture = static_cast<DeliveryCapture*>(context);
    if (capture == nullptr || capture->count >= capture->records.size())
        return;
    auto& record = capture->records[capture->count++];
    record = {};
    record.kind = SharedIoTraceKind::Delivery;
    record.generation = kGeneration;
    record.sequence = sequence;
    record.output_eligible = true;
    record.delivery =
        static_cast<pulp::gpu_audio::detail::SharedIoDeliveryDisposition>(disposition);
    record.callback_start_ns = callback_start_ns;
    record.callback_end_ns = callback_end_ns;
    record.result_visible_ns = result_visible_ns;
    record.callback_timing_available = callback_start_ns != 0 &&
                                       callback_end_ns >= callback_start_ns &&
                                       result_visible_ns >= callback_end_ns;
}

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
    return path == GpuConvolverTrialPath::StagedSync
               ? "staged_sync"
               : path == GpuConvolverTrialPath::StagedAsync ? "staged_async" : "shared_async";
}

void emit_record(const TrialResult& result, std::size_t ordinal, const SharedIoTraceRecord& record,
                 const SharedIoTraceRecord& delivery, std::uint64_t input_digest,
                 std::uint64_t ir_digest) {
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
              << pulp::gpu_audio::detail::shared_io_delivery_name(delivery.delivery)
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
    const auto started = monotonic_ns();
    result.path = path;
    GpuConvolver node(kChannels, kFrames, kSampleRate, ir, kLeadBlocks);
    GpuConvolverTrialConfig config;
    config.requested_path = path == GpuConvolverTrialPath::StagedAsync
                                ? SharedIoRequest::RequireStaged
                                : SharedIoRequest::RequireSharedHostPointer;
    config.generation = kGeneration;
    config.enable_trace = true;
    config.capture_admissions = true;
    config.capture_callback_timing = true;
    config.success_stride = 1;
    if (!pulp::gpu_audio::detail::configure_gpu_convolver_trial(node, config) || !node.prepare())
        return result;

    // A shared trial must prove that the shared provider actually won path
    // selection; a staged trial must prove it did not silently use shared I/O.
    const bool shared_active = pulp::gpu_audio::detail::realtime_gpu_node_path(&node).active();
    if ((path == GpuConvolverTrialPath::SharedAsync) != shared_active || !node.gpu_available())
        return result;

    GpuAudioTransport transport;
    if (!transport.prepare(&node,
                           {.ring_blocks = 8, .run_worker_thread = true, .wake_on_write = true}))
        return result;

    DeliveryCapture delivery_capture;
    if (path == GpuConvolverTrialPath::StagedAsync &&
        !pulp::gpu_audio::configure_gpu_audio_transport_trial_observer(transport, &delivery_capture,
                                                                       capture_delivery))
        return result;

    std::vector<float> output(static_cast<std::size_t>(kChannels) * kFrames);
    std::vector<const float*> input_ptrs(kChannels);
    std::vector<float*> output_ptrs(kChannels);
    std::vector<float> tail_input(static_cast<std::size_t>(kChannels) * kFrames, 0.0f);
    auto next_callback = std::chrono::steady_clock::now();
    // The transport's lead latency means the final two measured GPU requests
    // become deliverable only after two additional callback positions. Drive
    // those positions with zero input, then retain only identities from the
    // measured window below.
    for (std::uint32_t block = 0; block < kBlocks + kLeadBlocks; ++block) {
        for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
            input_ptrs[channel] =
                block < kBlocks ? input[channel].data() + static_cast<std::size_t>(block) * kFrames
                                : tail_input.data() + static_cast<std::size_t>(channel) * kFrames;
            output_ptrs[channel] = output.data() + static_cast<std::size_t>(channel) * kFrames;
        }
        BufferView<const float> input_view(input_ptrs.data(), kChannels, kFrames);
        BufferView<float> output_view(output_ptrs.data(), kChannels, kFrames);
        transport.process(input_view, output_view, kFrames);
        next_callback += kCallbackPeriod;
        std::this_thread::sleep_until(next_callback);
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
        // The independent transport worker owns pump(). This bounded wait only
        // allows it to retire all submitted work before quiescent draining.
        if (transport.stats().produced_blocks >= kBlocks)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    result.misses = transport.stats().miss_blocks;
    transport.release();

    result.delivery_records.assign(delivery_capture.records.begin(),
                                   delivery_capture.records.begin() + delivery_capture.count);

    result.available =
        pulp::gpu_audio::detail::drain_gpu_convolver_trial_records(node, result.records);
    if (path == GpuConvolverTrialPath::SharedAsync) {
        for (const auto& record : result.records) {
            if (record.kind == SharedIoTraceKind::Delivery)
                result.delivery_records.push_back(record);
        }
    }
    const auto measured_record = [](const SharedIoTraceRecord& record) noexcept {
        return record.sequence < kBlocks;
    };
    result.records.erase(
        std::remove_if(result.records.begin(), result.records.end(),
                       [&](const auto& record) { return !measured_record(record); }),
        result.records.end());
    result.delivery_records.erase(
        std::remove_if(result.delivery_records.begin(), result.delivery_records.end(),
                       [&](const auto& record) { return !measured_record(record); }),
        result.delivery_records.end());
    result.records_valid = result.available && !result.records.empty() &&
                           !result.delivery_records.empty() &&
                           std::all_of(result.records.begin(), result.records.end(),
                                       [](const auto& record) { return record.valid(); });
    if (result.records_valid) {
        using Identity = std::pair<std::uint64_t, std::uint64_t>;
        std::set<Identity> terminal_ids;
        std::set<Identity> delivery_ids;
        for (const auto& record : result.records) {
            if (record.kind == SharedIoTraceKind::Terminal &&
                !terminal_ids.emplace(record.generation, record.sequence).second) {
                result.records_valid = false;
                break;
            }
        }
        for (const auto& delivery : result.delivery_records) {
            if (!delivery_ids.emplace(delivery.generation, delivery.sequence).second) {
                result.records_valid = false;
                break;
            }
        }
        if (result.records_valid && terminal_ids != delivery_ids)
            result.records_valid = false;

        std::vector<SharedIoTraceRecord> matched;
        matched.reserve(result.records.size() + result.delivery_records.size());
        for (const auto& terminal : result.records) {
            if (!result.records_valid)
                break;
            if (terminal.kind != SharedIoTraceKind::Terminal)
                continue;
            const auto it = std::find_if(result.delivery_records.begin(),
                                         result.delivery_records.end(), [&](const auto& delivery) {
                                             return delivery.generation == terminal.generation &&
                                                    delivery.sequence == terminal.sequence;
                                         });
            if (it == result.delivery_records.end()) {
                result.records_valid = false;
                break;
            }
            matched.push_back(terminal);
            matched.push_back(*it);
        }
        if (result.records_valid) {
            for (std::size_t i = 0; i + 1 < matched.size(); i += 2)
                emit_record(result, i / 2, matched[i], matched[i + 1], input_digest, ir_digest);
        }
    }
    result.duration_ns = monotonic_ns() - started;
    return result;
}

std::size_t terminal_record_count(const TrialResult& result) {
    return static_cast<std::size_t>(
        std::count_if(result.records.begin(), result.records.end(), [](const auto& record) {
            return record.kind == SharedIoTraceKind::Terminal && record.gpu_work_admitted;
        }));
}

bool emit_raw_receipt_if_authenticated(const TrialResult& staged_sync,
                                       const TrialResult& staged,
                                       const TrialResult& shared) {
    // The campaign is deliberately opt-in. Missing provenance must leave the
    // existing matched diagnostic intact rather than producing an anonymous
    // receipt that cannot be reproduced or attributed to a provider.
    const auto env = [](const char* name) -> const char* {
        const auto* value = std::getenv(name);
        return value != nullptr && *value != '\0' ? value : nullptr;
    };
    const auto source = env("PULP_GPU_AUDIO_P4_SOURCE_REVISION");
    const auto binary = env("PULP_GPU_AUDIO_P4_BINARY_SHA256");
    const auto machine = env("PULP_GPU_AUDIO_P4_MACHINE_ID");
    const auto model = env("PULP_GPU_AUDIO_P4_MACHINE_MODEL");
    const auto os = env("PULP_GPU_AUDIO_P4_OS_VERSION");
    const auto adapter = env("PULP_GPU_AUDIO_P4_ADAPTER_NAME");
    const auto provider = env("PULP_GPU_AUDIO_P4_PROVIDER");
    const auto provider_revision = env("PULP_GPU_AUDIO_P4_PROVIDER_REVISION");
    const auto provider_asset = env("PULP_GPU_AUDIO_P4_PROVIDER_ASSET_SHA256");
    const auto campaign_id = env("PULP_GPU_AUDIO_P4_CAMPAIGN_ID");
    const auto ui_p99 = env("PULP_GPU_AUDIO_P4_UI_P99_NS");
    const auto raw_output = env("PULP_GPU_AUDIO_P4_RAW_OUTPUT");
    if (!source || !binary || !machine || !model || !os || !adapter || !provider ||
        !provider_revision || !provider_asset || !campaign_id || !ui_p99 || !raw_output)
        return false;
    const auto parse_u64 = [](const char* value) -> std::uint64_t {
        char* end = nullptr;
        const auto parsed = std::strtoull(value, &end, 10);
        return end != value && end != nullptr && *end == '\0' ? parsed : 0;
    };
    const auto ui_frame_p99_ns = parse_u64(ui_p99);
    if (ui_frame_p99_ns == 0)
        return false;

    pulp::gpu_audio::detail::GpuConvolverRawManifest manifest;
    manifest.campaign = "screening";
    manifest.campaign_id = campaign_id;
    manifest.source_revision = source;
    manifest.binary_sha256 = binary;
    manifest.machine_id = machine;
    manifest.machine_model = model;
    manifest.os_version = os;
    manifest.adapter_name = adapter;
    manifest.adapter_backend = "metal";
    manifest.provider = provider;
    manifest.provider_revision = provider_revision;
    manifest.provider_asset_sha256 = provider_asset;
    manifest.generated_utc = "provided-by-campaign-wrapper";
    manifest.build_flags = {"-O3", "-DNDEBUG"};
    manifest.warmup_blocks = 2;
    manifest.expected_trials = 3;
    manifest.expected_matched_pairs = 1;
    manifest.expected_staged_sync_trials = 1;
    manifest.bootstrap_resamples = 100;
    manifest.paced = true;
    manifest.block_frames = kFrames;
    manifest.expected_blocks_per_trial = kBlocks;
    manifest.sample_rate_hz = kSampleRate;
    manifest.channels = kChannels;
    manifest.ir_frames = 257;
    manifest.inflight_depth = 2;
    manifest.lead_blocks = kLeadBlocks;
    manifest.deadline_ns = (std::uint64_t{1'000'000'000} * kFrames) / kSampleRate;
    manifest.watchdog_ns = manifest.deadline_ns * 4;

    std::array<std::vector<SharedIoTraceRecord>, 3> storage;
    const auto collect = [](const TrialResult& result,
                            std::vector<SharedIoTraceRecord>& destination) {
        destination.reserve(result.records.size() + result.delivery_records.size());
        for (const auto& record : result.records)
            if (record.kind == SharedIoTraceKind::Terminal)
                destination.push_back(record);
        for (const auto& record : result.delivery_records)
            if (record.kind == SharedIoTraceKind::Delivery)
                destination.push_back(record);
    };
    collect(staged_sync, storage[0]);
    collect(staged, storage[1]);
    collect(shared, storage[2]);
    using pulp::gpu_audio::detail::GpuConvolverRawTrial;
    using pulp::gpu_audio::detail::GpuConvolverRawTrialPath;
    const std::array<GpuConvolverRawTrial, 3> trials{
        GpuConvolverRawTrial{1, 0, GpuConvolverRawTrialPath::StagedSync, 1, kGeneration,
                             ui_frame_p99_ns, staged_sync.duration_ns, storage[0]},
        GpuConvolverRawTrial{2, kPairId, GpuConvolverRawTrialPath::StagedAsync, 1, kGeneration,
                             ui_frame_p99_ns, staged.duration_ns, storage[1]},
        GpuConvolverRawTrial{3, kPairId, GpuConvolverRawTrialPath::SharedAsync, 1, kGeneration,
                             ui_frame_p99_ns, shared.duration_ns, storage[2]}};
    std::ofstream output(raw_output, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    return pulp::gpu_audio::detail::write_gpu_convolver_raw_jsonl(output, manifest, trials);
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

    const auto staged_sync = run_staged_sync_trial(input, ir);
    const auto staged =
        run_trial(GpuConvolverTrialPath::StagedAsync, input, ir, input_digest, ir_digest);
    const auto shared =
        run_trial(GpuConvolverTrialPath::SharedAsync, input, ir, input_digest, ir_digest);
    if (!staged_sync.available || !staged.available || !shared.available) {
        std::cout << "{\"schema\":\"pulp.gpu-audio.p4.matched.v1\",\"status\":\"unavailable\""
                     ",\"performance_verdict\":\"unassigned\",\"reason\":\"required_path_"
                     "unavailable\"}\n";
        return 77;
    }
    const auto staged_sync_terminal_records = terminal_record_count(staged_sync);
    const auto staged_terminal_records = terminal_record_count(staged);
    const auto shared_terminal_records = terminal_record_count(shared);
    const bool matched = staged_sync.records_valid && staged.records_valid && shared.records_valid &&
                         staged_sync_terminal_records == kBlocks &&
                         staged_terminal_records == kBlocks && shared_terminal_records == kBlocks;
    std::cout << "{\"schema\":\"pulp.gpu-audio.p4.matched.v1\",\"status\":\""
              << (matched ? "screening_complete" : "screening_failed")
              << "\",\"performance_verdict\":\"unassigned\",\"pair_id\":" << kPairId
              << ",\"geometry\":{\"sample_rate_hz\":" << kSampleRate
              << ",\"channels\":" << kChannels << ",\"block_frames\":" << kFrames
              << ",\"lead_blocks\":" << kLeadBlocks << ",\"ir_frames\":" << ir.size()
              << "},\"input_digest\":" << input_digest << ",\"ir_digest\":" << ir_digest
              << ",\"staged_sync_records\":" << staged_sync.records.size()
              << ",\"staged_records\":" << staged.records.size()
              << ",\"staged_sync_terminal_records\":" << staged_sync_terminal_records
              << ",\"shared_records\":" << shared.records.size()
              << ",\"staged_terminal_records\":" << staged_terminal_records
              << ",\"shared_terminal_records\":" << shared_terminal_records
              << ",\"terminal_records_required\":" << kBlocks
              << ",\"staged_misses\":" << staged.misses << ",\"shared_misses\":" << shared.misses
              << ",\"raw_receipt\":\""
              << (matched && emit_raw_receipt_if_authenticated(staged_sync, staged, shared)
                      ? "emitted"
                      : "not_emitted")
              << "\"}\n";
    return matched ? 0 : 1;
}
