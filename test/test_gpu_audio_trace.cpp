#include "detail/shared_io_convolution_session.hpp"
#include "detail/shared_io_trace.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/runtime/trace.hpp>
#include <pulp/runtime/trace_session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace pulp::gpu_audio::detail;

namespace {
SharedIoTraceConfig config(std::uint32_t stride = 4) {
    SharedIoTraceConfig value;
    value.engine_id = 17;
    value.generation = 3;
    value.contract.channels = 2;
    value.contract.block_size = 32;
    value.contract.sample_rate = 48000;
    value.contract.algorithmic_lead_blocks = 2;
    value.contract.pipeline_depth = 3;
    value.contract.active_path = SharedIoPath::StagedAsync;
    value.contract.miss_policy = pulp::gpu_audio::MissPolicy::CpuFallback;
    value.contract.cpu_fallback_prepared = true;
    value.success_stride = stride;
    value.capture_admissions = true;
    value.enabled = true;
    return value;
}

class ProductTraceProvider final : public SharedIoArenaProvider {
    struct Slot {
        std::uint32_t index = 0;
        std::vector<std::byte> input;
        std::vector<std::byte> output;
        bool retired = false;
    };
    struct Pending {
        SlotToken token;
        std::shared_ptr<SharedIoTerminalInbox> inbox;
    };

  public:
    bool create_slot(std::uint32_t index, std::size_t input_bytes, std::size_t output_bytes,
                     SlotResources& resources) noexcept override {
        try {
            auto slot = std::make_unique<Slot>();
            slot->index = index;
            slot->input.resize(input_bytes);
            slot->output.resize(output_bytes);
            resources.input = slot->input.data();
            resources.input_size = input_bytes;
            resources.output = slot->output.data();
            resources.output_size = output_bytes;
            resources.opaque = slot.get();
            resources.input_lifecycle = {
                .allocated = true, .import_attempted = true, .import_succeeded = true};
            resources.output_lifecycle = resources.input_lifecycle;
            slots_.push_back(std::move(slot));
            return true;
        } catch (...) {
            return false;
        }
    }

    void retire_slot(SlotResources& resources) noexcept override {
        if (auto* slot = static_cast<Slot*>(resources.opaque))
            slot->retired = true;
        resources.input_lifecycle.dispose_observed = true;
        resources.output_lifecycle.dispose_observed = true;
    }

    void destroy_slot(SlotResources& resources) noexcept override {
        auto* slot = static_cast<Slot*>(resources.opaque);
        std::erase_if(slots_, [slot](const auto& candidate) { return candidate.get() == slot; });
        resources.input_lifecycle.host_freed = true;
        resources.output_lifecycle.host_freed = true;
        resources.opaque = nullptr;
    }

    bool acquire_slot_buffers(const SlotResources& resources,
                              SlotBufferHandle& handle) const noexcept override {
        const auto* slot = static_cast<const Slot*>(resources.opaque);
        if (!slot || slot->retired)
            return false;
        handle = {.provider = this,
                  .device = this,
                  .input_buffer = slot->input.data(),
                  .output_buffer = slot->output.data(),
                  .slot = slot->index,
                  .generation = 1,
                  .lifetime = lifetime_};
        return true;
    }

    bool validate_slot_buffers(const SlotBufferHandle& handle) const noexcept override {
        return handle.provider == this && handle.device == this && !handle.lifetime.expired() &&
               std::any_of(slots_.begin(), slots_.end(), [&](const auto& slot) {
                   return !slot->retired && handle.slot == slot->index &&
                          handle.input_buffer == slot->input.data() &&
                          handle.output_buffer == slot->output.data();
               });
    }

    bool submit(const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> inbox) noexcept override {
        return submit_program(resources, token, std::move(inbox));
    }

    void poll() noexcept override {}

    bool drain() noexcept override {
        if (!allow_drain)
            return false;
        complete_all();
        return pending_.empty();
    }

