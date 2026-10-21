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
        .pipeline_depth = 5,
        .provider_slots = 1,
        .max_inflight = 3,
        .requested_path = SharedIoRequest::Auto,
        .active_path = SharedIoPath::StagedAsync,
        .miss_policy = policy,
        .shared_host_pointer_capable = false,
        .cpu_fallback_prepared = policy == MissPolicy::CpuFallback,
    };
}

struct PublicationProbe {
    bool called = false;
    bool completion_rejected = false;
    std::uint32_t observed_in_flight = 0;
};

struct ReuseProbe {
    bool called = false;
    SharedIoAdmission admission_before_reuse = SharedIoAdmission::Accepted;
    std::uint32_t observed_in_flight = 1;
    std::uint64_t next_sequence = 0;
};

void observe_before_publish(SharedIoExecutionController& controller, std::uint64_t sequence,
                            void* context) noexcept {
    auto& probe = *static_cast<PublicationProbe*>(context);
    probe.called = true;
    probe.observed_in_flight = controller.in_flight_for_testing();
    probe.completion_rejected =
        !controller.record_completion(sequence, SharedIoCompletion::Success);
}

void observe_before_reuse(SharedIoExecutionController& controller, std::uint32_t remaining,
                          void* context) noexcept {
    auto& probe = *static_cast<ReuseProbe*>(context);
    probe.called = true;
    probe.observed_in_flight = remaining;
    probe.admission_before_reuse = controller.admit_submission(probe.next_sequence);
}
} // namespace

TEST_CASE("execution controller reserves occupancy before publishing admission",
          "[gpu_audio][shared_io][controller][adversarial]") {
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(contract()));
    PublicationProbe probe;
    controller.set_before_publish_test_hook(observe_before_publish, &probe);

    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(probe.called);
    REQUIRE(probe.observed_in_flight == 1);
    REQUIRE(probe.completion_rejected);
    REQUIRE(controller.record_completion(0, SharedIoCompletion::Success));
    REQUIRE(controller.deliver(0).path == SharedIoDeliveryPath::Priming);
    REQUIRE(controller.deliver(1).path == SharedIoDeliveryPath::Priming);
    REQUIRE(controller.deliver(2).path == SharedIoDeliveryPath::Gpu);
    REQUIRE(controller.in_flight_for_testing() == 0);
}

TEST_CASE("execution controller keeps a retiring slot unavailable until it is published",
          "[gpu_audio][shared_io][controller][adversarial]") {
    auto minimum_capacity = contract();
    minimum_capacity.algorithmic_lead_blocks = 1;
    minimum_capacity.pipeline_depth = 3;
    minimum_capacity.max_inflight = 2;

    SharedIoTelemetry telemetry;
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(minimum_capacity, &telemetry));
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(1) == SharedIoAdmission::Accepted);
    REQUIRE(controller.record_completion(0, SharedIoCompletion::Success));
    REQUIRE(controller.record_completion(1, SharedIoCompletion::Success));
    REQUIRE(controller.in_flight_for_testing() == 0);
    REQUIRE(controller.admit_submission(2) == SharedIoAdmission::Accepted);
    REQUIRE(controller.deliver(0).path == SharedIoDeliveryPath::Priming);

    // Every table slot is occupied while delivery retires sequence 0: the
    // in-flight budget has room, but the retiring slot is not yet Empty.
    ReuseProbe probe;
    probe.next_sequence = 3;
    controller.set_before_reuse_test_hook(observe_before_reuse, &probe);
    REQUIRE(controller.deliver(1).path == SharedIoDeliveryPath::Gpu);
    REQUIRE(probe.called);
    REQUIRE(probe.observed_in_flight == 1);
    REQUIRE(probe.admission_before_reuse == SharedIoAdmission::CapacityFull);
    controller.set_before_reuse_test_hook(nullptr, nullptr);
    REQUIRE(controller.admit_submission(3) == SharedIoAdmission::Accepted);
    REQUIRE(controller.in_flight_for_testing() == minimum_capacity.max_inflight);
    REQUIRE(controller.admit_submission(4) == SharedIoAdmission::CapacityFull);
    REQUIRE(telemetry.snapshot().in_flight_high_water == minimum_capacity.max_inflight);
}

