#include "detail/dawn_shared_io_provider.hpp"
#include "detail/shared_io_compute_plan.hpp"
#include "detail/shared_io_convolution_executor.hpp"
#include "support/dawn_transfer_call_counter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif

namespace {
using pulp::gpu_audio::detail::DawnSharedIoProvider;
using pulp::gpu_audio::detail::SharedIoArena;
using pulp::gpu_audio::detail::SharedIoComputePlan;
using pulp::gpu_audio::detail::SharedIoConvolutionExecutor;
using TransferSnapshot = pulp::test::DawnTransferCallCounter::Snapshot;

std::vector<float> spectrum(const std::vector<float>& time) {
    const auto n = static_cast<std::uint32_t>(time.size());
    std::vector<float> result(static_cast<std::size_t>(n) * 2u);
    constexpr float pi = 3.14159265358979323846f;
    for (std::uint32_t k = 0; k < n; ++k)
        for (std::uint32_t sample = 0; sample < n; ++sample) {
            const float angle = -2.0f * pi * static_cast<float>(k * sample) / static_cast<float>(n);
            result[2u * k] += time[sample] * std::cos(angle);
            result[2u * k + 1u] += time[sample] * std::sin(angle);
        }
    return result;
}

std::vector<float> direct_convolution(const std::vector<float>& input,
                                      const std::vector<float>& impulse) {
    const auto n = input.size();
    std::vector<float> output(n);
    for (std::size_t frame = 0; frame < n; ++frame)
        for (std::size_t tap = 0; tap <= frame; ++tap)
            output[frame] += input[frame - tap] * impulse[tap];
    return output;
}

std::vector<double> direct_full_tail(const std::vector<float>& input,
                                     const std::vector<float>& impulse) {
    std::vector<double> output(input.size() + impulse.size() - 1u, 0.0);
    for (std::size_t frame = 0; frame < output.size(); ++frame) {
        const auto first = frame >= input.size() ? frame - input.size() + 1u : 0u;
        const auto last = std::min(frame, impulse.size() - 1u);
        for (std::size_t tap = first; tap <= last; ++tap)
            output[frame] += static_cast<double>(input[frame - tap]) * impulse[tap];
    }
    return output;
}

bool zero_transfer(const TransferSnapshot& transfer) {
    return transfer.queue_write_buffer_calls == 0 && transfer.copy_buffer_to_buffer_calls == 0 &&
           transfer.buffer_map_async_calls == 0 && transfer.buffer_get_mapped_range_calls == 0 &&
           transfer.buffer_unmap_calls == 0;
}

struct CaseResult {
    bool passed = false;
    TransferSnapshot transfer;
    std::uint64_t submissions = 0;
    int stage = 0;
};

CaseResult run_case(std::uint32_t frames, std::uint32_t channels) {
    constexpr std::uint32_t slots = 2;
    const std::uint32_t n = frames * 2u;
    const std::size_t values = static_cast<std::size_t>(channels) * n * 2u;
    std::vector<float> impulse(n);
    constexpr std::uint32_t ir_length = 8;
    impulse[0] = 0.75f;
    impulse[1] = -0.25f;
    impulse[3] = 0.125f;
    impulse[7] = -0.0625f;
    const auto impulse_spectrum = spectrum(impulse);
    auto normalized_impulse_spectrum = impulse_spectrum;
    for (auto& value : normalized_impulse_spectrum)
        value /= static_cast<float>(n);

    pulp::test::DawnTransferCallCounter transfer_counter(
        pulp::test::DawnTransferCallCounter::InstallMode::DeferredSingleInstall);
    auto created = DawnSharedIoProvider::create(
        {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
         .proc_table_override_for_testing = transfer_counter.deferred_proc_table()});
    if (!created.provider)
        return {.stage = 1};
    SharedIoComputePlan plan;
    const auto bytes = values * sizeof(float);
    auto program = created.provider->make_convolution_program(
        {.fft_size = n,
         .channels = channels,
         .logical_frames = frames,
         .ir_length = ir_length,
         .normalized_ir_spectrum = normalized_impulse_spectrum});
    if (!program ||
        !plan.prepare(
            *created.provider,
            {.slots = slots, .input_bytes_per_slot = bytes, .output_bytes_per_slot = bytes},
            std::move(program)))
        return {.stage = 3};

    transfer_counter.reset();
    // Fill and submit every slot before servicing completion. This exercises
    // the prepared program's overlapping in-flight contract; writable FFT
    // scratch must be owned by the slot rather than shared by the plan.
    std::array<std::vector<std::vector<float>>, slots> inputs;
    for (std::uint32_t submission = 0; submission < slots; ++submission) {
        auto write = plan.acquire_input(submission + 1u, 0);
        if (!write || write->bytes.size() != bytes)
            return {.stage = 4};
        auto* complex = reinterpret_cast<float*>(write->bytes.data());
        inputs[submission].reserve(channels);
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            std::vector<float> input(n);
            for (std::uint32_t frame = 0; frame < n; ++frame) {
                input[frame] = frame < frames
                                   ? std::sin(static_cast<float>((channel + 1u) * (frame + 3u) +
                                                                 submission * 11u) *
                                              0.17f) +
                                         static_cast<float>(channel) * 0.125f +
                                         static_cast<float>(submission) * 0.03125f
                                   : 0.0f;
                const auto offset = (static_cast<std::size_t>(channel) * n + frame) * 2u;
                complex[offset] = input[frame];
                complex[offset + 1u] = 0.0f;
            }
            inputs[submission].push_back(std::move(input));
        }
        if (!plan.submit({write->token, 0}))
            return {.stage = 5};
    }

