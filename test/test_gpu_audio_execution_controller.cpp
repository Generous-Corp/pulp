#include "detail/shared_io_execution_controller.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

using namespace pulp::gpu_audio;
using namespace pulp::gpu_audio::detail;

namespace {
SharedIoExecutionContract contract(MissPolicy policy = MissPolicy::CpuFallback) {
    return SharedIoExecutionContract{
        .channels = 2,
        .block_size = 64,
        .sample_rate = 48000,
        .algorithmic_lead_blocks = 2,
        .pipeline_depth = 3,
        .requested_path = SharedIoRequest::Auto,
        .active_path = SharedIoPath::StagedAsync,
        .miss_policy = policy,
        .shared_host_pointer_capable = false,
        .cpu_fallback_prepared = policy == MissPolicy::CpuFallback,
    };
}
} // namespace

TEST_CASE("execution controller admits lead independently of pipeline depth",
          "[gpu_audio][shared_io][controller]") {
    SharedIoTelemetry telemetry;
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(contract(), &telemetry));
    REQUIRE(controller.capacity() == 3);
    REQUIRE(controller.algorithmic_lead_blocks() == 2);
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(1) == SharedIoAdmission::Accepted);
    REQUIRE(controller.record_completion(0, SharedIoCompletion::Success));

    const auto priming = controller.deliver(0);
    REQUIRE(priming.path == SharedIoDeliveryPath::Priming);
    REQUIRE(controller.deliver(1).path == SharedIoDeliveryPath::Priming);
    const auto wet = controller.deliver(2);
    REQUIRE(wet.path == SharedIoDeliveryPath::Gpu);
    REQUIRE(wet.expected_sequence == 0);
}

TEST_CASE("execution controller sustains a lead larger than physical depth",
          "[gpu_audio][shared_io][controller][adversarial]") {
    auto delayed_contract = contract();
    delayed_contract.algorithmic_lead_blocks = 5;
    delayed_contract.pipeline_depth = 2;

    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(delayed_contract));
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(1) == SharedIoAdmission::Accepted);
    REQUIRE(controller.record_completion(0, SharedIoCompletion::Success));
    REQUIRE(controller.record_completion(1, SharedIoCompletion::Success));

    for (std::uint64_t sequence = 0; sequence < 5; ++sequence)
        REQUIRE(controller.deliver(sequence).path == SharedIoDeliveryPath::Priming);
    REQUIRE(controller.deliver(5).path == SharedIoDeliveryPath::Gpu);

    REQUIRE(controller.admit_submission(2) == SharedIoAdmission::Accepted);
    REQUIRE(controller.record_completion(2, SharedIoCompletion::Success));
    REQUIRE(controller.deliver(6).path == SharedIoDeliveryPath::Gpu);
}

TEST_CASE("execution controller requires exact sequences and reports typed fallback",
          "[gpu_audio][shared_io][controller]") {
    SharedIoTelemetry telemetry;
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(contract(), &telemetry));
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(2) == SharedIoAdmission::SequenceGap);
    REQUIRE(controller.admit_submission(1) == SharedIoAdmission::Accepted);

    auto miss = controller.deliver(0);
    REQUIRE(miss.path == SharedIoDeliveryPath::Priming);
    REQUIRE(controller.deliver(1).path == SharedIoDeliveryPath::Priming);
    miss = controller.deliver(2);
    REQUIRE(miss.path == SharedIoDeliveryPath::CpuFallback);
    REQUIRE(miss.fallback_reason == SharedIoFallbackReason::DeadlineExceeded);

    REQUIRE(controller.record_completion(0, SharedIoCompletion::Success));
    const auto late = controller.deliver(3);
    REQUIRE(late.path == SharedIoDeliveryPath::CpuFallback);
    REQUIRE(late.resynced);
    REQUIRE(late.fallback_reason == SharedIoFallbackReason::DeadlineExceeded);

    const auto stats = telemetry.snapshot();
    REQUIRE(stats.resync_drops >= 2);
    REQUIRE(stats.deadline_misses == 2);
    REQUIRE(stats.fallback_blocks == 2);
}

TEST_CASE("execution controller maps provider failure to the declared CPU fallback",
          "[gpu_audio][shared_io][controller]") {
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(contract()));
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.record_completion(0, SharedIoCompletion::Failed,
                                         SharedIoFallbackReason::DeviceLost));
    REQUIRE(controller.deliver(0).path == SharedIoDeliveryPath::Priming);
    REQUIRE(controller.deliver(1).path == SharedIoDeliveryPath::Priming);
    const auto delivery = controller.deliver(2);
    REQUIRE(delivery.path == SharedIoDeliveryPath::CpuFallback);
    REQUIRE(delivery.fallback_reason == SharedIoFallbackReason::DeviceLost);
    REQUIRE_FALSE(controller.record_completion(0, SharedIoCompletion::Success));
}