TEST_CASE("execution controller releases the in-flight budget at completion",
          "[gpu_audio][shared_io][controller][adversarial]") {
    auto single = contract();
    single.max_inflight = 1;

    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(single));
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(1) == SharedIoAdmission::CapacityFull);
    REQUIRE(controller.record_completion(0, SharedIoCompletion::Success));
    REQUIRE(controller.in_flight_for_testing() == 0);
    // Sequence 0 still waits out the lead in its table slot, yet the provider
    // is idle, so the next submission is admitted.
    REQUIRE(controller.admit_submission(1) == SharedIoAdmission::Accepted);
    REQUIRE(controller.record_completion(1, SharedIoCompletion::Failed));
    REQUIRE(controller.in_flight_for_testing() == 0);
}

TEST_CASE("execution controller tracks lead separately from logical pipeline depth",
          "[gpu_audio][shared_io][controller]") {
    SharedIoTelemetry telemetry;
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(contract(), &telemetry));
    REQUIRE(controller.capacity() == 5);
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

TEST_CASE("execution controller enforces max-inflight before publishing admission",
          "[gpu_audio][shared_io][controller][adversarial]") {
    auto bounded = contract();
    bounded.pipeline_depth = 4;
    bounded.max_inflight = 2;

    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(bounded));
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(1) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(2) == SharedIoAdmission::CapacityFull);
}

TEST_CASE("execution controller rejects an impossible max-inflight budget",
          "[gpu_audio][shared_io][controller][adversarial]") {
    auto invalid = contract();
    invalid.max_inflight = 0;
    SharedIoExecutionController controller;
    REQUIRE_FALSE(controller.prepare(invalid));

    invalid.max_inflight = 2;
    invalid.pipeline_depth = invalid.algorithmic_lead_blocks + 1;
    REQUIRE_FALSE(controller.prepare(invalid));
}

TEST_CASE("execution controller rejects a lead larger than logical pipeline depth",
          "[gpu_audio][shared_io][controller][adversarial]") {
    auto delayed_contract = contract();
    delayed_contract.algorithmic_lead_blocks = 5;
    delayed_contract.pipeline_depth = 2;

    SharedIoExecutionController controller;
    REQUIRE_FALSE(controller.prepare(delayed_contract));
}

TEST_CASE("execution controller sustains one submission per callback with a free slot beyond lead",
          "[gpu_audio][shared_io][controller][adversarial]") {
    auto exact_depth = contract();
    exact_depth.pipeline_depth = exact_depth.algorithmic_lead_blocks + 1;
    exact_depth.max_inflight = 1;
    REQUIRE(exact_depth.algorithmic_lead_blocks == 2);

    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(exact_depth));
    for (std::uint64_t callback_sequence = 0; callback_sequence < 8; ++callback_sequence) {
        const auto delivery = controller.deliver(callback_sequence);
        if (callback_sequence < exact_depth.algorithmic_lead_blocks) {
            REQUIRE(delivery.path == SharedIoDeliveryPath::Priming);
        } else {
            REQUIRE(delivery.path == SharedIoDeliveryPath::Gpu);
            REQUIRE(delivery.expected_sequence ==
                    callback_sequence - exact_depth.algorithmic_lead_blocks);
        }
        REQUIRE(controller.admit_submission(callback_sequence) == SharedIoAdmission::Accepted);
        REQUIRE(controller.record_completion(callback_sequence, SharedIoCompletion::Success));
    }
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

