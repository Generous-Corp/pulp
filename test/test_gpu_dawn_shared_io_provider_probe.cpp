#include "detail/dawn_shared_io_provider.hpp"
#include "detail/shared_io_arena.hpp"
#include "support/dawn_transfer_call_counter.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <sys/sysctl.h>
#include <sys/utsname.h>

#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif

using pulp::gpu_audio::detail::DawnSharedIoProvider;
using pulp::gpu_audio::detail::SharedIoArena;

namespace {

constexpr auto kMaxCompletionWaitNs =
    static_cast<std::uint64_t>(std::chrono::nanoseconds::max().count());

using Clock = std::chrono::steady_clock;
enum class TransferControl : std::uint8_t { None, WriteBuffer, CopyBuffer, MapAsync };

struct Scenario {
    std::string_view name = "baseline";
    DawnSharedIoProvider::Fault fault = DawnSharedIoProvider::Fault::None;
    bool expect_prepare_failure = false;
    bool expect_submit_refusal = false;
    bool expect_poisoned_retirement = false;
    bool expect_expired_late_retirement = false;
    bool expect_oracle_mismatch = false;
    bool watchdog_hang = false;
    TransferControl transfer_control = TransferControl::None;
};

std::string json_escape(std::string_view value) {
    std::string result;
    for (const char ch : value) {
        if (ch == '\\' || ch == '"')
            result.push_back('\\');
        result.push_back(ch);
    }
    return result;
}

std::string hardware_model() {
    std::size_t size = 0;
    if (sysctlbyname("hw.model", nullptr, &size, nullptr, 0) != 0 || size == 0)
        return "unknown";
    std::string value(size, '\0');
    if (sysctlbyname("hw.model", value.data(), &size, nullptr, 0) != 0)
        return "unknown";
    if (!value.empty() && value.back() == '\0')
        value.pop_back();
    return value;
}

std::optional<Scenario> parse_scenario(std::string_view value) {
    if (value == "baseline")
        return Scenario{};
    if (value == "wrong-output")
        return Scenario{value, DawnSharedIoProvider::Fault::WrongOutput, false, false, false, false,
                        true};
    if (value == "refuse-allocation")
        return Scenario{value, DawnSharedIoProvider::Fault::RefuseAllocation, true};
    if (value == "refuse-input-import")
        return Scenario{value, DawnSharedIoProvider::Fault::RefuseInputImport, true};
    if (value == "refuse-output-import")
        return Scenario{value, DawnSharedIoProvider::Fault::RefuseOutputImport, true};
    if (value == "reject-before-submit")
        return Scenario{value, DawnSharedIoProvider::Fault::RejectBeforeSubmit, false, true};
    if (value == "poison-after-submit")
        return Scenario{value, DawnSharedIoProvider::Fault::PoisonAfterSubmit, false, false, true};
    if (value == "delay-completion")
        return Scenario{value, DawnSharedIoProvider::Fault::DelayCompletion, false, false, false,
                        true};
    if (value == "watchdog-hang")
        return Scenario{
            value, DawnSharedIoProvider::Fault::DelayCompletion, false, false, false, false, false,
            true};
    if (value == "plant-write-buffer") {
        Scenario result{value, DawnSharedIoProvider::Fault::PlantWriteBuffer};
        result.transfer_control = TransferControl::WriteBuffer;
        return result;
    }
    if (value == "plant-copy-buffer") {
        Scenario result{value, DawnSharedIoProvider::Fault::PlantCopyBuffer};
        result.transfer_control = TransferControl::CopyBuffer;
        return result;
    }
    if (value == "plant-map-async") {
        Scenario result{value, DawnSharedIoProvider::Fault::PlantMapAsync};
        result.transfer_control = TransferControl::MapAsync;
        return result;
    }
    if (value == "invalid-command-after-submit")
        return Scenario{value, DawnSharedIoProvider::Fault::InvalidCommandAfterSubmit, false, false,
                        true};
    if (value == "synthetic-queue-error")
        return Scenario{value, DawnSharedIoProvider::Fault::SyntheticQueueError, false, false,
                        true};
    if (value == "synthetic-queue-cancel")
        return Scenario{value, DawnSharedIoProvider::Fault::SyntheticQueueCancelled, false, false,
                        true};
    if (value == "force-loss-before-submit")
        return Scenario{value, DawnSharedIoProvider::Fault::ForceLossBeforeSubmit, false, false,
                        true};
    if (value == "force-loss-between-submit-and-registration")
        return Scenario{
            value, DawnSharedIoProvider::Fault::ForceLossBetweenSubmitAndCompletionRegistration,
            false, false, true};
    if (value == "force-loss-after-registration")
        return Scenario{value, DawnSharedIoProvider::Fault::ForceLossAfterCompletionRegistration,
                        false, false, true};
    if (value == "terminal-busy-retry")
        return Scenario{value, DawnSharedIoProvider::Fault::HoldTerminalBusy};
    if (value == "real-device-repetition")
        return Scenario{value};
    if (value == "native-input-oom")
        return Scenario{value, DawnSharedIoProvider::Fault::NativeInputOom, true};
    if (value == "native-output-oom")
        return Scenario{value, DawnSharedIoProvider::Fault::NativeOutputOom, true};
    return std::nullopt;
}

std::optional<DawnSharedIoProvider::CompletionPolicy>
parse_completion_policy(std::string_view value) {
    if (value == "process-events")
        return DawnSharedIoProvider::CompletionPolicy::ProcessEvents;
    if (value == "wait-any")
        return DawnSharedIoProvider::CompletionPolicy::WaitAny;
    if (value == "timed-wait-any")
        return DawnSharedIoProvider::CompletionPolicy::TimedWaitAny;
    return std::nullopt;
}

std::string_view completion_policy_name(DawnSharedIoProvider::CompletionPolicy policy) {
    switch (policy) {
        case DawnSharedIoProvider::CompletionPolicy::ProcessEvents: return "process-events";
        case DawnSharedIoProvider::CompletionPolicy::WaitAny: return "wait-any";
        case DawnSharedIoProvider::CompletionPolicy::TimedWaitAny: return "timed-wait-any";
    }
    return "unknown";
}

void emit(std::string_view scenario, std::string_view status, std::string_view reason,
          DawnSharedIoProvider::CompletionPolicy completion_policy, std::optional<bool> oracle,
          std::uint32_t alignment, std::uint64_t installs,
          const pulp::test::DawnTransferCallCounter::Snapshot& transfers, std::uint64_t submissions,
          const DawnSharedIoProvider::Stats& stats,
          const DawnSharedIoProvider::AdapterIdentity& adapter) {
    struct utsname system_info{};
    const bool have_system_info = uname(&system_info) == 0;
    std::cout << "{\"schema\":\"pulp.gpu-dawn-shared-io-provider.v2\","
              << "\"scenario\":\"" << scenario << "\",\"status\":\"" << status << "\",\"reason\":\""
              << reason << "\",\"completion_policy\":\""
              << completion_policy_name(completion_policy) << "\",\"oracle\":";
    if (oracle)
        std::cout << (*oracle ? "true" : "false");
    else
        std::cout << "null";
    std::cout << ",\"alignment\":" << alignment << ",\"proc_table_installs\":" << installs
              << ",\"queue_submit_calls\":" << transfers.queue_submit_calls
              << ",\"submitted_command_buffers\":" << transfers.submitted_command_buffers
              << ",\"write_buffer_calls\":" << transfers.queue_write_buffer_calls
              << ",\"write_buffer_bytes\":" << transfers.queue_write_buffer_bytes
              << ",\"copy_buffer_to_buffer_calls\":" << transfers.copy_buffer_to_buffer_calls
              << ",\"copy_buffer_to_buffer_bytes\":" << transfers.copy_buffer_to_buffer_bytes
              << ",\"map_async_calls\":" << transfers.buffer_map_async_calls
              << ",\"map_async_bytes\":" << transfers.buffer_map_async_bytes
              << ",\"expected_submissions\":" << submissions
              << ",\"slots_created\":" << stats.slots_created
              << ",\"slots_destroyed\":" << stats.slots_destroyed
              << ",\"allocations\":" << stats.allocations
              << ",\"import_attempts\":" << stats.import_attempts
              << ",\"import_successes\":" << stats.import_successes
              << ",\"retired_success\":" << stats.retired_success
              << ",\"retired_failure\":" << stats.retired_failure
              << ",\"disposals_observed\":" << stats.disposals_observed
              << ",\"host_frees\":" << stats.host_frees << ",\"drain_calls\":" << stats.drain_calls
              << ",\"failed_drains\":" << stats.failed_drains
              << ",\"terminal_busy_retries\":" << stats.terminal_busy_retries
              << ",\"process_events_calls\":" << stats.process_events_calls
              << ",\"wait_any_calls\":" << stats.wait_any_calls
              << ",\"wait_any_timeouts\":" << stats.wait_any_timeouts
              << ",\"wait_any_unsupported\":" << stats.wait_any_unsupported
              << ",\"fault_injections\":" << stats.fault_injections << ",\"hardware_model\":\""
              << json_escape(hardware_model()) << "\""
              << ",\"os\":\"" << json_escape(have_system_info ? system_info.sysname : "unknown")
              << "\""
              << ",\"os_release\":\""
              << json_escape(have_system_info ? system_info.release : "unknown") << "\""
              << ",\"architecture\":\""
              << json_escape(have_system_info ? system_info.machine : "unknown") << "\""
              << ",\"adapter_name\":\"" << json_escape(adapter.name) << "\""
              << ",\"adapter_vendor_id\":" << adapter.vendor_id
              << ",\"adapter_device_id\":" << adapter.device_id << ",\"adapter_backend\":\"metal\""
              << ",\"render_concurrency\":\"not_exercised\"}\n";
}

bool transfer_oracle(const pulp::test::DawnTransferCallCounter::Snapshot& transfers,
                     std::uint64_t submissions) {
    return transfers.queue_write_buffer_calls == 0 && transfers.queue_write_buffer_bytes == 0 &&
           transfers.copy_buffer_to_buffer_calls == 0 &&
           transfers.copy_buffer_to_buffer_bytes == 0 && transfers.buffer_map_async_calls == 0 &&
           transfers.buffer_map_async_bytes == 0 && transfers.queue_submit_calls == submissions &&
           transfers.submitted_command_buffers == submissions;
}

bool transfer_control_oracle(const pulp::test::DawnTransferCallCounter::Snapshot& transfers,
                             std::uint64_t submissions, TransferControl control) {
    if (transfers.queue_submit_calls != submissions ||
        transfers.submitted_command_buffers != submissions) {
        return false;
    }
    const bool write = transfers.queue_write_buffer_calls == 1 &&
                       transfers.queue_write_buffer_bytes == sizeof(float);
    const bool copy = transfers.copy_buffer_to_buffer_calls == 1 &&
                      transfers.copy_buffer_to_buffer_bytes == sizeof(float);
    const bool map =
        transfers.buffer_map_async_calls == 1 && transfers.buffer_map_async_bytes == sizeof(float);
    if (control == TransferControl::WriteBuffer)
        return write && transfers.copy_buffer_to_buffer_calls == 0 &&
               transfers.buffer_map_async_calls == 0;
    if (control == TransferControl::CopyBuffer)
        return copy && transfers.queue_write_buffer_calls == 0 &&
               transfers.buffer_map_async_calls == 0;
    if (control == TransferControl::MapAsync)
        return map && transfers.queue_write_buffer_calls == 0 &&
               transfers.copy_buffer_to_buffer_calls == 0;
    return false;
}

bool wait_for_output(SharedIoArena& arena, std::uint64_t sequence,
                     std::optional<SharedIoArena::OutputLease>& output,
                     std::chrono::milliseconds limit) {
    const auto deadline = Clock::now() + limit;
    while (!output && Clock::now() < deadline) {
        arena.drain_completions();
        output = arena.acquire_output(arena.preparation_epoch(), sequence);
        if (!output)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return output.has_value();
}

} // namespace

int main(int argc, char** argv) {
    bool strict = false;
    bool verify_completion_wait_bound = false;
    std::string_view scenario_name = "baseline";
    auto completion_policy = DawnSharedIoProvider::CompletionPolicy::ProcessEvents;
    std::uint64_t completion_wait_ns = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--strict")
            strict = true;
        else if (argument == "--verify-completion-wait-bound")
            verify_completion_wait_bound = true;
        else if (argument.starts_with("--scenario="))
            scenario_name = argument.substr(std::string_view("--scenario=").size());
        else if (argument.starts_with("--completion-policy=")) {
            const auto parsed = parse_completion_policy(
                argument.substr(std::string_view("--completion-policy=").size()));
            if (!parsed)
                return 1;
            completion_policy = *parsed;
        } else if (argument.starts_with("--completion-wait-ns=")) {
            try {
                completion_wait_ns = std::stoull(std::string(
                    argument.substr(std::string_view("--completion-wait-ns=").size())));
            } catch (...) {
                return 1;
            }
        }
        else
            return 1;
    }
    if (verify_completion_wait_bound) {
        const auto invalid_wait_ns = kMaxCompletionWaitNs + 1;
        const auto rejected = DawnSharedIoProvider::create(
            {.completion_wait_ns = invalid_wait_ns});
        return !rejected.provider &&
                       rejected.reason == "completion_wait_ns_out_of_range"
                   ? 0
                   : 1;
    }
    const auto scenario = parse_scenario(scenario_name);
    if (!scenario)
        return 1;

