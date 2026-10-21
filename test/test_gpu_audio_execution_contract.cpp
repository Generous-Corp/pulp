#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <thread>

#include "detail/shared_io_execution_contract.hpp"
#include "support/thread_progress.hpp"
#include <pulp/gpu_audio/gpu_audio_node.hpp>

using namespace pulp::gpu_audio;
using namespace pulp::gpu_audio::detail;

TEST_CASE("shared IO contract separates logical capacity from physical provider slots",
          "[gpu_audio][shared_io]") {
    SharedIoExecutionContract contract{
        .channels = 2,
        .block_size = 32,
        .sample_rate = 48000,
        .algorithmic_lead_blocks = 3,
        .pipeline_depth = 2,
        .provider_slots = 1,
        .requested_path = SharedIoRequest::RequireSharedHostPointer,
        .active_path = SharedIoPath::SharedHostPointer,
        .miss_policy = MissPolicy::CpuFallback,
        .shared_host_pointer_capable = true,
        .cpu_fallback_prepared = true,
    };
    REQUIRE(validate_shared_io_contract(contract).error ==
            SharedIoContractError::InsufficientPipelineDepth);
    contract.pipeline_depth = contract.algorithmic_lead_blocks;
    REQUIRE(validate_shared_io_contract(contract).error ==
            SharedIoContractError::InsufficientPipelineDepth);
    contract.pipeline_depth = contract.algorithmic_lead_blocks + 1;
    REQUIRE(validate_shared_io_contract(contract).accepted());
    contract.provider_slots = 0;
    REQUIRE(validate_shared_io_contract(contract).error ==
            SharedIoContractError::MissingProviderSlots);
    contract.provider_slots = 1;
    contract.algorithmic_lead_blocks = 0;
    REQUIRE(validate_shared_io_contract(contract).error ==
            SharedIoContractError::MissingAlgorithmicLead);
}

TEST_CASE("shared IO contract rejects unknown backend-neutral enum values",
          "[gpu_audio][shared_io]") {
    const auto valid_contract = [] {
        return SharedIoExecutionContract{
            .channels = 2,
            .block_size = 32,
            .sample_rate = 48000,
            .algorithmic_lead_blocks = 1,
            .pipeline_depth = 2,
            .provider_slots = 1,
            .requested_path = SharedIoRequest::RequireSharedHostPointer,
            .active_path = SharedIoPath::SharedHostPointer,
            .shared_host_pointer_capable = true,
            .cpu_fallback_prepared = true,
        };
    };

    auto contract = valid_contract();
    contract.active_path = static_cast<SharedIoPath>(0xfe);
    CHECK(validate_shared_io_contract(contract).error == SharedIoContractError::InvalidEnumValue);

    contract = valid_contract();
    contract.requested_path = static_cast<SharedIoRequest>(0xfe);
    CHECK(validate_shared_io_contract(contract).error == SharedIoContractError::InvalidEnumValue);

    contract = valid_contract();
    contract.miss_policy = static_cast<MissPolicy>(0xfe);
    CHECK(validate_shared_io_contract(contract).error == SharedIoContractError::InvalidEnumValue);

    contract = valid_contract();
    contract.fallback_reason = static_cast<SharedIoFallbackReason>(0xfe);
    CHECK(validate_shared_io_contract(contract).error == SharedIoContractError::InvalidEnumValue);
}

TEST_CASE("shared IO depth lead policy bounds multi-flight admission",
          "[gpu_audio][shared_io][p4]") {
    constexpr SharedIoDepthLeadPolicy policy{4, 8, 4};
    STATIC_REQUIRE(policy.valid());
    STATIC_REQUIRE(policy.available_inflight() == 4);
    constexpr SharedIoDepthLeadPolicy overcommitted{4, 8, 5};
    STATIC_REQUIRE_FALSE(overcommitted.valid());

    SharedIoExecutionContract contract{
        .channels = 2,
        .block_size = 32,
        .sample_rate = 48000,
        .algorithmic_lead_blocks = 4,
        .pipeline_depth = 8,
        .provider_slots = 4,
        .max_inflight = 5,
        .active_path = SharedIoPath::StagedAsync,
    };
    REQUIRE_FALSE(SharedIoDepthLeadPolicy{contract.algorithmic_lead_blocks, contract.pipeline_depth,
                                          contract.max_inflight}
                      .valid());
    REQUIRE(validate_shared_io_contract(contract).error ==
            SharedIoContractError::InvalidMaxInflight);
    contract.max_inflight = 4;
    REQUIRE(validate_shared_io_contract(contract).accepted());
    contract.max_inflight = 0;
    REQUIRE(validate_shared_io_contract(contract).error ==
            SharedIoContractError::InvalidMaxInflight);
}