TEST_CASE("device-loss fallback preserves callback continuity and receipt accounting",
          "[gpu_audio][shared_io][controller][continuity][fault-injection]") {
    auto failure_contract = contract();
    failure_contract.algorithmic_lead_blocks = 1;
    failure_contract.pipeline_depth = 4;
    failure_contract.max_inflight = 3;

    SharedIoTelemetry telemetry;
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(failure_contract, &telemetry));

    // Prime one successful block, then inject a deterministic provider loss
    // for the next three blocks. Every callback must still produce either the
    // GPU result or the declared CPU replacement; no callback may silently
    // fall through to silence.
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.record_completion(0, SharedIoCompletion::Success));
    REQUIRE(controller.deliver(0).path == SharedIoDeliveryPath::Priming);

    REQUIRE(controller.admit_submission(1) == SharedIoAdmission::Accepted);
    REQUIRE(controller.record_completion(1, SharedIoCompletion::Failed,
                                         SharedIoFallbackReason::DeviceLost));
    const auto gpu = controller.deliver(1);
    REQUIRE(gpu.path == SharedIoDeliveryPath::Gpu);
    REQUIRE(gpu.expected_sequence == 0);

    REQUIRE(controller.admit_submission(2) == SharedIoAdmission::Accepted);
    REQUIRE(controller.record_completion(2, SharedIoCompletion::Failed,
                                         SharedIoFallbackReason::DeviceLost));
    const auto first_fallback = controller.deliver(2);
    REQUIRE(first_fallback.path == SharedIoDeliveryPath::CpuFallback);
    REQUIRE(first_fallback.fallback_reason == SharedIoFallbackReason::DeviceLost);

    REQUIRE(controller.admit_submission(3) == SharedIoAdmission::Accepted);
    REQUIRE(controller.record_completion(3, SharedIoCompletion::Failed,
                                         SharedIoFallbackReason::DeviceLost));
    const auto second_fallback = controller.deliver(3);
    REQUIRE(second_fallback.path == SharedIoDeliveryPath::CpuFallback);
    REQUIRE(second_fallback.fallback_reason == SharedIoFallbackReason::DeviceLost);

    // With one lead block, callback 4 consumes the failed sequence 3 entry.
    const auto third_fallback = controller.deliver(4);
    REQUIRE(third_fallback.path == SharedIoDeliveryPath::CpuFallback);
    REQUIRE(third_fallback.expected_sequence == 3);
    REQUIRE(third_fallback.fallback_reason == SharedIoFallbackReason::DeviceLost);

    const auto receipt = telemetry.snapshot();
    REQUIRE(receipt.callback_blocks == 5);
    REQUIRE(receipt.submitted_blocks == 4);
    REQUIRE(receipt.retired_success == 1);
    REQUIRE(receipt.retired_failure == 3);
    REQUIRE(receipt.delivered_blocks == 5);
    REQUIRE(receipt.fallback_blocks == 3);
    REQUIRE(receipt.deadline_misses == 0);
    REQUIRE(receipt.resync_drops == 0);
}

TEST_CASE("device-loss continuity refuses a missing CPU fallback",
          "[gpu_audio][shared_io][controller][continuity][fault-injection][negative]") {
    auto invalid = contract();
    invalid.cpu_fallback_prepared = false;

    SharedIoExecutionController controller;
    REQUIRE_FALSE(controller.prepare(invalid));
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
    REQUIRE(telemetry.snapshot().input_drops == 0);
    REQUIRE(telemetry.snapshot().resync_drops == 1);
}

TEST_CASE("execution controller resynchronizes admission after a callback gap",
          "[gpu_audio][shared_io][controller][adversarial]") {
    SharedIoTelemetry telemetry;
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(contract(), &telemetry));
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(1) == SharedIoAdmission::Accepted);
    REQUIRE(controller.record_completion(0, SharedIoCompletion::Success));
    REQUIRE(controller.deliver(0).path == SharedIoDeliveryPath::Priming);

    const auto gap = controller.deliver(4);
    REQUIRE(gap.fallback_reason == SharedIoFallbackReason::SequenceGap);
    REQUIRE(gap.resynced);
    REQUIRE_FALSE(controller.record_completion(0, SharedIoCompletion::Success));
    REQUIRE(controller.record_completion(1, SharedIoCompletion::Success));
    REQUIRE(controller.admit_submission(3) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(4) == SharedIoAdmission::Accepted);
    const auto resumed = controller.deliver(5);
    REQUIRE(resumed.path == SharedIoDeliveryPath::CpuFallback);
    REQUIRE(resumed.resynced);
    REQUIRE(controller.admit_submission(5) == SharedIoAdmission::Accepted);
    REQUIRE(telemetry.snapshot().in_flight_high_water <= controller.capacity());
    REQUIRE(telemetry.snapshot().late_completions == 2);
}