    // Declared first so the provider and every Dawn wrapper are destroyed
    // before the test-only proc-table interposer restores the native table.
    std::optional<pulp::test::DawnTransferCallCounter> transfer_counter;
    transfer_counter.emplace(
        pulp::test::DawnTransferCallCounter::InstallMode::DeferredSingleInstall);
    auto created = DawnSharedIoProvider::create(
        {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
         .proc_table_override_for_testing = transfer_counter->deferred_proc_table(),
         .fault = scenario->fault,
         .completion_policy = completion_policy,
         .completion_wait_ns = completion_wait_ns});
    if (!created.provider) {
        std::cout << "{\"schema\":\"pulp.gpu-dawn-shared-io-provider.v2\","
                  << "\"scenario\":\"" << scenario->name
                  << "\",\"status\":\"unavailable\",\"reason\":\"" << created.reason
                  << "\",\"completion_policy\":\""
                  << completion_policy_name(completion_policy) << "\"}\n";
        return strict ? 1 : 77;
    }
    transfer_counter->reset();
    const auto alignment = created.provider->alignment();
    const auto installs = created.provider->proc_table_install_count();
    const auto adapter = created.provider->adapter_identity();

    bool passed = true;
    std::optional<bool> oracle;
    std::string_view reason = "shared_io_oracle";
    std::uint64_t submissions = 0;
    struct BufferSizes {
        std::size_t input;
        std::size_t output;
    };
    const BufferSizes sizes[] = {{4, 4}, {256, 256}, {4096, 4096}, {4, 4096}, {4096, 4}};
    const std::size_t size_count = scenario->name == "baseline"                 ? std::size(sizes)
                                   : scenario->name == "real-device-repetition" ? 64
                                                                                : 1;