    bool submit_program(const SlotResources& resources, SlotToken token,
                        std::shared_ptr<SharedIoTerminalInbox> inbox) noexcept {
        ++submits;
        if (!accept_submissions)
            return false;
        const auto bytes = std::min(resources.input_size, resources.output_size);
        std::memcpy(resources.output, resources.input, bytes);
        pending_.push_back({token, std::move(inbox)});
        return true;
    }

    void complete_sequence(std::uint64_t sequence) noexcept {
        const auto found =
            std::find_if(pending_.begin(), pending_.end(), [sequence](const auto& item) {
                return item.token.stream_sequence == sequence;
            });
        if (found == pending_.end())
            return;
        const auto pushed = found->inbox->push(found->token, terminal_status);
        if (pushed == SharedIoTerminalInbox::PushResult::Accepted)
            pending_.erase(found);
    }

    void complete_all() noexcept {
        for (auto cursor = pending_.begin(); cursor != pending_.end();) {
            const auto pushed = cursor->inbox->push(cursor->token, terminal_status);
            if (pushed == SharedIoTerminalInbox::PushResult::Accepted)
                cursor = pending_.erase(cursor);
            else
                ++cursor;
        }
    }

    bool accept_submissions = true;
    bool allow_drain = true;
    CompletionStatus terminal_status = CompletionStatus::RetiredSuccess;
    std::uint32_t submits = 0;

  private:
    std::shared_ptr<const void> lifetime_ = std::make_shared<int>(0);
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<Pending> pending_;
};

class ProductTraceProgram final : public SharedIoPreparedProgram {
  public:
    explicit ProductTraceProgram(ProductTraceProvider& provider) : provider_(provider) {}

    bool prepare(SharedIoArenaProvider& provider,
                 std::span<const SlotBufferHandle> slots) noexcept override {
        prepared_ = &provider == &provider_ && !slots.empty() &&
                    std::all_of(slots.begin(), slots.end(), [&](const auto& slot) {
                        return provider_.validate_slot_buffers(slot);
                    });
        return prepared_;
    }

    bool submit(SharedIoArenaProvider& provider, const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> inbox) noexcept override {
        return prepared_ && &provider == &provider_ &&
               provider_.submit_program(resources, token, std::move(inbox));
    }

    bool release() noexcept override {
        prepared_ = false;
        return true;
    }

  private:
    ProductTraceProvider& provider_;
    bool prepared_ = false;
};

struct ProductTraceFixture {
    ProductTraceProvider* provider = nullptr;
    SharedIoConvolutionSession session;

    void prepare() {
        SharedIoTraceConfig trace;
        trace.engine_id = 17;
        trace.success_stride = 1;
        trace.capture_admissions = true;
        trace.enabled = true;

        auto owner = std::make_unique<ProductTraceProvider>();
        provider = owner.get();
        auto program = std::make_unique<ProductTraceProgram>(*provider);
        REQUIRE(session.prepare(
            {std::move(owner), std::move(program)},
            {.pipeline =
                 {.capacity = 2, .channels = 1, .block_size = 2, .fft_size = 2, .ir_length = 1},
             .slots = 1,
             .sample_rate = 48000,
             .requested_path = SharedIoRequest::RequireSharedHostPointer,
             .active_path = SharedIoPath::SharedHostPointer,
             .miss_policy = pulp::gpu_audio::MissPolicy::CpuFallback,
             .shared_host_pointer_capable = true,
             .cpu_fallback_prepared = true,
             .trace = trace}));
    }
};

SharedIoTraceRecord record(std::uint64_t sequence,
                           SharedIoTraceOutcome outcome = SharedIoTraceOutcome::Success) {
    SharedIoTraceRecord value;
    value.generation = 3;
    value.sequence = sequence;
    value.outcome = outcome;
    value.gpu_terminal = SharedIoGpuTerminalDisposition::CompletedAccepted;
    value.delivery = SharedIoDeliveryDisposition::None;
    value.gpu_work_admitted = true;
    value.output_eligible = false;
    value.set(SharedIoTraceStage::Scheduled, 1000 + sequence * 100);
    value.set(SharedIoTraceStage::WorkerEntry, 1010 + sequence * 100);
    value.set(SharedIoTraceStage::EncodeBegin, 1011 + sequence * 100);
    value.set(SharedIoTraceStage::EncodeEnd, 1015 + sequence * 100);
    value.set(SharedIoTraceStage::SubmitBegin, 1016 + sequence * 100);
    value.set(SharedIoTraceStage::SubmitEnd, 1018 + sequence * 100);
    value.set(SharedIoTraceStage::CompletionObserved, 1028 + sequence * 100);
    return value;
}
} // namespace