TEST_CASE("shared IO contract fails closed for unavailable paths and fallback",
          "[gpu_audio][shared_io]") {
    SharedIoExecutionContract contract{
        .channels = 2,
        .block_size = 128,
        .sample_rate = 48000,
        .algorithmic_lead_blocks = 2,
        .pipeline_depth = 3,
        .provider_slots = 1,
        .requested_path = SharedIoRequest::RequireSharedHostPointer,
        .active_path = SharedIoPath::StagedAsync,
        .miss_policy = MissPolicy::CpuFallback,
        .shared_host_pointer_capable = false,
        .cpu_fallback_prepared = false,
        .fallback_reason = SharedIoFallbackReason::UnsupportedFeature,
    };
    REQUIRE(validate_shared_io_contract(contract).error ==
            SharedIoContractError::CpuFallbackNotPrepared);
    contract.cpu_fallback_prepared = true;
    REQUIRE(validate_shared_io_contract(contract).error ==
            SharedIoContractError::RequestedPathUnavailable);
    contract.active_path = SharedIoPath::SharedHostPointer;
    REQUIRE(validate_shared_io_contract(contract).error ==
            SharedIoContractError::ProviderUnavailable);
    contract.shared_host_pointer_capable = true;
    REQUIRE(validate_shared_io_contract(contract).accepted());
    contract.requested_path = SharedIoRequest::Auto;
    REQUIRE(validate_shared_io_contract(contract).accepted());
}

TEST_CASE("shared IO telemetry reports zero copy and unavailable GPU timing explicitly",
          "[gpu_audio][shared_io]") {
    SharedIoTelemetry telemetry;
    telemetry.configure_depth_lead({2, 4, 2});
    telemetry.record_callback_block(true);
    telemetry.record_submit();
    telemetry.record_retired(true);
    telemetry.record_delivery(true, true);
    telemetry.observe_in_flight(2);
    telemetry.observe_in_flight(1);
    telemetry.record_payload_copy(0);
    telemetry.record_completion_timing(324695, 721250);

    const auto before_gpu_timing = telemetry.snapshot();
    REQUIRE(before_gpu_timing.callback_blocks == 1);
    REQUIRE(before_gpu_timing.configured_lead_blocks == 2);
    REQUIRE(before_gpu_timing.configured_pipeline_depth == 4);
    REQUIRE(before_gpu_timing.configured_max_inflight == 2);
    REQUIRE(before_gpu_timing.configured_available_inflight == 2);
    REQUIRE(before_gpu_timing.deadline_misses == 1);
    REQUIRE(before_gpu_timing.submitted_blocks == 1);
    REQUIRE(before_gpu_timing.retired_success == 1);
    REQUIRE(before_gpu_timing.delivered_blocks == 1);
    REQUIRE(before_gpu_timing.fallback_blocks == 1);
    REQUIRE(before_gpu_timing.late_completions == 1);
    REQUIRE(before_gpu_timing.in_flight_high_water == 2);
    REQUIRE(before_gpu_timing.payload_bytes_copied == 0);
    REQUIRE(before_gpu_timing.pre_submit_delay_ns == 324695);
    REQUIRE(before_gpu_timing.submit_to_completion_ns == 721250);
    REQUIRE(before_gpu_timing.scheduled_to_completion_ns == 1045945);
    REQUIRE(before_gpu_timing.completion_timing_valid);
    REQUIRE_FALSE(before_gpu_timing.gpu_elapsed_available);

    telemetry.record_completion_timing(std::numeric_limits<std::uint64_t>::max() - 1, 1);
    const auto boundary_timing = telemetry.snapshot();
    REQUIRE(boundary_timing.completion_timing_valid);
    REQUIRE(boundary_timing.scheduled_to_completion_ns ==
            std::numeric_limits<std::uint64_t>::max());

    telemetry.record_completion_timing(std::numeric_limits<std::uint64_t>::max(), 1);
    const auto overflowed_timing = telemetry.snapshot();
    REQUIRE_FALSE(overflowed_timing.completion_timing_valid);
    REQUIRE(overflowed_timing.scheduled_to_completion_ns == 0);
    REQUIRE(overflowed_timing.pre_submit_delay_ns == std::numeric_limits<std::uint64_t>::max());
    REQUIRE(overflowed_timing.submit_to_completion_ns == 1);

    telemetry.record_completion_timing(10, 20);
    const auto repaired_timing = telemetry.snapshot();
    REQUIRE(repaired_timing.completion_timing_valid);
    REQUIRE(repaired_timing.scheduled_to_completion_ns == 30);

    telemetry.record_gpu_elapsed_ns(42);
    telemetry.record_gpu_busy_counter(7);
    const auto after_gpu_timing = telemetry.snapshot();
    REQUIRE(after_gpu_timing.gpu_elapsed_available);
    REQUIRE(after_gpu_timing.gpu_elapsed_ns == 42);
    REQUIRE(after_gpu_timing.gpu_busy_counter == 7);

    telemetry.reset();
    REQUIRE(telemetry.snapshot().callback_blocks == 0);
    REQUIRE(telemetry.snapshot().configured_pipeline_depth == 0);
    REQUIRE(telemetry.snapshot().configured_available_inflight == 0);
    REQUIRE_FALSE(telemetry.snapshot().gpu_elapsed_available);
    REQUIRE(telemetry.snapshot().completion_timing_valid);
}