    for (std::size_t size_index = 0; passed && size_index < size_count; ++size_index) {
        const auto& buffer_size = sizes[size_index % std::size(sizes)];
        SharedIoArena arena;
        const bool prepared =
            arena.prepare(*created.provider, {.slots = 2,
                                              .input_bytes_per_slot = buffer_size.input,
                                              .output_bytes_per_slot = buffer_size.output});
        if (scenario->expect_prepare_failure) {
            passed = !prepared && !arena.prepared();
            if (passed) {
                SharedIoArena retry;
                passed =
                    retry.prepare(*created.provider, {.slots = 1,
                                                      .input_bytes_per_slot = sizeof(float),
                                                      .output_bytes_per_slot = sizeof(float)}) &&
                    retry.release();
            }
            reason = passed ? "expected_prepare_refusal" : "prepare_refusal_contract_failed";
            break;
        }
        if (!prepared) {
            passed = false;
            reason = "prepare_failed";
            break;
        }

        const std::uint32_t slot_trials = scenario->name == "baseline" ? 2 : 1;
        for (std::uint32_t trial = 0; passed && trial < slot_trials; ++trial) {
            const std::uint64_t sequence = size_index * 10 + trial + 1;
            auto write = arena.grant_write(sequence);
            if (!write) {
                passed = false;
                reason = "grant_failed";
                break;
            }
            auto* input = reinterpret_cast<float*>(write->bytes.data());
            const auto samples = write->bytes.size() / sizeof(float);
            for (std::size_t sample = 0; sample < samples; ++sample)
                input[sample] = static_cast<float>((sample + trial) % 97) * 0.125f - 3.0f;
            if (!arena.publish_written({write->token})) {
                passed = false;
                reason = "publish_failed";
                break;
            }
            const bool submitted = arena.submit(write->token);
            if (scenario->expect_submit_refusal) {
                passed = !submitted && arena.release();
                reason = passed ? "expected_submit_refusal" : "submit_refusal_contract_failed";
                break;
            }
            if (!submitted) {
                passed = false;
                reason = "submit_failed";
                break;
            }
            ++submissions;

            if (arena.acquire_output(arena.preparation_epoch(), sequence)) {
                passed = false;
                reason = "output_visible_without_provider_poll";
                break;
            }

            if (scenario->watchdog_hang) {
                for (;;)
                    std::this_thread::sleep_for(std::chrono::hours(1));
            }

            if (scenario->expect_expired_late_retirement && !arena.expire_delivery(write->token)) {
                passed = false;
                reason = "delivery_expiry_failed";
                break;
            }

            if (scenario->name == "terminal-busy-retry") {
                const auto busy_deadline = Clock::now() + std::chrono::seconds(15);
                while (created.provider->stats().terminal_busy_retries == 0 &&
                       Clock::now() < busy_deadline) {
                    arena.drain_completions();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                passed = created.provider->stats().terminal_busy_retries == 1 && arena.release() &&
                         created.provider->stats().retired_success == 1;
                oracle = std::nullopt;
                reason =
                    passed ? "terminal_busy_retried_by_drain" : "terminal_busy_drain_retry_failed";
                break;
            }

            std::optional<SharedIoArena::OutputLease> output;
            const auto wait =
                scenario->expect_poisoned_retirement || scenario->expect_expired_late_retirement
                    ? std::chrono::milliseconds(30)
                    : std::chrono::seconds(15);
            const bool completed = wait_for_output(arena, sequence, output, wait);
            if (scenario->expect_poisoned_retirement) {
                passed = !completed && arena.release();
                passed = passed && created.provider->stats().retired_failure == 1;
                reason = passed ? "expected_failure_quarantine_and_drain"
                                : "quarantine_or_drain_contract_failed";
                break;
            }
            if (scenario->expect_expired_late_retirement) {
                passed = !completed && arena.release();
                passed = passed && created.provider->stats().retired_success == 1;
                reason = passed ? "expired_delivery_late_clean_drain"
                                : "expiry_or_late_drain_contract_failed";
                break;
            }
            if (!completed) {
                passed = false;
                reason = "completion_timeout";
                break;
            }

            const auto* actual = reinterpret_cast<const float*>(output->bytes.data());
            const auto output_samples = output->bytes.size() / sizeof(float);
            bool trial_oracle = output->bytes.size() == buffer_size.output;
            for (std::size_t sample = 0; trial_oracle && sample < output_samples; ++sample) {
                const float expected = sample < samples ? input[sample] * 1.75f + 0.25f : 0.0f;
                trial_oracle =
                    std::isfinite(actual[sample]) && std::abs(actual[sample] - expected) <= 1.0e-6f;
            }
            oracle = oracle.value_or(true) && trial_oracle;
            const auto token = output->token;
            output.reset();
            if (!arena.release_output({token})) {
                passed = false;
                reason = "output_release_failed";
                break;
            }
            if (scenario->expect_oracle_mismatch) {
                passed = !trial_oracle && arena.release();
                reason =
                    passed ? "oracle_negative_control_detected" : "oracle_negative_control_failed";
                break;
            }
        }
        if (arena.prepared() && !arena.release()) {
            passed = false;
            reason = "arena_release_failed";
        }
    }

    DawnSharedIoProvider::Stats accumulated_stats{};
    if (scenario->name == "baseline" && passed) {
        accumulated_stats = created.provider->stats();
        created.provider.reset();
        created = DawnSharedIoProvider::create(
            {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
             .proc_table_override_for_testing = transfer_counter->deferred_proc_table(),
             .completion_policy = completion_policy,
             .completion_wait_ns = completion_wait_ns});
        if (!created.provider) {
            passed = false;
            reason = "same_process_provider_recreate_failed";
        } else {
            SharedIoArena arena;
            passed = arena.prepare(*created.provider, {.slots = 1,
                                                       .input_bytes_per_slot = sizeof(float),
                                                       .output_bytes_per_slot = sizeof(float)});
            auto write = passed ? arena.grant_write(999) : std::nullopt;
            if (write) {
                *reinterpret_cast<float*>(write->bytes.data()) = 2.0f;
                passed = arena.publish_written({write->token}) && arena.submit(write->token);
            } else {
                passed = false;
            }
            if (passed) {
                ++submissions;
                std::optional<SharedIoArena::OutputLease> output;
                passed = wait_for_output(arena, 999, output, std::chrono::seconds(15));
                if (passed) {
                    const float actual = *reinterpret_cast<const float*>(output->bytes.data());
                    passed = std::abs(actual - 3.75f) <= 1.0e-6f;
                    const auto token = output->token;
                    output.reset();
                    passed = passed && arena.release_output({token});
                }
            }
            passed = arena.release() && passed;
            reason = passed ? "shared_io_oracle" : "same_process_provider_recreate_failed";
        }
    }

    const auto transfers = transfer_counter->snapshot();
    const bool transfers_pass =
        scenario->transfer_control == TransferControl::None
            ? transfer_oracle(transfers, submissions)
            : transfer_control_oracle(transfers, submissions, scenario->transfer_control);
    passed = passed && transfers_pass && installs == 1;
    if (!transfers_pass)
        reason = "transfer_counter_oracle_mismatch";
    else if (scenario->transfer_control != TransferControl::None)
        reason = "transfer_counter_negative_control_detected";
    auto stats = created.provider ? created.provider->stats() : DawnSharedIoProvider::Stats{};
    if (scenario->name == "baseline") {
        stats.slots_created += accumulated_stats.slots_created;
        stats.slots_destroyed += accumulated_stats.slots_destroyed;
        stats.allocations += accumulated_stats.allocations;
        stats.import_attempts += accumulated_stats.import_attempts;
        stats.import_successes += accumulated_stats.import_successes;
        stats.retired_success += accumulated_stats.retired_success;
        stats.retired_failure += accumulated_stats.retired_failure;
        stats.disposals_observed += accumulated_stats.disposals_observed;
        stats.host_frees += accumulated_stats.host_frees;
        stats.drain_calls += accumulated_stats.drain_calls;
        stats.failed_drains += accumulated_stats.failed_drains;
        stats.terminal_busy_retries += accumulated_stats.terminal_busy_retries;
        stats.process_events_calls += accumulated_stats.process_events_calls;
        stats.wait_any_calls += accumulated_stats.wait_any_calls;
        stats.wait_any_timeouts += accumulated_stats.wait_any_timeouts;
        stats.wait_any_unsupported += accumulated_stats.wait_any_unsupported;
        stats.fault_injections += accumulated_stats.fault_injections;
    }
    if (completion_policy != DawnSharedIoProvider::CompletionPolicy::ProcessEvents &&
        stats.wait_any_calls == 0) {
        passed = false;
        reason = "completion_policy_not_exercised";
    }
    if (completion_policy == DawnSharedIoProvider::CompletionPolicy::TimedWaitAny &&
        stats.wait_any_unsupported != 0) {
        passed = false;
        reason = "timed_wait_any_unsupported";
    }
    created.provider.reset();
    emit(scenario->name, passed ? "passed" : "failed", reason, completion_policy, oracle,
         alignment, installs,
         transfers, submissions, stats, adapter);
    return passed ? 0 : 1;
}