TEST_CASE("GPU audio trace records require ordered timestamps and preserve unavailable GPU time",
          "[gpu_audio][trace]") {
    auto value = record(2);
    REQUIRE(value.valid());
    REQUIRE(shared_io_trace_duration(value, SharedIoTraceStage::Scheduled,
                                     SharedIoTraceStage::CompletionObserved)
                .ns == 28);
    REQUIRE_FALSE(shared_io_trace_duration(value, SharedIoTraceStage::EncodeEnd,
                                           SharedIoTraceStage::Scheduled)
                      .available);
    value.gpu_elapsed_available = false;
    value.gpu_elapsed_ns = 0;
    REQUIRE(value.valid());
    value.set(SharedIoTraceStage::SubmitEnd, 1);
    REQUIRE_FALSE(value.valid());

    auto contradictory = record(3, SharedIoTraceOutcome::CompletionFailed);
    REQUIRE_FALSE(contradictory.valid());
    contradictory.gpu_terminal = SharedIoGpuTerminalDisposition::ProviderFailed;
    REQUIRE(contradictory.valid());

    auto late_gpu_delivery = record(4);
    late_gpu_delivery.gpu_terminal = SharedIoGpuTerminalDisposition::LateRejected;
    REQUIRE_FALSE(late_gpu_delivery.valid());
}

TEST_CASE("GPU audio trace samples successes but keeps every anomaly", "[gpu_audio][trace]") {
    SharedIoTraceRecorder recorder(config(4));
    REQUIRE(recorder.enabled());
    for (std::uint64_t sequence = 0; sequence < 12; ++sequence)
        REQUIRE(recorder.publish_worker(record(sequence)) == (sequence % 4 == 0));
    auto anomaly = record(13, SharedIoTraceOutcome::LateRejected);
    anomaly.reason = SharedIoFallbackReason::DeadlineExceeded;
    anomaly.gpu_reason = SharedIoFallbackReason::DeadlineExceeded;
    anomaly.delivery_reason = SharedIoFallbackReason::DeadlineExceeded;
    anomaly.gpu_terminal = SharedIoGpuTerminalDisposition::LateRejected;
    REQUIRE(recorder.publish_worker(anomaly));
    auto terminal_anomaly = record(14, SharedIoTraceOutcome::LateRejected);
    terminal_anomaly.reason = SharedIoFallbackReason::DeadlineExceeded;
    terminal_anomaly.gpu_reason = SharedIoFallbackReason::DeadlineExceeded;
    terminal_anomaly.gpu_terminal = SharedIoGpuTerminalDisposition::LateRejected;
    REQUIRE(recorder.publish_worker(terminal_anomaly));

    std::vector<std::uint64_t> drained;
    REQUIRE(recorder.drain_worker_records(256, [&](const SharedIoTraceRecord& value) {
        drained.push_back(value.sequence);
    }) == 5);
    REQUIRE(drained == std::vector<std::uint64_t>{0, 4, 8, 13, 14});
    const auto stats = recorder.stats();
    REQUIRE(stats.attempted == 14);
    REQUIRE(stats.sampled_out == 9);
    REQUIRE(stats.enqueued == 5);
    REQUIRE(stats.drained == 5);
}

