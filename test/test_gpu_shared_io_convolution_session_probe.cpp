#include "detail/dawn_shared_io_convolution_session.hpp"
#include "detail/shared_io_stamped_bridge.hpp"
#include "support/dawn_transfer_call_counter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif

namespace {
using pulp::gpu_audio::detail::DawnSharedIoConvolutionSessionOptions;
using pulp::gpu_audio::detail::DawnSharedIoProvider;
using pulp::gpu_audio::detail::SharedIoConvolutionSession;
using pulp::gpu_audio::detail::SharedIoStampedBridge;
using TransferSnapshot = pulp::test::DawnTransferCallCounter::Snapshot;

std::vector<float> spectrum(std::span<const float> time) {
    const auto size = static_cast<std::uint32_t>(time.size());
    std::vector<float> result(static_cast<std::size_t>(size) * 2u);
    constexpr float pi = 3.14159265358979323846f;
    for (std::uint32_t bin = 0; bin < size; ++bin)
        for (std::uint32_t frame = 0; frame < size; ++frame) {
            const float angle =
                -2.0f * pi * static_cast<float>(bin * frame) / static_cast<float>(size);
            result[2u * bin] += time[frame] * std::cos(angle);
            result[2u * bin + 1u] += time[frame] * std::sin(angle);
        }
    return result;
}

std::vector<double> direct_convolution(std::span<const float> input,
                                       std::span<const float> impulse) {
    std::vector<double> output(input.size() + impulse.size() - 1u, 0.0);
    for (std::size_t frame = 0; frame < output.size(); ++frame) {
        const auto first = frame >= input.size() ? frame - input.size() + 1u : 0u;
        const auto last = std::min(frame, impulse.size() - 1u);
        for (std::size_t tap = first; tap <= last; ++tap)
            output[frame] += static_cast<double>(input[frame - tap]) * impulse[tap];
    }
    return output;
}

bool zero_transfer(const TransferSnapshot& value) {
    return value.queue_write_buffer_calls == 0 && value.queue_write_buffer_bytes == 0 &&
           value.copy_buffer_to_buffer_calls == 0 && value.copy_buffer_to_buffer_bytes == 0 &&
           value.buffer_map_async_calls == 0 && value.buffer_map_async_bytes == 0 &&
           value.buffer_get_mapped_range_calls == 0 && value.buffer_get_mapped_range_bytes == 0 &&
           value.buffer_unmap_calls == 0;
}

bool same_snapshot(const TransferSnapshot& lhs, const TransferSnapshot& rhs) {
    return lhs.queue_write_buffer_calls == rhs.queue_write_buffer_calls &&
           lhs.queue_write_buffer_bytes == rhs.queue_write_buffer_bytes &&
           lhs.copy_buffer_to_buffer_calls == rhs.copy_buffer_to_buffer_calls &&
           lhs.copy_buffer_to_buffer_bytes == rhs.copy_buffer_to_buffer_bytes &&
           lhs.buffer_map_async_calls == rhs.buffer_map_async_calls &&
           lhs.buffer_map_async_bytes == rhs.buffer_map_async_bytes &&
           lhs.buffer_get_mapped_range_calls == rhs.buffer_get_mapped_range_calls &&
           lhs.buffer_get_mapped_range_bytes == rhs.buffer_get_mapped_range_bytes &&
           lhs.buffer_unmap_calls == rhs.buffer_unmap_calls &&
           lhs.queue_submit_calls == rhs.queue_submit_calls &&
           lhs.submitted_command_buffers == rhs.submitted_command_buffers;
}

bool service_batch(SharedIoConvolutionSession& session, std::size_t expected_completions,
                   std::uint64_t& now_ns, std::uint64_t& submissions) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::size_t completions = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto serviced = session.service(now_ns++);
        submissions += serviced.submitted;
        completions += serviced.completions;
        if (serviced.fenced)
            return false;
        if (completions >= expected_completions)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

int run_baseline() {
    constexpr std::uint32_t channels = 2;
    constexpr std::uint32_t frames = 32;
    constexpr std::uint32_t fft_size = 64;
    constexpr std::uint32_t slots = 2;
    constexpr std::uint32_t input_blocks = 4;
    constexpr std::uint32_t callbacks = 7;
    constexpr std::uint32_t ir_length = 8;

    std::vector<float> impulse(fft_size);
    impulse[0] = 0.75f;
    impulse[1] = -0.25f;
    impulse[3] = 0.125f;
    impulse[7] = -0.0625f;
    auto normalized_spectrum = spectrum(impulse);
    for (auto& value : normalized_spectrum)
        value /= static_cast<float>(fft_size);

    std::array<std::vector<float>, channels> input;
    std::array<std::vector<double>, channels> reference;
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        input[channel].resize(static_cast<std::size_t>(input_blocks) * frames);
        for (std::size_t frame = 0; frame < input[channel].size(); ++frame)
            input[channel][frame] =
                static_cast<float>(0.23 * std::sin(0.019 * frame + channel * 0.73) +
                                   0.11 * std::cos(0.083 * frame + channel * 0.31));
        input[channel][0] += channel == 0 ? 0.71f : -0.43f;
        input[channel][97u + 19u * channel] += channel == 0 ? -0.37f : 0.59f;
        reference[channel] = direct_convolution(input[channel], impulse);
    }

