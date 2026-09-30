// Opt-in CoreAudio workgroup A/B benchmark for the experimental shared-I/O path.
//
// This is deliberately a real CoreAudio render callback, rather than a timer
// that pretends to be one. The callback is silent and only advances a prepared
// transport. The worker remains the only caller of Dawn; this benchmark measures
// whether adopting the device's callback workgroup changes worker-side tails.

#include "detail/gpu_audio_transport_trial_observer.hpp"
#include "detail/gpu_convolver_trial_config.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/shared_io_trace.hpp"

#include <coreaudio_device.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/device.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_convolver.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {
using pulp::audio::BufferView;
using pulp::audio::DeviceConfig;
using pulp::audio::mac::CoreAudioSystem;
using pulp::gpu_audio::GpuAudioTransport;
using pulp::gpu_audio::GpuConvolver;
using pulp::gpu_audio::detail::SharedIoTraceRecord;

constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kFrames = 32;
constexpr std::uint32_t kRate = 48'000;
// Keep the screening run below the paired callback/worker trace queue capacity.
// Longer campaigns need an explicit lossless drain/statistics contract.
constexpr std::uint32_t kBlocks = 64;
constexpr std::uint32_t kLead = 2;
constexpr std::uint64_t kGeneration = 1;
constexpr auto kWait = std::chrono::seconds(15);

struct DeliveryCapture {
    static constexpr std::size_t capacity = kBlocks + 8;
    std::array<SharedIoTraceRecord, capacity> records{};
    std::atomic<std::size_t> count{0};
    std::atomic<bool> overflow{false};
};

void capture_delivery(void* context, std::uint64_t sequence, std::uint8_t disposition,
                      std::uint64_t callback_start_ns, std::uint64_t callback_end_ns,
                      std::uint64_t visible_ns) noexcept {
    auto* capture = static_cast<DeliveryCapture*>(context);
    const auto index = capture->count.fetch_add(1, std::memory_order_relaxed);
    if (index >= capture->records.size()) {
        capture->overflow.store(true, std::memory_order_relaxed);
        return;
    }
    auto& record = capture->records[index];
    record = {};
    record.kind = pulp::gpu_audio::detail::SharedIoTraceKind::Delivery;
    record.generation = kGeneration;
    record.sequence = sequence;
    record.output_eligible = true;
    record.delivery =
        static_cast<pulp::gpu_audio::detail::SharedIoDeliveryDisposition>(disposition);
    record.callback_start_ns = callback_start_ns;
    record.callback_end_ns = callback_end_ns;
    record.result_visible_ns = visible_ns;
    record.callback_timing_available = callback_start_ns != 0 &&
                                       callback_end_ns >= callback_start_ns &&
                                       visible_ns >= callback_end_ns;
}

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::vector<float> make_ir() {
    std::vector<float> ir(129);
    for (std::size_t i = 0; i < ir.size(); ++i)
        ir[i] = static_cast<float>(0.08 * std::cos(0.031 * i) * std::exp(-0.012 * i));
    return ir;
}

struct ModeResult {
    const char* mode = "ordinary";
    bool opened = false;
    bool started = false;
    bool callback_workgroup_available = false;
    bool worker_joined = false;
    std::uint64_t worker_join_failures = 0;
    std::uint64_t callbacks = 0;
    std::uint64_t misses = 0;
    std::uint64_t produced = 0;
    std::uint64_t callback_p99_ns = 0;
    std::uint64_t callback_max_ns = 0;
    std::uint64_t delivery_records = 0;
    std::uint64_t terminal_records = 0;
    bool identity_valid = false;
    std::uint32_t actual_buffer_size = 0;
    std::uint64_t oracle_checked = 0;
    std::uint64_t oracle_mismatches = 0;
    float oracle_max_error = 0.0f;
    std::vector<SharedIoTraceRecord> trace_records;
};

using BlockAudio = std::array<float, kChannels * kFrames>;
using TrialAudio = std::array<BlockAudio, kBlocks>;