TEST_CASE("GPU audio trace overflow is counted without blocking the producer",
          "[gpu_audio][trace]") {
    SharedIoTraceRecorder recorder(config(1));
    std::uint64_t accepted = 0;
    for (std::uint64_t sequence = 0; sequence < 300; ++sequence)
        accepted += recorder.publish_worker(record(sequence)) ? 1 : 0;
    const auto stats = recorder.stats();
    REQUIRE(stats.attempted == 300);
    REQUIRE(stats.enqueued == accepted);
    REQUIRE(stats.dropped > 0);
    REQUIRE(stats.dropped + stats.enqueued == 300);
}

TEST_CASE("GPU audio trace preserves SPSC record identity under concurrency",
          "[gpu_audio][trace][concurrency]") {
    constexpr std::uint64_t count = 10000;
    SharedIoTraceRecorder recorder(config(1));
    std::atomic<bool> producer_done{false};
    std::vector<std::uint64_t> sequences;
    sequences.reserve(count);

    std::thread producer([&] {
        for (std::uint64_t sequence = 0; sequence < count; ++sequence) {
            const auto value = record(sequence);
            while (!recorder.publish_worker(value))
                std::this_thread::yield();
        }
        producer_done.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        while (!producer_done.load(std::memory_order_acquire) ||
               recorder.stats().drained != recorder.stats().enqueued) {
            if (recorder.drain_worker_records(256, [&](const SharedIoTraceRecord& value) {
                    sequences.push_back(value.sequence);
                }) == 0)
                std::this_thread::yield();
        }
    });
    producer.join();
    consumer.join();

    REQUIRE(sequences.size() == count);
    for (std::uint64_t sequence = 0; sequence < count; ++sequence)
        REQUIRE(sequences[sequence] == sequence);
    const auto stats = recorder.stats();
    REQUIRE(stats.enqueued == count);
    REQUIRE(stats.drained == count);
}

TEST_CASE("GPU audio trace rejects generation mismatch and separates sessions",
          "[gpu_audio][trace][generation]") {
    SharedIoTraceRecorder generation_three(config(1));
    auto next_config = config(1);
    next_config.generation = 4;
    SharedIoTraceRecorder generation_four(next_config);

    auto old_record = record(7);
    auto new_record = record(8);
    new_record.generation = 4;
    REQUIRE(generation_three.publish_worker(old_record));
    REQUIRE_FALSE(generation_three.publish_worker(new_record));
    REQUIRE_FALSE(generation_four.publish_worker(old_record));
    REQUIRE(generation_four.publish_worker(new_record));

    std::vector<std::uint64_t> old_sequences;
    std::vector<std::uint64_t> new_sequences;
    REQUIRE(generation_three.drain_worker_records(256, [&](const SharedIoTraceRecord& value) {
        old_sequences.push_back(value.sequence);
    }) == 1);
    REQUIRE(generation_four.drain_worker_records(256, [&](const SharedIoTraceRecord& value) {
        new_sequences.push_back(value.sequence);
    }) == 1);
    REQUIRE(old_sequences == std::vector<std::uint64_t>{7});
    REQUIRE(new_sequences == std::vector<std::uint64_t>{8});
    REQUIRE(generation_three.stats().invalid == 1);
    REQUIRE(generation_four.stats().invalid == 1);
}