    // The counter exists before any Dawn object and outlives the session. The
    // factory gets its deferred table once; it owns the sole provider and its
    // prepared program until session release.
    pulp::test::DawnTransferCallCounter counter(
        pulp::test::DawnTransferCallCounter::InstallMode::DeferredSingleInstall);
    auto created = pulp::gpu_audio::detail::create_dawn_shared_io_convolution_session(
        {.provider = {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
                      .proc_table_override_for_testing = counter.deferred_proc_table()},
         .session = {.pipeline = {.capacity = 8,
                                  .channels = channels,
                                  .block_size = frames,
                                  .fft_size = fft_size,
                                  .ir_length = ir_length},
                     .slots = slots},
         .normalized_ir_spectrum = normalized_spectrum});
    if (!created.session || created.availability != DawnSharedIoProvider::Availability::Ready ||
        created.reason !=
            pulp::gpu_audio::detail::DawnSharedIoConvolutionSessionCreateResult::Reason::Ready)
        return 1;

    counter.reset();
    std::vector<float> callback_input(static_cast<std::size_t>(channels) * frames);
    std::vector<float> callback_output(callback_input.size());
    std::uint64_t now_ns = 1;
    std::uint64_t submissions = 0;
    for (std::uint64_t batch = 0; batch < callbacks; batch += slots) {
        const auto count =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(slots, callbacks - batch));
        for (std::uint64_t sequence = batch; sequence < batch + count; ++sequence) {
            std::fill(callback_input.begin(), callback_input.end(), 0.f);
            for (std::uint32_t channel = 0; channel < channels; ++channel)
                for (std::uint32_t frame = 0; frame < frames; ++frame) {
                    const auto source = sequence * frames + frame;
                    if (source < input[channel].size())
                        callback_input[static_cast<std::size_t>(channel) * frames + frame] =
                            input[channel][source];
                }
            const auto before_callback = counter.snapshot();
            const auto callback = created.session->begin_callback(callback_input);
            if (!callback.valid() || callback.stamp.sequence != sequence)
                return 2;
            const auto delivery = created.session->consume_output(callback, callback_output);
            // The callback functions are the only operations between these
            // snapshots. They must not enter any Dawn transfer or submission API;
            // the delta check continues to hold after earlier service calls submitted work.
            const auto after_callback = counter.snapshot();
            if (!zero_transfer(after_callback) || !same_snapshot(before_callback, after_callback))
                return 3;
            if (sequence < SharedIoStampedBridge::kLeadBlocks) {
                if (delivery != SharedIoConvolutionSession::Delivery::Priming)
                    return 4;
            } else {
                const auto produced = sequence - SharedIoStampedBridge::kLeadBlocks;
                if (produced == 0) {
                    if (delivery != SharedIoConvolutionSession::Delivery::Missing)
                        return 5;
                } else {
                    if (delivery != SharedIoConvolutionSession::Delivery::Ready)
                        return 6;
                    const auto offset = static_cast<std::size_t>(produced) * frames;
                    for (std::uint32_t channel = 0; channel < channels; ++channel)
                        for (std::uint32_t frame = 0; frame < frames; ++frame) {
                            const auto expected = offset + frame < reference[channel].size()
                                                      ? reference[channel][offset + frame]
                                                      : 0.0;
                            const auto actual =
                                callback_output[static_cast<std::size_t>(channel) * frames + frame];
                            if (!std::isfinite(actual) ||
                                std::abs(actual - expected) > 2.0e-2 * (1.0 + std::abs(expected)))
                                return 7;
                        }
                }
            }
        }
        // Two callback-owned ingress records are published before this call.
        // The session must claim both physical slots before it may poll a
        // completion, proving the factory path preserves overlapping in-flight
        // provider work rather than serializing every block.
        if (!service_batch(*created.session, count, now_ns, submissions))
            return 8;
    }
    // The physical fence stops the real provider. Reopening requires a new
    // preparation transaction; keep the existing session CPU-only.
    const auto high_water_in_flight = created.session->telemetry().high_water_in_flight;
    if (submissions != callbacks || high_water_in_flight < slots ||
        !created.session->fence_for_offline())
        return 9;
    const auto fenced_callback = created.session->begin_callback(callback_input);
    if (!fenced_callback.valid() || fenced_callback.stamp.epoch != 0 ||
        created.session->consume_output(fenced_callback, callback_output) !=
            SharedIoConvolutionSession::Delivery::EpochChanged ||
        created.session->service(++now_ns).submitted != 0 || created.session->fence_and_reprime() ||
        !created.session->release())
        return 9;
    created.session.reset();
    const auto transfer = counter.snapshot();
    if (!zero_transfer(transfer) || transfer.queue_submit_calls != submissions ||
        transfer.submitted_command_buffers != submissions)
        return 10;
    std::cout
        << R"json({"schema":"pulp.gpu-audio.shared-io.convolution-session.v1","scenario":"baseline","status":"passed","submissions":)json"
        << submissions << R"json(,"queue_submit_calls":)json" << transfer.queue_submit_calls
        << R"json(,"high_water_in_flight":)json" << high_water_in_flight
        << R"json(,"write_buffer_calls":)json" << transfer.queue_write_buffer_calls
        << R"json(,"write_buffer_bytes":)json" << transfer.queue_write_buffer_bytes
        << R"json(,"copy_buffer_to_buffer_calls":)json" << transfer.copy_buffer_to_buffer_calls
        << R"json(,"copy_buffer_to_buffer_bytes":)json" << transfer.copy_buffer_to_buffer_bytes
        << R"json(,"map_async_calls":)json" << transfer.buffer_map_async_calls
        << R"json(,"map_async_bytes":)json" << transfer.buffer_map_async_bytes
        << R"json(,"get_mapped_range_calls":)json" << transfer.buffer_get_mapped_range_calls
        << R"json(,"get_mapped_range_bytes":)json" << transfer.buffer_get_mapped_range_bytes
        << R"json(,"unmap_calls":)json" << transfer.buffer_unmap_calls << "}\n";
    return 0;
}