TrialAudio make_input() {
    TrialAudio input{};
    for (std::uint32_t block = 0; block < kBlocks; ++block)
        for (std::uint32_t channel = 0; channel < kChannels; ++channel)
            for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
                const auto absolute = static_cast<double>(block * kFrames + frame);
                input[block][channel * kFrames + frame] =
                    static_cast<float>(0.21 * std::sin(0.017 * absolute + channel * 0.41) +
                                       0.07 * std::cos(0.071 * absolute + channel * 0.19));
            }
    input[0][0] += 0.73f;
    input[3][kFrames + 7] -= 0.31f;
    return input;
}

TrialAudio make_oracle(const TrialAudio& input, const std::vector<float>& ir) {
    TrialAudio expected{};
    for (std::uint32_t block = 0; block < kBlocks; ++block) {
        if (block < kLead)
            continue;
        const auto source_block = block - kLead;
        for (std::uint32_t channel = 0; channel < kChannels; ++channel)
            for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
                const auto output_index = static_cast<std::size_t>(source_block * kFrames + frame);
                double sum = 0.0;
                for (std::size_t tap = 0; tap < ir.size(); ++tap) {
                    if (tap > output_index)
                        break;
                    const auto source_index = output_index - tap;
                    const auto source_block_index = source_index / kFrames;
                    const auto source_frame = source_index % kFrames;
                    sum += static_cast<double>(
                               input[source_block_index][channel * kFrames + source_frame]) *
                           ir[tap];
                }
                expected[block][channel * kFrames + frame] = static_cast<float>(sum);
            }
    }
    return expected;
}