namespace {
using Kind = SharedIoTraceKind;
using Delivery = SharedIoConvolutionSession::Delivery;
std::vector<SharedIoTraceRecord> take_records(SharedIoConvolutionSession& session) {
    std::vector<SharedIoTraceRecord> records;
    session.drain_trace_records(256, [&](const auto& value) { records.push_back(value); });
    return records;
}

void drive_two_deliveries(ProductTraceFixture& fixture) {
    const std::array<float, 2> input{1.f, 2.f};
    std::array<float, 2> output{};
    for (std::uint64_t sequence = 0; sequence < 2; ++sequence) {
        auto callback = fixture.session.begin_callback(input);
        REQUIRE(callback.stamp.sequence == sequence);
        REQUIRE(fixture.session.consume_output(callback, output) == Delivery::Priming);
        REQUIRE(fixture.session.service(0).submitted == 1);
        fixture.provider->complete_sequence(sequence);
        REQUIRE(fixture.session.service(0).completions == 1);
    }
    for (std::uint64_t sequence = 2; sequence < 4; ++sequence) {
        auto callback = fixture.session.begin_callback(input);
        REQUIRE(callback.stamp.sequence == sequence);
        REQUIRE(fixture.session.consume_output(callback, output) == Delivery::Ready);
        REQUIRE(output == input);
    }
}
} // namespace

TEST_CASE("session tracing observes distinct physical terminals and actual bridge deliveries",
          "[gpu_audio][trace]") {
    ProductTraceFixture fixture;
    fixture.prepare();
    drive_two_deliveries(fixture);
    const auto records = take_records(fixture.session);
    REQUIRE(records.size() == 6);
    for (std::uint64_t sequence = 0; sequence < 2; ++sequence) {
        for (const auto kind : {Kind::Terminal, Kind::Eligible, Kind::Delivery}) {
            REQUIRE(std::count_if(records.begin(), records.end(), [&](const auto& r) {
                        return r.kind == kind && r.sequence == sequence &&
                               r.generation == fixture.session.epoch();
                    }) == 1);
        }
    }
    REQUIRE(fixture.session.trace_stats().invalid == 0);
}

TEST_CASE("session tracing does not turn a late GPU terminal into an audible delivery",
          "[gpu_audio][trace]") {
    ProductTraceFixture fixture;
    fixture.prepare();
    const std::array<float, 2> input{1.f, 2.f};
    std::array<float, 2> output{};
    for (std::uint64_t sequence = 0; sequence < 3; ++sequence) {
        auto callback = fixture.session.begin_callback(input);
        const auto delivery = fixture.session.consume_output(callback, output);
        REQUIRE(delivery == (sequence < 2 ? Delivery::Priming : Delivery::Missing));
        if (sequence == 0)
            REQUIRE(fixture.session.service(0).submitted == 1);
    }
    fixture.provider->complete_sequence(0);
    fixture.provider->accept_submissions = false;
    fixture.session.service(0);
    auto records = take_records(fixture.session);
    REQUIRE(std::count_if(records.begin(), records.end(), [](const auto& r) {
                return r.sequence == 0 && r.kind == Kind::Delivery &&
                       r.delivery == SharedIoDeliveryDisposition::SilenceDelivered;
            }) == 1);
    REQUIRE(std::count_if(records.begin(), records.end(), [](const auto& r) {
                return r.sequence == 0 && r.kind == Kind::Terminal &&
                       r.gpu_terminal == SharedIoGpuTerminalDisposition::CompletedAccepted;
            }) == 1);
    REQUIRE(fixture.session.trace_stats().invalid == 0);
}

TEST_CASE("traced callback disposition is allocation free and rejects duplicate consumption",
          "[gpu_audio][trace][rt-safety]") {
    ProductTraceFixture fixture;
    fixture.prepare();
    drive_two_deliveries(fixture);
    const std::array<float, 2> input{1.f, 2.f};
    std::array<float, 2> output{};
    (void)take_records(fixture.session);
    pulp::test::RtAllocationProbe probe;
    auto callback = fixture.session.begin_callback(input);
    const auto first = fixture.session.consume_output(callback, output);
    const auto duplicate = fixture.session.consume_output(callback, output);
    const auto allocations = probe.allocation_count();
    REQUIRE(allocations == 0);
    REQUIRE(first == Delivery::Missing);
    REQUIRE(duplicate == Delivery::Invalid);
    const auto records = take_records(fixture.session);
    REQUIRE(records.size() == 2);
}