    std::vector<SharedIoComputePlan::Completion> completions;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (completions.size() < slots && std::chrono::steady_clock::now() < deadline) {
        plan.drain(0);
        while (auto completion = plan.pop_completion())
            completions.push_back(*completion);
    }
    if (completions.size() != slots)
        return {.stage = 6};
    bool oracle = true;
    for (const auto& completion : completions) {
        auto output = plan.acquire_output(completion);
        if (!output || output->bytes.size() != bytes)
            return {.stage = 7};
        const auto submission =
            static_cast<std::size_t>(completion.token.slot.stream_sequence - 1u);
        if (submission >= slots)
            return {.stage = 7};
        const auto* actual = reinterpret_cast<const float*>(output->bytes.data());
        for (std::uint32_t channel = 0; oracle && channel < channels; ++channel) {
            const auto expected = direct_convolution(inputs[submission][channel], impulse);
            for (std::uint32_t frame = 0; oracle && frame < n; ++frame) {
                const auto offset = (static_cast<std::size_t>(channel) * n + frame) * 2u;
                oracle = std::isfinite(actual[offset]) &&
                         std::abs(actual[offset] - expected[frame]) < 2.0e-3f &&
                         std::abs(actual[offset + 1u]) < 2.0e-3f;
            }
        }
        const auto token = output->token;
        output.reset();
        if (!plan.release_output({token}))
            return {.stage = 8};
    }
    if (!plan.release())
        return {.stage = 8};

    const auto transfer = transfer_counter.snapshot();
    return {oracle && zero_transfer(transfer), transfer, slots, oracle ? 0 : 9};
}