int run_factory_prepare_failure() {
    constexpr std::uint32_t frames = 4;
    std::vector<float> impulse(frames * 2u, 0.f);
    impulse[0] = 1.f;
    auto normalized_spectrum = spectrum(impulse);
    for (auto& value : normalized_spectrum)
        value /= static_cast<float>(impulse.size());
    pulp::test::DawnTransferCallCounter counter(
        pulp::test::DawnTransferCallCounter::InstallMode::DeferredSingleInstall);
    auto created = pulp::gpu_audio::detail::create_dawn_shared_io_convolution_session(
        {.provider = {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
                      .proc_table_override_for_testing = counter.deferred_proc_table(),
                      .fault = DawnSharedIoProvider::Fault::ConvolutionPrepareAfterScopesFailure},
         .session = {.pipeline = {.capacity = 2,
                                  .channels = 1,
                                  .block_size = frames,
                                  .fft_size = frames * 2u,
                                  .ir_length = 1},
                     .slots = 1},
         .normalized_ir_spectrum = normalized_spectrum});
    const bool passed =
        created.session && created.availability == DawnSharedIoProvider::Availability::Failed &&
        created.reason == pulp::gpu_audio::detail::DawnSharedIoConvolutionSessionCreateResult::
                              Reason::SessionPreparationFailed &&
        created.session->release();
    std::cout
        << R"json({"schema":"pulp.gpu-audio.shared-io.convolution-session.v1","scenario":"prepare-scope-failure","status":")json"
        << (passed ? "passed" : "failed") << "\"}\n";
    return passed ? 0 : 1;
}

int run_submit_failure() {
    constexpr std::uint32_t frames = 4;
    std::vector<float> impulse(frames * 2u, 0.f);
    impulse[0] = 1.f;
    auto normalized_spectrum = spectrum(impulse);
    for (auto& value : normalized_spectrum)
        value /= static_cast<float>(impulse.size());
    pulp::test::DawnTransferCallCounter counter(
        pulp::test::DawnTransferCallCounter::InstallMode::DeferredSingleInstall);
    auto created = pulp::gpu_audio::detail::create_dawn_shared_io_convolution_session(
        {.provider = {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
                      .proc_table_override_for_testing = counter.deferred_proc_table(),
                      .fault = DawnSharedIoProvider::Fault::ConvolutionSubmitAfterScopesFailure},
         .session = {.pipeline = {.capacity = 2,
                                  .channels = 1,
                                  .block_size = frames,
                                  .fft_size = frames * 2u,
                                  .ir_length = 1},
                     .slots = 1},
         .normalized_ir_spectrum = normalized_spectrum});
    if (!created.session)
        return 1;
    counter.reset();
    const std::array<float, frames> input{0.1f, -0.2f, 0.3f, -0.4f};
    std::array<float, frames> output{};
    const auto callback = created.session->begin_callback(input);
    const auto delivery = created.session->consume_output(callback, output);
    const bool callback_clean = callback.valid() &&
                                delivery == SharedIoConvolutionSession::Delivery::Priming &&
                                zero_transfer(counter.snapshot());
    const auto serviced = created.session->service(1);
    const bool passed = callback_clean && serviced.refused == 1 && serviced.terminal_records == 1 &&
                        serviced.fenced && created.session->fenced() && created.session->release();
    std::cout
        << R"json({"schema":"pulp.gpu-audio.shared-io.convolution-session.v1","scenario":"submit-scope-failure","status":")json"
        << (passed ? "passed" : "failed") << "\"}\n";
    return passed ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view scenario =
        argc == 2 && std::string_view(argv[1]).starts_with("--scenario=")
            ? std::string_view(argv[1]).substr(11)
        : argc == 1 ? "baseline"
                    : "invalid";
    if (scenario == "baseline")
        return run_baseline();
    if (scenario == "prepare-scope-failure")
        return run_factory_prepare_failure();
    if (scenario == "submit-scope-failure")
        return run_submit_failure();
    return 1;
}