TEST_CASE("execution controller bounds admission and rejects stale completion identities",
          "[gpu_audio][shared_io][controller][adversarial]") {
    SharedIoTelemetry telemetry;
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(contract(), &telemetry));
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(1) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(2) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(3) == SharedIoAdmission::CapacityFull);
    REQUIRE_FALSE(controller.record_completion(99, SharedIoCompletion::Success));
    REQUIRE(controller.record_completion(0, SharedIoCompletion::Success));
    REQUIRE(controller.deliver(0).path == SharedIoDeliveryPath::Priming);
    REQUIRE(controller.deliver(1).path == SharedIoDeliveryPath::Priming);
    REQUIRE(controller.deliver(2).path == SharedIoDeliveryPath::Gpu);
    REQUIRE(controller.admit_submission(3) == SharedIoAdmission::Accepted);
    REQUIRE(telemetry.snapshot().input_drops == 1);
    REQUIRE(telemetry.snapshot().resync_drops == 1);
}

TEST_CASE("execution controller counts silence as a typed miss fallback",
          "[gpu_audio][shared_io][controller]") {
    SharedIoTelemetry telemetry;
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(contract(MissPolicy::Silence), &telemetry));
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.deliver(0).path == SharedIoDeliveryPath::Priming);
    REQUIRE(controller.deliver(1).path == SharedIoDeliveryPath::Priming);
    const auto miss = controller.deliver(2);
    REQUIRE(miss.path == SharedIoDeliveryPath::Silence);
    REQUIRE(miss.fallback_reason == SharedIoFallbackReason::DeadlineExceeded);
    REQUIRE(telemetry.snapshot().fallback_blocks == 1);
}

TEST_CASE("execution controller accounts for every callback and sequence-gap silence",
          "[gpu_audio][shared_io][controller][adversarial]") {
    SharedIoTelemetry telemetry;
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(contract(MissPolicy::Silence), &telemetry));

    REQUIRE(controller.deliver(0).path == SharedIoDeliveryPath::Priming);
    const auto gap = controller.deliver(2);
    REQUIRE(gap.path == SharedIoDeliveryPath::Silence);
    REQUIRE(gap.fallback_reason == SharedIoFallbackReason::SequenceGap);
    REQUIRE(gap.resynced);

    const auto stats = telemetry.snapshot();
    REQUIRE(stats.callback_blocks == 2);
    REQUIRE(stats.delivered_blocks == 2);
    REQUIRE(stats.fallback_blocks == 1);
    REQUIRE(stats.deadline_misses == 0);
    REQUIRE(stats.resync_drops == 1);
}

TEST_CASE("sequence-gap delivery honors the declared CPU fallback",
          "[gpu_audio][shared_io][controller][adversarial]") {
    SharedIoTelemetry telemetry;
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(contract(), &telemetry));
    REQUIRE(controller.deliver(0).path == SharedIoDeliveryPath::Priming);
    const auto gap = controller.deliver(2);
    REQUIRE(gap.path == SharedIoDeliveryPath::CpuFallback);
    REQUIRE(gap.fallback_reason == SharedIoFallbackReason::SequenceGap);
    REQUIRE(gap.resynced);
    REQUIRE(telemetry.snapshot().fallback_blocks == 1);
}

TEST_CASE("controller callback delivery performs no allocation",
          "[gpu_audio][shared_io][controller][allocation]") {
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(contract()));
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.record_completion(0, SharedIoCompletion::Success));

    bool callback_ok = false;
    std::size_t allocations = 1;
    {
        pulp::test::RtAllocationProbe probe;
        callback_ok = controller.deliver(0).path == SharedIoDeliveryPath::Priming;
        callback_ok = callback_ok &&
                      controller.deliver(1).path == SharedIoDeliveryPath::Priming;
        callback_ok = callback_ok &&
                      controller.deliver(2).path == SharedIoDeliveryPath::Gpu;
        allocations = probe.allocation_count();
    }
    REQUIRE(callback_ok);
    REQUIRE(allocations == 0);
}

static_assert(std::is_nothrow_destructible_v<SharedIoExecutionController>);