TEST_CASE("shared IO telemetry completion timing snapshots stay coherent under concurrent updates",
          "[gpu_audio][shared_io]") {
    // Normal samples satisfy submit == 2 * pre + 1 (odd); overflow samples carry
    // pre == max and an even submit; reset() publishes all zeros. A snapshot
    // that mixes fields from different updates matches none of these shapes.
    constexpr int kSnapshots = 200000;
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    SharedIoTelemetry telemetry;
    std::atomic<std::uint64_t> published{0};
    std::atomic<std::uint64_t> resets{0};
    std::atomic<bool> stop{false};
    std::thread recorder([&] {
        std::uint64_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            ++i;
            const bool sample_published = i % 8 == 0
                                              ? telemetry.record_completion_timing(kMax, 2 * i)
                                              : telemetry.record_completion_timing(i, 2 * i + 1);
            if (sample_published)
                published.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread resetter([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            telemetry.reset();
            resets.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });
    // Require a publication that actually won the guard while reset() was
    // contending for it, not merely a recorder call that dropped its sample.
    const bool writers_started = pulp::test::wait_for_condition([&] {
        return published.load(std::memory_order_relaxed) != 0 &&
               resets.load(std::memory_order_relaxed) != 0;
    });

    std::uint64_t incoherent = 0;
    for (int n = 0; writers_started && n < kSnapshots; ++n) {
        const auto s = telemetry.snapshot();
        const bool zero = s.pre_submit_delay_ns == 0 && s.submit_to_completion_ns == 0 &&
                          s.scheduled_to_completion_ns == 0;
        bool coherent = false;
        if (s.completion_timing_valid) {
            coherent = zero || (s.submit_to_completion_ns == 2 * s.pre_submit_delay_ns + 1 &&
                                s.scheduled_to_completion_ns ==
                                    s.pre_submit_delay_ns + s.submit_to_completion_ns);
        } else {
            // Invalid is either an overflow sample or the fail-closed contended
            // read, which carries no timing values.
            coherent =
                zero || (s.pre_submit_delay_ns == kMax && s.scheduled_to_completion_ns == 0 &&
                         s.submit_to_completion_ns != 0 && s.submit_to_completion_ns % 2 == 0);
        }
        if (!coherent)
            ++incoherent;
    }
    stop.store(true, std::memory_order_relaxed);
    recorder.join();
    resetter.join();

    REQUIRE(writers_started);
    REQUIRE(incoherent == 0);

    REQUIRE(telemetry.record_completion_timing(5, 11));
    const auto settled = telemetry.snapshot();
    REQUIRE(settled.completion_timing_valid);
    REQUIRE(settled.scheduled_to_completion_ns == 16);
}
