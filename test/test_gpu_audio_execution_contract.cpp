#include <catch2/catch_test_macros.hpp>

#include "detail/shared_io_execution_contract.hpp"
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
    REQUIRE_FALSE(before_gpu_timing.gpu_elapsed_available);

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
}

TEST_CASE("shared IO telemetry validates quiescent admission and delivery bounds",
          "[gpu_audio][shared_io][telemetry]") {
    SharedIoTelemetry telemetry;
    telemetry.configure_depth_lead({2, 4, 2});
    telemetry.record_callback_block(false);
    telemetry.record_admission();
    telemetry.record_submit();
    telemetry.record_retired(true);
    telemetry.record_delivery(true, false);
    telemetry.observe_in_flight(2);

    const auto valid = telemetry.snapshot();
    CHECK(validate_shared_io_telemetry(valid).accepted());

    auto forged = valid;
    forged.configured_available_inflight = 1;
    CHECK(validate_shared_io_telemetry(forged).error ==
          SharedIoTelemetryError::InvalidDepthConfiguration);

    forged = valid;
    forged.in_flight_high_water = 5;
    CHECK(validate_shared_io_telemetry(forged).error ==
          SharedIoTelemetryError::OccupancyExceedsPipelineDepth);

    forged = valid;
    forged.in_flight_high_water = 3;
    CHECK(validate_shared_io_telemetry(forged).error ==
          SharedIoTelemetryError::OccupancyExceedsMaxInflight);

    forged = valid;
    forged.submitted_blocks = 2;
    CHECK(validate_shared_io_telemetry(forged).error ==
          SharedIoTelemetryError::SubmittedExceedsAdmissions);

    forged = valid;
    forged.retired_success = 2;
    CHECK(validate_shared_io_telemetry(forged).error ==
          SharedIoTelemetryError::SuccessfulRetirementsExceedSubmissions);

    forged = valid;
    forged.retired_failure = 1;
    CHECK(validate_shared_io_telemetry(forged).error ==
          SharedIoTelemetryError::RetirementsExceedAdmissions);

    forged = valid;
    forged.delivered_blocks = 2;
    CHECK(validate_shared_io_telemetry(forged).error ==
          SharedIoTelemetryError::DeliveredExceedsCallbacks);

    forged = valid;
    forged.deadline_misses = forged.callback_blocks + 1;
    CHECK(validate_shared_io_telemetry(forged).error ==
          SharedIoTelemetryError::DeadlineMissesExceedCallbacks);

    forged = valid;
    forged.late_completions = forged.retired_success + forged.retired_failure + 1;
    CHECK(validate_shared_io_telemetry(forged).error ==
          SharedIoTelemetryError::LateCompletionsExceedRetirements);

    forged = valid;
    forged.fallback_blocks = 2;
    CHECK(validate_shared_io_telemetry(forged).error ==
          SharedIoTelemetryError::FallbacksExceedDelivered);

    // Controller-only producers from older snapshots use submitted_blocks as
    // their admission boundary and leave the optional admission counter zero.
    auto legacy = valid;
    legacy.admitted_blocks = 0;
    CHECK(validate_shared_io_telemetry(legacy).accepted());

    constexpr SharedIoTelemetrySnapshot unconfigured{};
    STATIC_REQUIRE(validate_shared_io_telemetry(unconfigured).accepted());
}