CaseResult run_stream_case(std::uint32_t frames, std::uint32_t channels) {
    constexpr std::uint32_t slots = 2;
    constexpr std::size_t input_frames = 384;
    constexpr std::size_t ir_length = 257;
    std::uint32_t fft_size = 1;
    while (fft_size < frames + ir_length - 1u)
        fft_size <<= 1u;
    const std::size_t values = static_cast<std::size_t>(channels) * fft_size * 2u;
    const std::size_t bytes = values * sizeof(float);

    std::vector<float> impulse(ir_length);
    for (std::size_t tap = 0; tap < impulse.size(); ++tap) {
        impulse[tap] = static_cast<float>(
            (0.19 * std::cos(0.031 * tap) - 0.07 * std::sin(0.113 * tap)) * std::exp(-0.009 * tap));
    }
    std::vector<float> padded_impulse(fft_size);
    std::copy(impulse.begin(), impulse.end(), padded_impulse.begin());
    auto impulse_spectrum = spectrum(padded_impulse);
    for (auto& value : impulse_spectrum)
        value /= static_cast<float>(fft_size);

    std::vector<std::vector<float>> inputs(channels, std::vector<float>(input_frames));
    std::vector<std::vector<double>> references(channels);
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        for (std::size_t frame = 0; frame < input_frames; ++frame) {
            inputs[channel][frame] =
                static_cast<float>(0.23 * std::sin(0.019 * frame + channel * 0.73) +
                                   0.11 * std::cos(0.083 * frame + channel * 0.31));
        }
        inputs[channel][0] += channel == 0u ? 0.71f : -0.43f;
        inputs[channel][97u + 19u * channel] += channel == 0u ? -0.37f : 0.59f;
        references[channel] = direct_full_tail(inputs[channel], impulse);
    }

    pulp::test::DawnTransferCallCounter transfer_counter(
        pulp::test::DawnTransferCallCounter::InstallMode::DeferredSingleInstall);
    auto created = DawnSharedIoProvider::create(
        {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
         .proc_table_override_for_testing = transfer_counter.deferred_proc_table()});
    if (!created.provider)
        return {.stage = 10};
    auto program = created.provider->make_convolution_program(
        {.fft_size = fft_size,
         .channels = channels,
         .logical_frames = frames,
         .ir_length = static_cast<std::uint32_t>(ir_length),
         .normalized_ir_spectrum = impulse_spectrum});
    SharedIoComputePlan plan;
    if (!program ||
        !plan.prepare(
            *created.provider,
            {.slots = slots, .input_bytes_per_slot = bytes, .output_bytes_per_slot = bytes},
            std::move(program))) {
        return {.stage = 11};
    }
    SharedIoConvolutionExecutor executor;
    if (!executor.prepare({.capacity = slots,
                           .channels = channels,
                           .block = frames,
                           .fft_size = fft_size,
                           .ir_length = static_cast<std::uint32_t>(ir_length)},
                          plan.preparation_epoch(), 0)) {
        return {.stage = 12};
    }

    transfer_counter.reset();
    const std::size_t output_frames = input_frames + ir_length - 1u;
    const std::size_t blocks = output_frames / frames;
    if (blocks * frames != output_frames)
        return {.stage = 13};
    std::vector<double> observed_carry(static_cast<std::size_t>(channels) * fft_size, 0.0);
    bool oracle = true;
    for (std::uint64_t sequence = 0; sequence < blocks; ++sequence) {
        auto write = plan.acquire_input(sequence, 0);
        if (!write || write->bytes.size() != bytes)
            return {.stage = 14};
        auto* complex = reinterpret_cast<float*>(write->bytes.data());
        std::fill_n(complex, values, 0.0f);
        const auto input_offset = static_cast<std::size_t>(sequence) * frames;
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                const auto source = input_offset + frame;
                if (source < input_frames) {
                    complex[(static_cast<std::size_t>(channel) * fft_size + frame) * 2u] =
                        inputs[channel][source];
                }
            }
        }
        if (!plan.submit({write->token, 0}))
            return {.stage = 15};

        std::optional<SharedIoComputePlan::Completion> completion;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!completion && std::chrono::steady_clock::now() < deadline) {
            plan.drain(0);
            completion = plan.pop_completion();
        }
        if (!completion || completion->status != SharedIoArena::CompletionStatus::RetiredSuccess) {
            return {.stage = 16};
        }
        auto output = plan.acquire_output(*completion);
        if (!output || output->bytes.size() != bytes)
            return {.stage = 17};
        const auto* time = reinterpret_cast<const float*>(output->bytes.data());
        // Validate the complete streamed output, including the executor's
        // intentionally suppressed recovery prefix. The shadow OLA is a probe
        // oracle only; product code continues to use the Dawn-free executor.
        const auto reference_offset = static_cast<std::size_t>(sequence) * frames;
        for (std::uint32_t channel = 0; oracle && channel < channels; ++channel) {
            auto* carry = observed_carry.data() + static_cast<std::size_t>(channel) * fft_size;
            const auto* block = time + static_cast<std::size_t>(channel) * fft_size * 2u;
            for (std::uint32_t frame = 0; frame < fft_size; ++frame)
                carry[frame] += block[frame * 2u];
            for (std::uint32_t frame = 0; oracle && frame < frames; ++frame) {
                const double expected = references[channel][reference_offset + frame];
                oracle = std::isfinite(carry[frame]) &&
                         std::abs(carry[frame] - expected) < 2.0e-2 * (1.0 + std::abs(expected));
            }
            std::move(carry + frames, carry + fft_size, carry);
            std::fill(carry + fft_size - frames, carry + fft_size, 0.0);
        }
        if (!executor.record_terminal(plan.preparation_epoch(), sequence,
                                      SharedIoConvolutionExecutor::Terminal::Success,
                                      {time, values})) {
            return {.stage = 18};
        }
        const auto output_token = output->token;
        output.reset();
        if (!plan.release_output({output_token}) || executor.collect() != 1)
            return {.stage = 19};

        auto ready = executor.take_ready(sequence);
        if (sequence < executor.valid_from_sequence()) {
            if (!ready.empty())
                return {.stage = 20};
            continue;
        }
        if (ready.size() != static_cast<std::size_t>(channels) * frames)
            return {.stage = 21};
        for (std::uint32_t channel = 0; oracle && channel < channels; ++channel) {
            for (std::uint32_t frame = 0; oracle && frame < frames; ++frame) {
                const double expected = references[channel][reference_offset + frame];
                const double actual = ready[static_cast<std::size_t>(channel) * frames + frame];
                oracle = std::isfinite(actual) &&
                         std::abs(actual - expected) < 2.0e-2 * (1.0 + std::abs(expected));
            }
        }
        if (!executor.release_ready(sequence))
            return {.stage = 22};
    }
    if (!plan.release())
        return {.stage = 23};
    const auto transfer = transfer_counter.snapshot();
    return {oracle && zero_transfer(transfer), transfer, blocks, oracle ? 0 : 24};
}