TEST_CASE("callback gap preserves ready results that are still ahead of the lead",
          "[gpu_audio][shared_io][controller][adversarial]") {
    auto deeper = contract();
    deeper.pipeline_depth = 6;
    deeper.max_inflight = 4;

    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(deeper));
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(1) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(2) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(3) == SharedIoAdmission::Accepted);
    REQUIRE(controller.record_completion(3, SharedIoCompletion::Success));
    REQUIRE(controller.deliver(0).path == SharedIoDeliveryPath::Priming);

    const auto gap = controller.deliver(4);
    REQUIRE(gap.fallback_reason == SharedIoFallbackReason::SequenceGap);
    REQUIRE(gap.resynced);
    const auto future = controller.deliver(5);
    REQUIRE(future.path == SharedIoDeliveryPath::Gpu);
    REQUIRE(future.expected_sequence == 3);
}

TEST_CASE("callback gap preserves a retryable admission that is still due",
          "[gpu_audio][shared_io][controller][adversarial]") {
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(contract()));
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(1) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(2) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(3) == SharedIoAdmission::CapacityFull);
    REQUIRE(controller.deliver(0).path == SharedIoDeliveryPath::Priming);

    const auto gap = controller.deliver(4);
    REQUIRE(gap.fallback_reason == SharedIoFallbackReason::SequenceGap);
    REQUIRE(gap.resynced);
    REQUIRE(controller.admit_submission(3) == SharedIoAdmission::CapacityFull);
    REQUIRE(controller.record_completion(0, SharedIoCompletion::Success));
    REQUIRE(controller.record_completion(1, SharedIoCompletion::Success));
    REQUIRE(controller.record_completion(2, SharedIoCompletion::Success));
    REQUIRE(controller.deliver(5).path == SharedIoDeliveryPath::CpuFallback);
    REQUIRE(controller.admit_submission(3) == SharedIoAdmission::Accepted);
}

TEST_CASE("execution controller counts every late completion removed by one callback",
          "[gpu_audio][shared_io][controller][adversarial]") {
    auto short_lead = contract();
    short_lead.algorithmic_lead_blocks = 1;

    SharedIoTelemetry telemetry;
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(short_lead, &telemetry));
    REQUIRE(controller.admit_submission(0) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(1) == SharedIoAdmission::Accepted);
    REQUIRE(controller.admit_submission(2) == SharedIoAdmission::Accepted);
    REQUIRE(controller.deliver(0).path == SharedIoDeliveryPath::Priming);
    REQUIRE(controller.deliver(1).path == SharedIoDeliveryPath::CpuFallback);
    REQUIRE(controller.deliver(2).path == SharedIoDeliveryPath::CpuFallback);
    REQUIRE(controller.record_completion(0, SharedIoCompletion::Success));
    REQUIRE(controller.record_completion(1, SharedIoCompletion::Success));

    const auto delivery = controller.deliver(3);
    REQUIRE(delivery.path == SharedIoDeliveryPath::CpuFallback);
    REQUIRE(delivery.resynced);
    REQUIRE(telemetry.snapshot().late_completions == 2);
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

TEST_CASE("duplicate callbacks fail closed without rewinding the sequence epoch",
          "[gpu_audio][shared_io][controller][adversarial]") {
    SharedIoExecutionController controller;
    REQUIRE(controller.prepare(contract()));
    REQUIRE(controller.deliver(0).path == SharedIoDeliveryPath::Priming);

    const auto duplicate = controller.deliver(0);
    REQUIRE(duplicate.path == SharedIoDeliveryPath::CpuFallback);
    REQUIRE(duplicate.fallback_reason == SharedIoFallbackReason::SequenceGap);
    REQUIRE_FALSE(duplicate.resynced);
    REQUIRE(controller.deliver(1).path == SharedIoDeliveryPath::Priming);
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
        callback_ok = callback_ok && controller.deliver(1).path == SharedIoDeliveryPath::Priming;
        callback_ok = callback_ok && controller.deliver(2).path == SharedIoDeliveryPath::Gpu;
        allocations = probe.allocation_count();
    }
    REQUIRE(callback_ok);
    REQUIRE(allocations == 0);
}

static_assert(std::is_nothrow_destructible_v<SharedIoExecutionController>);