TEST_CASE("quiescent recovery traces the real next epoch without joining old ingress",
          "[gpu_audio][trace][generation]") {
    ProductTraceFixture fixture;
    fixture.prepare();
    drive_two_deliveries(fixture);
    const auto old_epoch = fixture.session.epoch();
    REQUIRE(fixture.session.fence_and_reprime());
    REQUIRE(fixture.session.epoch() == old_epoch + 1);
    REQUIRE(fixture.session.last_closed_trace_stats().invalid == 0);
    REQUIRE(fixture.session.last_closed_trace_stats().attempted == 7);
    REQUIRE(fixture.session.service(0).dropped_ingress == 0);
    const std::array<float, 2> input{1.f, 2.f};
    std::array<float, 2> output{};
    auto callback = fixture.session.begin_callback(input);
    REQUIRE(callback.admission == SharedIoStampedBridge::Admission::Accepted);
    REQUIRE(callback.stamp.sequence == 4);
    REQUIRE(callback.stamp.epoch == old_epoch + 1);
    REQUIRE(fixture.session.consume_output(callback, output) == Delivery::Priming);
    REQUIRE(fixture.session.service(0).submitted == 1);
    REQUIRE(fixture.session.trace_stats().admissions_attempted == 1);
}

TEST_CASE("diagnostic overflow never refuses physical session submissions", "[gpu_audio][trace]") {
    ProductTraceFixture fixture;
    fixture.prepare();
    const std::array<float, 2> input{1.f, 2.f};
    std::array<float, 2> output{};
    for (std::uint64_t sequence = 0; sequence < 400; ++sequence) {
        auto callback = fixture.session.begin_callback(input);
        fixture.session.consume_output(callback, output);
        REQUIRE(fixture.session.service(0).submitted == 1);
        fixture.provider->complete_sequence(sequence);
        REQUIRE(fixture.session.service(0).completions == 1);
    }
    REQUIRE(fixture.provider->submits == 400);
    REQUIRE(fixture.session.trace_stats().dropped > 0);
    REQUIRE(fixture.session.trace_stats().admissions_dropped > 0);
    REQUIRE(fixture.session.trace_stats().invalid == 0);
}

TEST_CASE("GPU audio trace persists authoritative positive and planted invalid captures",
          "[gpu_audio][trace][perfetto]") {
    if (!pulp::runtime::kTracingEnabled)
        return;
    const auto* requested = std::getenv("PULP_GPU_AUDIO_TRACE_TEST_DIR");
    const auto directory =
        requested ? std::filesystem::path(requested) : std::filesystem::temp_directory_path();
    for (const bool negative : {false, true}) {
        const auto path = directory / (negative ? "pulp-gpu-audio-planted-duplicate.pftrace"
                                                : "pulp-gpu-audio-positive.pftrace");
        auto started = pulp::runtime::Tracing::start_exclusive({"gpu"}, path.string(), 4096);
        REQUIRE(started.status == pulp::runtime::TraceStartStatus::Started);
        REQUIRE(started.ownership);
        {
            ProductTraceFixture fixture;
            fixture.prepare();
            drive_two_deliveries(fixture);
            REQUIRE(fixture.session.release());
            const auto stats = fixture.session.last_closed_trace_stats();
            REQUIRE(stats.admissions_attempted == 2);
            REQUIRE(stats.attempted == 6);
            REQUIRE(stats.drained == 6);
            REQUIRE(stats.invalid == 0);
        }
        if (negative) {
            auto cfg = config(1);
            cfg.generation = 1;
            SharedIoTraceRecorder recorder(cfg);
            auto terminal = record(0);
            terminal.generation = 1;
            recorder.publish_admission(1, 0);
            recorder.publish_completed(terminal);
            recorder.publish_admission(1, 46);
            terminal.sequence = 47;
            recorder.publish_completed(terminal);
            SharedIoTraceRecord event;
            event.kind = Kind::Eligible;
            event.generation = 1;
            event.sequence = 48;
            recorder.publish_callback(event);
            event.kind = Kind::Delivery;
            event.output_eligible = true;
            event.delivery = SharedIoDeliveryDisposition::SilenceDelivered;
            event.sequence = 49;
            recorder.publish_callback(event);
            event.sequence = 0;
            recorder.publish_callback(event);
            drain_shared_io_trace(recorder, {}, 256);
            for (std::uint64_t sequence = 1000; sequence < 1300; ++sequence) {
                terminal = record(sequence);
                terminal.generation = 1;
                recorder.publish_completed(terminal);
            }
            REQUIRE(recorder.stats().dropped > 0);
            drain_shared_io_trace(recorder, {}, 256);
        }
        auto stopped = pulp::runtime::Tracing::stop_owned(*started.ownership);
        REQUIRE(stopped.ok);
        REQUIRE(stopped.trace_bytes > 0);
    }
}