ModeResult run_mode(bool use_workgroup, const TrialAudio& input_blocks, const TrialAudio& oracle,
                    const std::vector<float>& ir) {
    ModeResult result;
    result.mode = use_workgroup ? "audio_workgroup" : "ordinary_worker";

    CoreAudioSystem system;
    auto device = system.create_device("");
    if (!device)
        return result;
    DeviceConfig device_config;
    device_config.sample_rate = kRate;
    device_config.buffer_size = kFrames;
    device_config.input_channels = 0;
    device_config.output_channels = kChannels;
    if (!device->open(device_config))
        return result;
    result.opened = true;
    result.actual_buffer_size = static_cast<std::uint32_t>(device->buffer_size());
    if (result.actual_buffer_size != kFrames) {
        device->close();
        return result;
    }
    void* const callback_workgroup = device->callback_workgroup();
    result.callback_workgroup_available = callback_workgroup != nullptr;
    if (use_workgroup && callback_workgroup == nullptr) {
        device->close();
        return result;
    }

    GpuConvolver node(kChannels, kFrames, kRate, ir, kLead);
    if (!node.set_provider_policy(GpuConvolver::ProviderPolicy::SharedRequired))
        return result;
    pulp::gpu_audio::detail::GpuConvolverTrialConfig trial_config;
    trial_config.requested_path =
        pulp::gpu_audio::detail::SharedIoRequest::RequireSharedHostPointer;
    trial_config.generation = kGeneration;
    trial_config.enable_trace = true;
    trial_config.capture_admissions = true;
    trial_config.capture_callback_timing = true;
    trial_config.success_stride = 1;
    if (!pulp::gpu_audio::detail::configure_gpu_convolver_trial(node, trial_config) ||
        !node.prepare() || !node.gpu_available() ||
        !pulp::gpu_audio::detail::realtime_gpu_node_path(&node).active()) {
        device->close();
        return result;
    }

    GpuAudioTransport transport;
    GpuAudioTransport::Config config;
    config.ring_blocks = 8;
    config.run_worker_thread = true;
    config.wake_on_write = true;
    config.audio_workgroup = use_workgroup ? callback_workgroup : nullptr;
    config.join_audio_workgroup = use_workgroup;
    if (!transport.prepare(&node, config)) {
        device->close();
        return result;
    }
    DeliveryCapture capture;
    if (!pulp::gpu_audio::configure_gpu_audio_transport_trial_observer(transport, &capture,
                                                                       capture_delivery)) {
        transport.release();
        device->close();
        return result;
    }

    // Worker adoption is a strict precondition for the workgroup arm. Wait for
    // the owned worker to make its one join attempt before opening the callback.
    const auto join_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (use_workgroup && std::chrono::steady_clock::now() < join_deadline) {
        const auto stats = transport.stats();
        if (stats.worker_workgroup_joined || stats.worker_workgroup_join_failures != 0)
            break;
        std::this_thread::yield();
    }
    auto preflight = transport.stats();
    if (use_workgroup && !preflight.worker_workgroup_joined) {
        result.worker_join_failures = preflight.worker_workgroup_join_failures;
        transport.release();
        device->close();
        return result;
    }

    std::array<float, kChannels * kFrames> input_storage{};
    std::array<const float*, kChannels> input_ptrs{input_storage.data(),
                                                   input_storage.data() + kFrames};
    std::array<float*, kChannels> output_ptrs{};
    std::atomic<std::uint64_t> callback_count{0};
    std::atomic<std::uint64_t> completed_callbacks{0};
    std::atomic<std::uint64_t> callback_max_ns{0};
    std::array<std::uint64_t, kBlocks + 8> callback_durations{};
    std::array<std::uint8_t, kBlocks> oracle_checked{};
    std::array<std::uint8_t, kBlocks> oracle_mismatches{};
    std::array<float, kBlocks> oracle_errors{};
    std::atomic<bool> callback_overflow{false};
    auto callback = [&](const BufferView<const float>&, BufferView<float>& output,
                        const pulp::audio::CallbackContext&) {
        const auto start = now_ns();
        const auto ordinal = callback_count.fetch_add(1, std::memory_order_relaxed);
        if (output.num_channels() != kChannels || output.num_samples() != kFrames) {
            output.clear();
            callback_overflow.store(true, std::memory_order_relaxed);
            return;
        }
        if (ordinal < input_blocks.size())
            std::copy(input_blocks[ordinal].begin(), input_blocks[ordinal].end(),
                      input_storage.begin());
        else
            std::fill(input_storage.begin(), input_storage.end(), 0.0f);
        for (std::uint32_t ch = 0; ch < kChannels && ch < output.num_channels(); ++ch)
            output_ptrs[ch] = output.channel_ptr(ch);
        BufferView<const float> input(input_ptrs.data(), kChannels, kFrames);
        BufferView<float> target(output_ptrs.data(), output.num_channels(), kFrames);
        transport.process(input, target, kFrames);
        if (ordinal < oracle.size()) {
            float max_error = 0.0f;
            bool finite = true;
            for (std::uint32_t channel = 0; channel < kChannels; ++channel)
                for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
                    const auto actual = output.channel_ptr(channel)[frame];
                    const auto reference = oracle[ordinal][channel * kFrames + frame];
                    if (!std::isfinite(actual))
                        finite = false;
                    max_error = std::max(max_error, std::abs(actual - reference));
                }
            if (finite) {
                oracle_checked[ordinal] = 1;
                oracle_errors[ordinal] = max_error;
                if (max_error > 2.0e-3f)
                    oracle_mismatches[ordinal] = 1;
            }
        }
        for (std::uint32_t ch = 0; ch < output.num_channels(); ++ch)
            std::fill_n(output.channel_ptr(ch), kFrames, 0.0f);
        const auto duration = now_ns() - start;
        auto old = callback_max_ns.load(std::memory_order_relaxed);
        while (old < duration &&
               !callback_max_ns.compare_exchange_weak(old, duration, std::memory_order_relaxed)) {
        }
        if (ordinal < callback_durations.size())
            callback_durations[ordinal] = duration;
        else
            callback_overflow.store(true, std::memory_order_relaxed);
        completed_callbacks.fetch_add(1, std::memory_order_release);
    };

    if (!device->start(callback)) {
        transport.release();
        device->close();
        return result;
    }
    result.started = true;
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (completed_callbacks.load(std::memory_order_acquire) < kBlocks &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    device->stop();

    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (transport.stats().produced_blocks < kBlocks &&
           std::chrono::steady_clock::now() < drain_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const auto worker_before_release = transport.stats();
    transport.release();
    // release() joins the worker and performs its final pump. All delivery and
    // trace records must be read only after that join, while the borrowed
    // CoreAudio workgroup handle is still valid.
    const auto stats = transport.stats();
    result.callbacks = completed_callbacks.load(std::memory_order_acquire);
    result.misses = stats.miss_blocks;
    result.produced = stats.produced_blocks;
    result.worker_joined = worker_before_release.worker_workgroup_joined;
    result.worker_join_failures = std::max(worker_before_release.worker_workgroup_join_failures,
                                           stats.worker_workgroup_join_failures);
    result.callback_max_ns = callback_max_ns.load(std::memory_order_relaxed);
    for (std::uint64_t i = 0; i < std::min<std::uint64_t>(result.callbacks, kBlocks); ++i) {
        result.oracle_checked += oracle_checked[i];
        result.oracle_mismatches += oracle_mismatches[i];
        result.oracle_max_error = std::max(result.oracle_max_error, oracle_errors[i]);
    }
    if (result.callbacks > 0 && !callback_overflow.load(std::memory_order_relaxed)) {
        std::vector<std::uint64_t> durations(
            callback_durations.begin(),
            callback_durations.begin() +
                std::min<std::uint64_t>(result.callbacks, callback_durations.size()));
        std::sort(durations.begin(), durations.end());
        result.callback_p99_ns = durations[(durations.size() * 99) / 100];
    }
    result.delivery_records = capture.count.load(std::memory_order_acquire);
    if (pulp::gpu_audio::detail::drain_gpu_convolver_trial_records(node, result.trace_records)) {
        result.terminal_records = static_cast<std::uint64_t>(std::count_if(
            result.trace_records.begin(), result.trace_records.end(), [](const auto& record) {
                return record.kind == pulp::gpu_audio::detail::SharedIoTraceKind::Terminal;
            }));
    }
    std::unordered_set<std::uint64_t> terminal_sequences;
    bool terminal_ids_unique = true;
    for (const auto& record : result.trace_records) {
        if (record.kind == pulp::gpu_audio::detail::SharedIoTraceKind::Terminal &&
            !terminal_sequences.insert(record.sequence).second) {
            terminal_ids_unique = false;
        }
    }
    std::unordered_set<std::uint64_t> delivery_sequences;
    bool delivery_ids_unique = true;
    const auto delivery_count = std::min<std::size_t>(capture.count.load(std::memory_order_acquire),
                                                      capture.records.size());
    for (std::size_t i = 0; i < delivery_count; ++i) {
        if (!delivery_sequences.insert(capture.records[i].sequence).second)
            delivery_ids_unique = false;
    }
    result.identity_valid = terminal_ids_unique && delivery_ids_unique &&
                            !capture.overflow.load(std::memory_order_relaxed) &&
                            !result.trace_records.empty();
    // The worker's AudioWorkgroup token was released by transport.release().
    // The device remains open until after the worker and all records are
    // quiescent because the callback_workgroup handle is borrowed.
    device->close();
    return result;
}

void emit(const ModeResult& result) {
    std::cout << "{\"schema\":\"pulp.gpu-audio.coreaudio-workgroup-ab.v1\","
              << "\"mode\":\"" << result.mode
              << "\",\"opened\":" << (result.opened ? "true" : "false")
              << ",\"started\":" << (result.started ? "true" : "false")
              << ",\"callback_workgroup_available\":"
              << (result.callback_workgroup_available ? "true" : "false")
              << ",\"worker_joined\":" << (result.worker_joined ? "true" : "false")
              << ",\"worker_join_failures\":" << result.worker_join_failures
              << ",\"callbacks\":" << result.callbacks << ",\"produced_blocks\":" << result.produced
              << ",\"miss_blocks\":" << result.misses
              << ",\"delivery_records\":" << result.delivery_records
              << ",\"terminal_records\":" << result.terminal_records
              << ",\"identity_valid\":" << (result.identity_valid ? "true" : "false")
              << ",\"actual_buffer_size\":" << result.actual_buffer_size
              << ",\"lead_blocks\":" << kLead << ",\"oracle_checked\":" << result.oracle_checked
              << ",\"oracle_mismatches\":" << result.oracle_mismatches
              << ",\"oracle_max_error\":" << result.oracle_max_error
              << ",\"callback_p99_ns\":" << result.callback_p99_ns
              << ",\"callback_max_ns\":" << result.callback_max_ns << "}\n";
    for (const auto& record : result.trace_records) {
        if (record.kind != pulp::gpu_audio::detail::SharedIoTraceKind::Terminal)
            continue;
        const auto duration_json = [](const SharedIoTraceRecord& value,
                                      pulp::gpu_audio::detail::SharedIoTraceStage begin,
                                      pulp::gpu_audio::detail::SharedIoTraceStage end) {
            const auto duration =
                pulp::gpu_audio::detail::shared_io_trace_duration(value, begin, end);
            std::string text = duration.available
                                   ? (std::string{"{\"availability\":\"available\",\"value_ns\":"} +
                                      std::to_string(duration.ns) + "}")
                                   : "{\"availability\":\"unavailable\"}";
            return text;
        };
        std::cout << "{\"schema\":\"pulp.gpu-audio.coreaudio-workgroup-ab.v1\","
                  << "\"record_kind\":\"block\",\"mode\":\"" << result.mode
                  << "\",\"sequence\":" << record.sequence
                  << ",\"gpu_timestamps\":{"
                     "\"availability\":\"unavailable\"},\"phases\":{\"scheduled_to_completion\":"
                  << duration_json(record, pulp::gpu_audio::detail::SharedIoTraceStage::Scheduled,
                                   pulp::gpu_audio::detail::SharedIoTraceStage::CompletionObserved)
                  << ",\"worker_entry_to_encode\":"
                  << duration_json(record, pulp::gpu_audio::detail::SharedIoTraceStage::WorkerEntry,
                                   pulp::gpu_audio::detail::SharedIoTraceStage::EncodeBegin)
                  << ",\"encode\":"
                  << duration_json(record, pulp::gpu_audio::detail::SharedIoTraceStage::EncodeBegin,
                                   pulp::gpu_audio::detail::SharedIoTraceStage::EncodeEnd)
                  << ",\"submit\":"
                  << duration_json(record, pulp::gpu_audio::detail::SharedIoTraceStage::SubmitBegin,
                                   pulp::gpu_audio::detail::SharedIoTraceStage::SubmitEnd)
                  << ",\"completion_observation\":"
                  << duration_json(record, pulp::gpu_audio::detail::SharedIoTraceStage::SubmitBegin,
                                   pulp::gpu_audio::detail::SharedIoTraceStage::CompletionObserved)
                  << "}}\n";
    }
}
} // namespace

int main() {
#if !defined(__APPLE__)
    return 77;
#else
    if (std::getenv("PULP_GPU_AUDIO_COREAUDIO_WORKGROUP_BENCHMARK") == nullptr) {
        std::cout << "{\"schema\":\"pulp.gpu-audio.coreaudio-workgroup-ab.v1\","
                     "\"status\":\"opt_in_required\"}\n";
        return 77;
    }
    const auto ir = make_ir();
    const auto input = make_input();
    const auto oracle = make_oracle(input, ir);
    const auto ordinary = run_mode(false, input, oracle, ir);
    const auto workgroup = run_mode(true, input, oracle, ir);
    emit(ordinary);
    emit(workgroup);
    if (!ordinary.started || !workgroup.started || !workgroup.callback_workgroup_available ||
        !workgroup.worker_joined || workgroup.worker_join_failures != 0 ||
        ordinary.callbacks < kBlocks || workgroup.callbacks < kBlocks ||
        ordinary.terminal_records == 0 || workgroup.terminal_records == 0 ||
        !ordinary.identity_valid || !workgroup.identity_valid || ordinary.oracle_mismatches != 0 ||
        workgroup.oracle_mismatches != 0)
        return 1;
    return 0;
#endif
}
