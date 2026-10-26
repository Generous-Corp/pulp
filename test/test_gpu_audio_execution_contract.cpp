#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
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
    CHECK(validate_shared_io_contract(contract).accepted());
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
    //
    // Overlap is measured, not hoped for. Each writer bumps `started` before
    // every update call and `finished` after it returns, so a read overlapped an
    // update in flight exactly when `started`, loaded after the snapshot,
    // exceeds `finished`, loaded before it. The reader drives two phases:
    // recorder only, then recorder and reset() contending for the guard. In the
    // contended phase it counts overlaps with each writer separately. Each
    // phase keeps reading until every watched writer has kTargetOverlappedReads
    // overlapped reads. That count gives the test its power against torn
    // reads, and unloaded or sanitizer-slowed hosts reach it. A starved host may
    // stop at kMinimumOverlappedReads per writer once the phase's soft budget has
    // passed, with a WARN carrying the counts. The test fails only if even that
    // minimum is not reached by the shared hard deadline. Progress is counted
    // only from samples that won the guard and were published.
    constexpr std::uint64_t kTargetOverlappedReads = 100000;
    constexpr std::uint64_t kMinimumOverlappedReads = 1000;
    constexpr auto kPhaseSoftBudget = pulp::test::kProgressDeadline;
    constexpr auto kHardDeadline = 12 * pulp::test::kProgressDeadline;
    constexpr std::uint32_t kReadsBeforeBackoff = 1024;
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    enum Phase : int { Idle = 0, RecorderOnly = 1, Both = 2, Stop = 3 };
    struct WriterProgress {
        std::atomic<std::uint64_t> started{0};
        std::atomic<std::uint64_t> finished{0};
        std::atomic<std::uint64_t> published{0};
    };
    SharedIoTelemetry telemetry;
    std::atomic<int> phase{Idle};
    WriterProgress recorder_progress;
    WriterProgress reset_progress;
    // Writers sleep briefly every kUpdatesPerBurst updates. A thread that never
    // blocks loses scheduling priority on a saturated host and can go
    // unscheduled for a whole phase; the wakeup keeps it competitive.
    constexpr std::uint64_t kUpdatesPerBurst = 4096;
    const auto writer_pause = [] { std::this_thread::sleep_for(std::chrono::microseconds(20)); };
    const auto idle_pause = [] { std::this_thread::sleep_for(std::chrono::microseconds(100)); };
    std::thread recorder([&] {
        std::uint64_t i = 0;
        for (int p = phase.load(std::memory_order_acquire); p != Stop;
             p = phase.load(std::memory_order_acquire)) {
            if (p != RecorderOnly && p != Both) {
                idle_pause();
                continue;
            }
            if (++i % kUpdatesPerBurst == 0)
                writer_pause();
            recorder_progress.started.fetch_add(1, std::memory_order_seq_cst);
            const bool sample_published = i % 8 == 0
                                              ? telemetry.record_completion_timing(kMax, 2 * i)
                                              : telemetry.record_completion_timing(i, 2 * i + 1);
            recorder_progress.finished.fetch_add(1, std::memory_order_seq_cst);
            if (sample_published)
                recorder_progress.published.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread resetter([&] {
        std::uint64_t i = 0;
        for (int p = phase.load(std::memory_order_acquire); p != Stop;
             p = phase.load(std::memory_order_acquire)) {
            if (p != Both) {
                idle_pause();
                continue;
            }
            reset_progress.started.fetch_add(1, std::memory_order_seq_cst);
            telemetry.reset();
            reset_progress.finished.fetch_add(1, std::memory_order_seq_cst);
            reset_progress.published.fetch_add(1, std::memory_order_relaxed);
            if (++i % kUpdatesPerBurst == 0)
                writer_pause();
            // reset() waits for the guard; yielding lets the recorder win it
            // too, so the phase races both kinds of update.
            else
                std::this_thread::yield();
        }
    });

    std::uint64_t incoherent = 0;
    const auto coherent = [&](const SharedIoTelemetrySnapshot& s) {
        const bool zero = s.pre_submit_delay_ns == 0 && s.submit_to_completion_ns == 0 &&
                          s.scheduled_to_completion_ns == 0;
        if (s.completion_timing_valid)
            return zero || (s.submit_to_completion_ns == 2 * s.pre_submit_delay_ns + 1 &&
                            s.scheduled_to_completion_ns ==
                                s.pre_submit_delay_ns + s.submit_to_completion_ns);
        // Invalid is either an overflow sample or the fail-closed contended
        // read, which carries no timing values.
        return zero || (s.pre_submit_delay_ns == kMax && s.scheduled_to_completion_ns == 0 &&
                        s.submit_to_completion_ns != 0 && s.submit_to_completion_ns % 2 == 0);
    };
    struct PhaseResult {
        std::uint64_t reads = 0;
        std::uint64_t recorder_overlaps = 0;
        std::uint64_t reset_overlaps = 0;
        std::uint64_t recorder_published = 0;
        std::uint64_t resets_published = 0;
        bool met = false;
        bool reached_target = false;
    };
    const auto hard_deadline = std::chrono::steady_clock::now() + kHardDeadline;
    const auto run_phase = [&](Phase active) {
        PhaseResult result;
        const bool watch_resets = active == Both;
        const auto reached = [&](std::uint64_t quota) {
            return result.recorder_overlaps >= quota &&
                   (!watch_resets || result.reset_overlaps >= quota);
        };
        const auto recorder_published_before =
            recorder_progress.published.load(std::memory_order_relaxed);
        const auto resets_published_before =
            reset_progress.published.load(std::memory_order_relaxed);
        phase.store(active, std::memory_order_release);
        const auto soft_budget = std::chrono::steady_clock::now() + kPhaseSoftBudget;
        std::uint32_t reads_without_overlap = 0;
        while (!reached(kTargetOverlappedReads)) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= hard_deadline || (now >= soft_budget && reached(kMinimumOverlappedReads)))
                break;
            const auto recorder_finished =
                recorder_progress.finished.load(std::memory_order_seq_cst);
            const auto reset_finished = reset_progress.finished.load(std::memory_order_seq_cst);
            const auto snapshot = telemetry.snapshot();
            const auto recorder_started = recorder_progress.started.load(std::memory_order_seq_cst);
            const auto reset_started = reset_progress.started.load(std::memory_order_seq_cst);
            ++result.reads;
            if (!coherent(snapshot))
                ++incoherent;
            // An update was in flight during this read: it started before the
            // read ended and had not finished when the read began.
            const bool raced_recorder = recorder_started > recorder_finished;
            const bool raced_reset = watch_resets && reset_started > reset_finished;
            result.recorder_overlaps += raced_recorder ? 1 : 0;
            result.reset_overlaps += raced_reset ? 1 : 0;
            if (raced_recorder || raced_reset) {
                reads_without_overlap = 0;
            } else if (++reads_without_overlap == kReadsBeforeBackoff) {
                // The writers are not getting scheduled. Give up this core
                // briefly instead of spinning the reader against its own
                // writers; a host where they do run never reaches this.
                reads_without_overlap = 0;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
        phase.store(Idle, std::memory_order_release);
        result.recorder_published =
            recorder_progress.published.load(std::memory_order_relaxed) - recorder_published_before;
        result.resets_published =
            reset_progress.published.load(std::memory_order_relaxed) - resets_published_before;
        result.met = reached(kMinimumOverlappedReads) && result.recorder_published > 0 &&
                     (!watch_resets || result.resets_published > 0);
        result.reached_target = reached(kTargetOverlappedReads);
        return result;
    };
    const auto recorder_only = run_phase(RecorderOnly);
    const auto both = run_phase(Both);
    phase.store(Stop, std::memory_order_release);
    recorder.join();
    resetter.join();

    INFO("recorder-only phase: " << recorder_only.recorder_overlaps << " overlapped of "
                                 << recorder_only.reads << " reads, "
                                 << recorder_only.recorder_published << " published");
    INFO("contended phase: " << both.recorder_overlaps << " recorder / " << both.reset_overlaps
                             << " reset overlapped of " << both.reads << " reads, "
                             << both.recorder_published << " published, " << both.resets_published
                             << " resets");
    INFO("target " << kTargetOverlappedReads << ", minimum " << kMinimumOverlappedReads
                   << " overlapped reads per watched writer");
    if (!recorder_only.reached_target || !both.reached_target)
        WARN("a phase stopped below its overlap target after the soft budget (starved host)");
    REQUIRE(recorder_only.met);
    REQUIRE(both.met);
    REQUIRE(incoherent == 0);

    REQUIRE(telemetry.record_completion_timing(5, 11));
    const auto settled = telemetry.snapshot();
    REQUIRE(settled.completion_timing_valid);
    REQUIRE(settled.scheduled_to_completion_ns == 16);
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

    // A wrapped scheduled-to-completion sum must never be accepted as a
    // healthy timing receipt. This is a deterministic negative control for
    // providers that report very large nanosecond samples.
    forged = valid;
    forged.pre_submit_delay_ns = UINT64_MAX;
    forged.submit_to_completion_ns = 1;
    forged.scheduled_to_completion_ns = 0;
    CHECK(validate_shared_io_telemetry(forged).error ==
          SharedIoTelemetryError::ScheduledCompletionBeforeComponents);

    // Controller-only producers from older snapshots use submitted_blocks as
    // their admission boundary and leave the optional admission counter zero.
    auto legacy = valid;
    legacy.admitted_blocks = 0;
    CHECK(validate_shared_io_telemetry(legacy).accepted());

    // A legacy receipt has no separate admission counter, so all terminal
    // outcomes still must be bounded by the provider submissions it reports.
    legacy = {};
    legacy.submitted_blocks = 0;
    legacy.retired_failure = 1;
    CHECK(validate_shared_io_telemetry(legacy).error ==
          SharedIoTelemetryError::RetirementsExceedSubmissions);

    constexpr SharedIoTelemetrySnapshot unconfigured{};
    STATIC_REQUIRE(validate_shared_io_telemetry(unconfigured).accepted());
}

TEST_CASE("shared IO telemetry validation rejects producer-invalid completion timing first",
          "[gpu_audio][shared_io][telemetry]") {
    // Positive control: zeroed timing that the producer marks valid is an
    // ordinary empty receipt.
    SharedIoTelemetrySnapshot zero_valid{};
    zero_valid.completion_timing_valid = true;
    CHECK(validate_shared_io_telemetry(zero_valid).accepted());

    // Planted negative: the same zeroed triple marked invalid passes every
    // component comparison, so only the validity bit can reject it.
    auto zero_invalid = zero_valid;
    zero_invalid.completion_timing_valid = false;
    CHECK(validate_shared_io_telemetry(zero_invalid).error ==
          SharedIoTelemetryError::CompletionTimingInvalid);

    // The validity bit is checked before any counter relation.
    auto also_inconsistent = zero_invalid;
    also_inconsistent.delivered_blocks = 1;
    CHECK(validate_shared_io_telemetry(also_inconsistent).error ==
          SharedIoTelemetryError::CompletionTimingInvalid);

    // The real producer path: an overflowing sample is published as invalid
    // and rejected; the next representable sample is accepted again.
    SharedIoTelemetry telemetry;
    telemetry.record_completion_timing(UINT64_MAX, 1);
    CHECK(validate_shared_io_telemetry(telemetry.snapshot()).error ==
          SharedIoTelemetryError::CompletionTimingInvalid);
    telemetry.record_completion_timing(10, 20);
    CHECK(validate_shared_io_telemetry(telemetry.snapshot()).accepted());
}