TEST_CASE("session trace producers preserve identities across callback and service threads",
          "[gpu_audio][trace][concurrency]") {
    ProductTraceFixture fixture;
    fixture.prepare();
    constexpr std::uint64_t count = 200;
    std::atomic<std::uint64_t> callback_done{0}, service_done{0};
    std::atomic<bool> failed{false};
    std::vector<SharedIoTraceRecord> records;
    std::vector<SharedIoTraceAdmission> admissions;
    records.reserve(count * 3);
    admissions.reserve(count);
    std::thread callback([&] {
        const std::array<float, 2> input{1.f, 2.f};
        std::array<float, 2> output{};
        for (std::uint64_t sequence = 0; sequence < count; ++sequence) {
            auto current = fixture.session.begin_callback(input);
            if (!current.valid() || current.stamp.sequence != sequence)
                failed.store(true);
            auto result = fixture.session.consume_output(current, output);
            if (result != (sequence < 2 ? Delivery::Priming : Delivery::Ready))
                failed.store(true);
            callback_done.store(sequence + 1, std::memory_order_release);
            while (service_done.load(std::memory_order_acquire) < sequence + 1)
                std::this_thread::yield();
        }
    });
    std::thread worker([&] {
        for (std::uint64_t sequence = 0; sequence < count; ++sequence) {
            while (callback_done.load(std::memory_order_acquire) < sequence + 1)
                std::this_thread::yield();
            if (fixture.session.service(0).submitted != 1)
                failed.store(true);
            fixture.provider->complete_sequence(sequence);
            if (fixture.session.service(0).completions != 1)
                failed.store(true);
            fixture.session.drain_trace_records(256, [&](const auto& r) { records.push_back(r); });
            fixture.session.drain_trace_admissions(256,
                                                   [&](const auto& a) { admissions.push_back(a); });
            service_done.store(sequence + 1, std::memory_order_release);
        }
    });
    callback.join();
    worker.join();
    REQUIRE_FALSE(failed.load());
    REQUIRE(records.size() == count + 2 * (count - 2));
    REQUIRE(admissions.size() == count);
    for (std::uint64_t sequence = 0; sequence < count; ++sequence) {
        REQUIRE(admissions[sequence].sequence == sequence);
        REQUIRE(std::count_if(records.begin(), records.end(), [&](const auto& r) {
                    return r.kind == Kind::Terminal && r.sequence == sequence;
                }) == 1);
    }
    REQUIRE(fixture.session.trace_stats().invalid == 0);
    REQUIRE(fixture.session.trace_stats().dropped == 0);
}