bool run_fault_scenario(std::string_view scenario) {
    const auto fault = scenario == "prepare-scope-failure"
                           ? DawnSharedIoProvider::Fault::ConvolutionPrepareAfterScopesFailure
                           : DawnSharedIoProvider::Fault::ConvolutionSubmitAfterScopesFailure;
    pulp::test::DawnTransferCallCounter transfer_counter(
        pulp::test::DawnTransferCallCounter::InstallMode::DeferredSingleInstall);
    auto created = DawnSharedIoProvider::create(
        {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
         .proc_table_override_for_testing = transfer_counter.deferred_proc_table(),
         .fault = fault});
    if (!created.provider)
        return false;

    constexpr std::uint32_t frames = 4;
    constexpr std::uint32_t fft_size = frames * 2u;
    std::vector<float> impulse(fft_size);
    impulse[0] = 1.0f;
    auto impulse_spectrum = spectrum(impulse);
    for (auto& value : impulse_spectrum)
        value /= static_cast<float>(fft_size);

    SharedIoComputePlan plan;
    auto program =
        created.provider->make_convolution_program({.fft_size = fft_size,
                                                    .channels = 1,
                                                    .logical_frames = frames,
                                                    .ir_length = 1,
                                                    .normalized_ir_spectrum = impulse_spectrum});
    if (!program)
        return false;
    const bool prepared = plan.prepare(*created.provider,
                                       {.slots = 1,
                                        .input_bytes_per_slot = fft_size * 2u * sizeof(float),
                                        .output_bytes_per_slot = fft_size * 2u * sizeof(float)},
                                       std::move(program));
    if (scenario == "prepare-scope-failure")
        return !prepared && !plan.prepared() && created.provider->stats().fault_injections == 1;
    if (!prepared)
        return false;
    auto input = plan.acquire_input(1, 0);
    return input && !plan.submit({input->token, 0}) && plan.release() &&
           created.provider->stats().fault_injections == 1;
}

} // namespace

int main(int argc, char** argv) {
    std::string_view scenario = "baseline";
    if (argc == 2 && std::string_view(argv[1]).starts_with("--scenario="))
        scenario = std::string_view(argv[1]).substr(std::string_view("--scenario=").size());
    else if (argc != 1)
        return 1;
    if (scenario != "baseline") {
        if (scenario != "prepare-scope-failure" && scenario != "submit-scope-failure")
            return 1;
        const bool passed = run_fault_scenario(scenario);
        std::cout
            << R"json({"schema":"pulp.gpu-audio.shared-io.private-convolution-fault.v1","scenario":")json"
            << scenario << R"json(","status":")json" << (passed ? "passed" : "failed") << "\"}"
            << std::endl;
        return passed ? 0 : 1;
    }

    const std::array<std::uint32_t, 3> frames = {32, 64, 128};
    std::uint64_t submissions = 0, write_buffer = 0, copy_buffer = 0, map_async = 0,
                  mapped_range = 0, unmap = 0;
    int first_failure = 0;
    bool passed = true;
    for (const auto frame_count : frames)
        for (const std::uint32_t channels : {1u, 2u}) {
            for (const auto result :
                 {run_case(frame_count, channels), run_stream_case(frame_count, channels)}) {
                passed = passed && result.passed;
                if (!result.passed && first_failure == 0)
                    first_failure = result.stage;
                submissions += result.submissions;
                write_buffer += result.transfer.queue_write_buffer_calls;
                copy_buffer += result.transfer.copy_buffer_to_buffer_calls;
                map_async += result.transfer.buffer_map_async_calls;
                mapped_range += result.transfer.buffer_get_mapped_range_calls;
                unmap += result.transfer.buffer_unmap_calls;
            }
        }
    std::cout << R"json({"schema":"pulp.gpu-audio.shared-io.private-convolution.v1","status":")json"
              << (passed ? "passed" : "failed") << R"json(","cases":12,"submissions":)json"
              << submissions << R"json(,"write_buffer_calls":)json" << write_buffer
              << R"json(,"copy_buffer_to_buffer_calls":)json" << copy_buffer
              << R"json(,"map_async_calls":)json" << map_async
              << R"json(,"get_mapped_range_calls":)json" << mapped_range
              << R"json(,"unmap_calls":)json" << unmap << R"json(,"first_failure_stage":)json"
              << first_failure << "}" << std::endl;
    return passed ? 0 : 1;
}