TEST_CASE("session trace reports executor rejection of nonfinite provider history",
          "[gpu_audio][trace]") {
    ProductTraceFixture fixture;
    fixture.prepare();
    const std::array<float, 2> input{std::numeric_limits<float>::quiet_NaN(), 2.f};
    std::array<float, 2> output{};
    auto callback = fixture.session.begin_callback(input);
    fixture.session.consume_output(callback, output);
    REQUIRE(fixture.session.service(0).submitted == 1);
    fixture.provider->complete_sequence(0);
    REQUIRE(fixture.session.service(0).fenced);
    const auto records = take_records(fixture.session);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].kind == Kind::Terminal);
    REQUIRE(records[0].gpu_terminal == SharedIoGpuTerminalDisposition::ProviderFailed);
    REQUIRE(records[0].outcome == SharedIoTraceOutcome::CompletionFailed);
    REQUIRE(fixture.session.trace_stats().invalid == 0);
}

TEST_CASE("invalid callback identity cannot suppress a valid terminal or emit delivery",
          "[gpu_audio][trace]") {
    ProductTraceFixture fixture;
    fixture.prepare();
    const std::array<float, 2> input{1.f, 2.f};
    std::array<float, 2> output{};
    auto first = fixture.session.begin_callback(input);
    fixture.session.consume_output(first, output);
    REQUIRE(fixture.session.service(0).submitted == 1);
    auto forged = first;
    forged.stamp.sequence = 10000;
    REQUIRE(fixture.session.consume_output(forged, output) == Delivery::Invalid);
    fixture.provider->complete_sequence(0);
    REQUIRE(fixture.session.service(0).completions == 1);
    auto second = fixture.session.begin_callback(input);
    REQUIRE(fixture.session.consume_output(second, output) == Delivery::Priming);
    auto third = fixture.session.begin_callback(input);
    REQUIRE(fixture.session.consume_output(third, output) == Delivery::Ready);
    const auto records = take_records(fixture.session);
    REQUIRE(records.size() == 3);
    REQUIRE(
        std::none_of(records.begin(), records.end(), [](const auto& r) { return r.sequence > 0; }));
}

TEST_CASE("worker and callback trace queues publish concurrently with independent draining",
          "[gpu_audio][trace][concurrency]") {
    constexpr std::uint64_t count = 10000;
    SharedIoTraceRecorder recorder(config(1));
    std::atomic<unsigned> done{0};
    std::array<std::vector<std::uint64_t>, 2> received;
    for (auto& values : received)
        values.reserve(count);
    std::thread worker([&] {
        for (std::uint64_t sequence = 0; sequence < count; ++sequence)
            while (!recorder.publish_completed(record(sequence)))
                std::this_thread::yield();
        done.fetch_add(1, std::memory_order_release);
    });
    std::thread callback([&] {
        SharedIoTraceRecord value;
        value.kind = Kind::Delivery;
        value.generation = 3;
        value.output_eligible = true;
        value.delivery = SharedIoDeliveryDisposition::GpuDelivered;
        for (std::uint64_t sequence = 0; sequence < count; ++sequence) {
            value.sequence = sequence;
            while (!recorder.publish_callback(value))
                std::this_thread::yield();
        }
        done.fetch_add(1, std::memory_order_release);
    });
    std::thread diagnostic([&] {
        while (done.load(std::memory_order_acquire) < 2 ||
               recorder.stats().drained < recorder.stats().enqueued) {
            recorder.drain_worker_records(256, [&](const auto& value) {
                received[value.kind == Kind::Terminal ? 0 : 1].push_back(value.sequence);
            });
        }
    });
    worker.join();
    callback.join();
    diagnostic.join();
    for (const auto& values : received) {
        REQUIRE(values.size() == count);
        for (std::uint64_t sequence = 0; sequence < count; ++sequence)
            REQUIRE(values[sequence] == sequence);
    }
    REQUIRE(recorder.stats().invalid == 0);
}
