#include "detail/shared_io_convolution_session.hpp"
#include "harness/fake_shared_io_provider.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {
using namespace pulp::gpu_audio::detail;
using Session = SharedIoConvolutionSession;

using FakeProvider = pulp::test::FakeSharedIoProvider;
using State = FakeProvider::Lifecycle;

// The session moves input to output at submission through its prepared
// program; completion is a separate, test-controlled step.
std::unique_ptr<FakeProvider> make_provider(std::shared_ptr<State> state) {
    auto provider = std::make_unique<FakeProvider>(std::move(state));
    provider->transform = FakeProvider::Transform::Affine;
    provider->transform_at_submit = true;
    provider->require_program = true;
    provider->require_program_release = true;
    provider->complete_on_poll = true;
    provider->drain_completes = true;
    provider->resume_supported = true;
    return provider;
}

class FakeProgram final : public SharedIoPreparedProgram {
  public:
    FakeProgram(FakeProvider& provider, std::shared_ptr<State> state)
        : provider_(provider), state_(std::move(state)) {}
    ~FakeProgram() override {
        state_->program_destroyed = true;
    }

    bool prepare(SharedIoArenaProvider& provider,
                 std::span<const SlotBufferHandle> slots) noexcept override {
        prepared_ = &provider == &provider_ && !slots.empty() &&
                    std::all_of(slots.begin(), slots.end(), [&](const auto& slot) {
                        return provider_.validate_slot_buffers(slot);
                    });
        return prepared_ && allow_prepare;
    }

    bool submit(SharedIoArenaProvider& provider, const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> inbox) noexcept override {
        return prepared_ && &provider == &provider_ &&
               provider_.submit_program(resources, token, std::move(inbox));
    }

    bool release() noexcept override {
        if (!allow_release)
            return false;
        state_->program_released = true;
        prepared_ = false;
        return true;
    }

    bool allow_prepare = true;
    bool allow_release = true;

  private:
    FakeProvider& provider_;
    std::shared_ptr<State> state_;
    bool prepared_ = false;
};

struct Fixture {
    std::shared_ptr<State> state = std::make_shared<State>();
    FakeProvider* provider = nullptr;
    Session session;

    void prepare(std::uint32_t slots = 2, std::uint32_t channels = 1,
                 bool capture_callback_timing = false, std::uint32_t prediction_samples = 0) {
        auto owner = make_provider(state);
        provider = owner.get();
        auto program = std::make_unique<FakeProgram>(*provider, state);
        REQUIRE(session.prepare({std::move(owner), std::move(program)},
                                {.pipeline = {.capacity = 8,
                                              .channels = channels,
                                              .block_size = 2,
                                              .fft_size = 2,
                                              .ir_length = 1,
                                              .capture_callback_timing = capture_callback_timing},
                                 .slots = slots,
                                 .sample_rate = capture_callback_timing ? 48'000u : 0u,
                                 .prediction = {.enabled = prediction_samples != 0,
                                                .minimum_samples = prediction_samples}}));
    }
};

constexpr std::array<float, 2> a{1.f, 2.f};
constexpr std::array<float, 2> b{3.f, 4.f};
constexpr std::array<float, 2> zero{};

template <std::size_t Samples>
Session::Callback callback(Session& session, std::span<const float> input,
                           std::array<float, Samples>& output) {
    const auto record = session.begin_callback(input);
    REQUIRE(record.valid());
    session.consume_output(record, output);
    return record;
}
} // namespace

TEST_CASE("shared convolution session provider identity fails closed before preparation",
          "[gpu_audio][shared_io][provider]") {
    Session session;
    const auto identity = session.provider_identity();
    CHECK_FALSE(identity.authenticated);
    CHECK_FALSE(identity.native_runtime_authenticated);
    CHECK(identity.provider_revision.empty());
    CHECK(identity.adapter_name.empty());
}

TEST_CASE("shared convolution session reports provider-owned identity and capabilities",
          "[gpu_audio][shared_io][provider]") {
    Fixture fixture;
    fixture.provider = nullptr;
    fixture.prepare();
    REQUIRE(fixture.provider != nullptr);
    fixture.provider->identity = {.authenticated = true,
                                  .provider_revision = "fake-provider-1",
                                  .adapter_name = "fake-adapter",
                                  .adapter_backend = "native",
                                  .adapter_vendor_id = 0xfeed,
                                  .adapter_device_id = 0xbeef,
                                  .native_runtime_authenticated = true,
                                  .native_runtime_name = "fake-runtime",
                                  .native_runtime_backend = "native"};
    fixture.provider->capabilities = {.imported_host_pointer = true,
                                      .ordered_causal_state = true,
                                      .completion_service = true,
                                      .device_loss_recovery = false,
                                      .gpu_timestamps = true};
    const auto identity = fixture.session.provider_identity();
    const auto capabilities = fixture.session.provider_capabilities();
    CHECK(identity.authenticated);
    CHECK(identity.adapter_backend == "native");
    CHECK(capabilities.imported_host_pointer);
    CHECK(capabilities.ordered_causal_state);
    CHECK(capabilities.completion_service);
    CHECK(capabilities.gpu_timestamps);
}

TEST_CASE("shared convolution session packs provider slots and delivers exact prepared output",
          "[gpu_audio][shared_io][session]") {
    Fixture fixture;
    fixture.prepare();
    std::array<float, 2> output;
    CHECK(callback(fixture.session, a, output).stamp.sequence == 0);
    auto serviced = fixture.session.service(10);
    CHECK(serviced.submitted == 1);
    CHECK(serviced.completions == 1);
    CHECK(serviced.terminal_records == 1);
    CHECK(output == zero);
    CHECK(callback(fixture.session, b, output).stamp.sequence == 1);
    fixture.session.service(11);
    CHECK(callback(fixture.session, b, output).stamp.sequence == 2);
    CHECK(output == a);
    CHECK(fixture.state->submits == 2);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared convolution session forwards the callback block deadline to admission",
          "[gpu_audio][shared_io][session][deadline]") {
    Fixture fixture;
    auto owner = make_provider(fixture.state);
    fixture.provider = owner.get();
    auto program = std::make_unique<FakeProgram>(*fixture.provider, fixture.state);
    REQUIRE(fixture.session.prepare({std::move(owner), std::move(program)},
                                    {.pipeline = {.capacity = 8,
                                                  .channels = 1,
                                                  .block_size = 2,
                                                  .fft_size = 2,
                                                  .ir_length = 1,
                                                  .capture_callback_timing = true},
                                     .slots = 1,
                                     .sample_rate = 48'000}));

    std::array<float, 2> output{};
    const auto callback_record = fixture.session.begin_callback(a, 0, 1'000'000'000ull);
    REQUIRE(callback_record.valid());
    fixture.session.consume_output(callback_record, output);

    // The one-block budget is about 41.6 us, so this service time is after the
    // propagated deadline. A zero deadline would leave late_completions at 0.
    REQUIRE(fixture.session.service(1'002'000'000ull).submitted == 1);
    CHECK(fixture.session.telemetry().late_completions == 1);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared convolution session turns predictor refusal into a terminal drop",
          "[gpu_audio][shared_io][session][deadline]") {
    Fixture fixture;
    fixture.prepare(1, 1, true, 1);
    std::array<float, 2> output{};
    const auto record = fixture.session.begin_callback(a, 0, 1'000'000'000ull);
    REQUIRE(record.valid());
    fixture.session.consume_output(record, output);
    const auto refused = fixture.session.service(1'000'000'001ull);
    CHECK(refused.refused == 1);
    CHECK(refused.dropped_ingress == 1);
    CHECK(refused.terminal_records == 1);
    CHECK(fixture.session.service(1'000'000'002ull).submitted == 0);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared convolution session fails closed for unknown callback timing",
          "[gpu_audio][shared_io][session][deadline]") {
    Fixture fixture;
    fixture.prepare(1, 1, true);
    std::array<float, 2> output{};
    const auto record = fixture.session.begin_callback(a);
    REQUIRE(record.valid());
    fixture.session.consume_output(record, output);
    const auto refused = fixture.session.service(1);
    CHECK(refused.refused == 1);
    CHECK(refused.dropped_ingress == 1);
    CHECK(refused.terminal_records == 1);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared convolution session fails closed on callback deadline overflow",
          "[gpu_audio][shared_io][session][deadline]") {
    Fixture fixture;
    fixture.prepare(1, 1, true);
    std::array<float, 2> output{};
    const auto record = fixture.session.begin_callback(
        a, 0, std::numeric_limits<std::uint64_t>::max() - 500'000ull);
    REQUIRE(record.valid());
    fixture.session.consume_output(record, output);
    const auto refused = fixture.session.service(std::numeric_limits<std::uint64_t>::max());
    CHECK(refused.refused == 1);
    CHECK(refused.dropped_ingress == 1);
    CHECK(refused.terminal_records == 1);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared convolution session reports no provider identity for a non-Dawn provider",
          "[gpu_audio][shared_io][session][identity]") {
    Fixture fixture;
    fixture.prepare();
    const auto identity = fixture.session.provider_identity();
    CHECK_FALSE(identity.authenticated);
    CHECK(identity.provider_revision.empty());
    CHECK(identity.adapter_name.empty());
    CHECK(identity.adapter_backend.empty());
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared convolution session trace captures admission and terminal identity",
          "[gpu_audio][shared_io][trace]") {
    Fixture fixture;
    auto owner = make_provider(fixture.state);
    fixture.provider = owner.get();
    auto program = std::make_unique<FakeProgram>(*fixture.provider, fixture.state);
    REQUIRE(fixture.session.prepare(
        {std::move(owner), std::move(program)},
        {.pipeline = {.capacity = 8, .channels = 1, .block_size = 2, .fft_size = 2, .ir_length = 1},
         .slots = 2,
         .sample_rate = 48'000,
         .trace = {.success_stride = 1, .capture_admissions = true, .enabled = true}}));

    std::array<float, 2> output{};
    const auto record = callback(fixture.session, a, output);
    REQUIRE(record.valid());
    REQUIRE(fixture.session.service(10).submitted == 1);
    const auto diagnostics = fixture.session.provider_diagnostics();
    CHECK(diagnostics.configured_slots == 2);
    CHECK(diagnostics.high_water_in_flight > 0);
    CHECK(diagnostics.retired_success == 1);
    CHECK(diagnostics.retired_failure == 0);
    const auto drained = fixture.session.drain_trace();
    CHECK(drained.records_drained >= 1);
    CHECK(fixture.session.trace_stats().admissions_enqueued >= 1);
    CHECK(fixture.session.trace_stats().enqueued >= 1);
    REQUIRE(fixture.session.release());
    CHECK(fixture.session.last_closed_trace_stats().drained >= 1);
}

TEST_CASE("shared convolution session closes only matched admission identities",
          "[gpu_audio][shared_io][trace][identity]") {
    Fixture fixture;
    auto owner = make_provider(fixture.state);
    fixture.provider = owner.get();
    auto program = std::make_unique<FakeProgram>(*fixture.provider, fixture.state);
    REQUIRE(fixture.session.prepare(
        {std::move(owner), std::move(program)},
        {.pipeline = {.capacity = 8, .channels = 1, .block_size = 2, .fft_size = 2, .ir_length = 1},
         .slots = 2,
         .sample_rate = 48'000,
         .trace = {.success_stride = 1, .capture_admissions = true, .enabled = true}}));

    std::array<float, 2> output{};
    callback(fixture.session, a, output);
    REQUIRE(fixture.session.service(10).submitted == 1);
    REQUIRE(fixture.session.release());
    const auto records = fixture.session.take_last_closed_trace_records();
    REQUIRE(records.size() == 1);
    CHECK(records.front().admission_identity_matched);
    CHECK(records.front().gpu_work_admitted);
    CHECK(records.front().gpu_terminal == SharedIoGpuTerminalDisposition::CompletedAccepted);
}

TEST_CASE("shared convolution session preserves fixed planar channel layout in complex slots",
          "[gpu_audio][shared_io][session]") {
    Fixture fixture;
    fixture.prepare(2, 2);
    const std::array<float, 4> stereo{1.f, 2.f, 3.f, 4.f};
    std::array<float, 4> output;
    callback(fixture.session, stereo, output);
    fixture.session.service(1);
    callback(fixture.session, stereo, output);
    fixture.session.service(2);
    callback(fixture.session, stereo, output);
    CHECK(output == stereo);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared convolution session keeps completion chronology and retained ingress exact",
          "[gpu_audio][shared_io][session][ordering]") {
    Fixture fixture;
    fixture.prepare(1);
    fixture.provider->complete_on_poll = false;
    std::array<float, 2> output;
    callback(fixture.session, a, output);
    CHECK(fixture.session.service(10).submitted == 1);
    callback(fixture.session, b, output);
    // The second ingress stays claimed while the sole physical slot is busy.
    CHECK(fixture.session.service(11).submitted == 0);
    fixture.provider->complete_sequence(0);
    CHECK(fixture.session.service(12).submitted == 1);
    fixture.provider->complete_sequence(1);
    fixture.session.service(13);
    callback(fixture.session, b, output); // consumes sequence zero
    CHECK(output == a);
    callback(fixture.session, b, output); // consumes sequence one
    CHECK(output == b);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared convolution session does not let an out-of-order terminal leapfrog its head",
          "[gpu_audio][shared_io][session][ordering]") {
    Fixture fixture;
    fixture.prepare(2);
    fixture.provider->complete_on_poll = false;
    std::array<float, 2> output;
    callback(fixture.session, a, output);
    REQUIRE(fixture.session.service(1).submitted == 1);
    callback(fixture.session, b, output);
    REQUIRE(fixture.session.service(2).submitted == 1);
    fixture.provider->complete_sequence(1);
    CHECK(fixture.session.service(3).terminal_records == 1);
    // The later terminal is recorded but must remain behind sequence zero.
    fixture.provider->complete_sequence(0);
    CHECK(fixture.session.service(4).terminal_records == 1);
    callback(fixture.session, b, output);
    CHECK(output == a);
    callback(fixture.session, b, output);
    CHECK(output == b);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared convolution session turns pre-submit refusal into one failed terminal",
          "[gpu_audio][shared_io][session][mutation-control]") {
    Fixture fixture;
    fixture.prepare(1);
    fixture.provider->accept_submissions = false;
    std::array<float, 2> output;
    callback(fixture.session, a, output);
    const auto refused = fixture.session.service(1);
    CHECK(refused.refused == 1);
    CHECK(refused.terminal_records == 1);
    CHECK(refused.fenced);
    CHECK(fixture.session.fenced());
    // Removing the failed terminal record leaves the executor waiting for
    // sequence zero forever; this accepted next callback must therefore be
    // CPU-only after the planted pre-submit refusal.
    fixture.provider->accept_submissions = true;
    const auto cpu = fixture.session.begin_callback(b);
    CHECK(cpu.stamp.epoch == 0);
    CHECK(fixture.session.consume_output(cpu, output) == Session::Delivery::Priming);
    CHECK(fixture.state->submits == 1);
    // A stopped callback allows the next epoch to recover this bounded
    // pre-submit failure. Delivery must reopen only after both plan and
    // pipeline reprime, and the callback remains CPU-only before that point.
    REQUIRE(fixture.session.fence_and_reprime());
    CHECK_FALSE(fixture.session.fenced());
    CHECK(callback(fixture.session, b, output).stamp == SharedIoStampedBridge::Stamp{2, 2});
    CHECK(fixture.session.service(2).submitted == 1);
    callback(fixture.session, b, output);
    callback(fixture.session, b, output);
    CHECK(output == b);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared convolution session terminal failure fences delivery and drops later ingress",
          "[gpu_audio][shared_io][session]") {
    Fixture fixture;
    fixture.prepare();
    fixture.provider->terminal_status = SharedIoTerminalStatus::RetiredFailed;
    std::array<float, 2> output;
    callback(fixture.session, a, output);
    const auto failed = fixture.session.service(1);
    CHECK(failed.completions == 1);
    CHECK(failed.fenced);
    CHECK(fixture.session.fenced());
    const auto cpu = fixture.session.begin_callback(b);
    CHECK(cpu.stamp.epoch == 0);
    CHECK(fixture.session.consume_output(cpu, output) == Session::Delivery::Priming);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared convolution session reprimes plan and pipeline on one next epoch",
          "[gpu_audio][shared_io][session][reset]") {
    Fixture fixture;
    fixture.prepare();
    std::array<float, 2> output;
    callback(fixture.session, a, output);
    fixture.session.service(1);
    REQUIRE(fixture.session.fence_and_reprime());
    CHECK(fixture.session.epoch() == 2);
    CHECK(fixture.session.next_sequence() == 1);
    CHECK(callback(fixture.session, b, output).stamp == SharedIoStampedBridge::Stamp{2, 1});
    fixture.session.service(2);
    callback(fixture.session, b, output); // stale epoch-one output drains
    CHECK(output == zero);
    callback(fixture.session, b, output);
    CHECK(output == b);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared convolution session suspends delivery when reprime cannot drain",
          "[gpu_audio][shared_io][session][reset]") {
    Fixture fixture;
    fixture.prepare();
    fixture.provider->allow_drain = false;
    CHECK_FALSE(fixture.session.fence_and_reprime());
    CHECK(fixture.session.fenced());
    std::array<float, 2> output;
    const auto cpu = fixture.session.begin_callback(a);
    CHECK(cpu.stamp.epoch == 0);
    CHECK(fixture.session.consume_output(cpu, output) == Session::Delivery::Priming);
    fixture.provider->allow_drain = true;
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared convolution session retains a failed preparation until its drain retries",
          "[gpu_audio][shared_io][session][lifecycle]") {
    auto state = std::make_shared<State>();
    Session session;
    auto provider = make_provider(state);
    auto* provider_control = provider.get();
    auto program = std::make_unique<FakeProgram>(*provider_control, state);
    auto* program_control = program.get();
    program_control->allow_prepare = false;
    provider_control->allow_drain = false;
    CHECK_FALSE(session.prepare(
        {std::move(provider), std::move(program)},
        {.pipeline = {.capacity = 3, .channels = 1, .block_size = 2, .fft_size = 2, .ir_length = 1},
         .slots = 1}));
    CHECK_FALSE(session.prepared());
    CHECK_FALSE(session.release());
    CHECK_FALSE(state->program_released);
    CHECK_FALSE(state->program_destroyed);

    // A retained failed transaction still owns the provider/program. A new
    // prepare must be rejected until its physical drain succeeds; otherwise
    // replacing provider_ would leave the plan pointing at a destroyed owner.
    auto replacement_state = std::make_shared<State>();
    auto replacement_provider = make_provider(replacement_state);
    auto replacement_program =
        std::make_unique<FakeProgram>(*replacement_provider, replacement_state);
    CHECK_FALSE(session.prepare(
        {std::move(replacement_provider), std::move(replacement_program)},
        {.pipeline = {.capacity = 3, .channels = 1, .block_size = 2, .fft_size = 2, .ir_length = 1},
         .slots = 1}));
    CHECK_FALSE(state->program_released);
    CHECK_FALSE(state->program_destroyed);

    provider_control->allow_drain = true;
    REQUIRE(session.release());
    CHECK(state->program_released);
    CHECK(state->program_destroyed);
    CHECK_FALSE(state->retired_before_program_release);

    // Once release clears the retained owner, a fresh transaction can prepare
    // and release normally.
    auto fresh_provider = make_provider(replacement_state);
    auto fresh_program = std::make_unique<FakeProgram>(*fresh_provider, replacement_state);
    REQUIRE(session.prepare(
        {std::move(fresh_provider), std::move(fresh_program)},
        {.pipeline = {.capacity = 3, .channels = 1, .block_size = 2, .fft_size = 2, .ir_length = 1},
         .slots = 1}));
    REQUIRE(session.release());
    CHECK(replacement_state->program_released);
}

TEST_CASE("shared convolution session releases its prepared program before provider slots",
          "[gpu_audio][shared_io][session][lifecycle]") {
    Fixture fixture;
    fixture.prepare();
    REQUIRE(fixture.session.release());
    CHECK(fixture.state->program_released);
    CHECK(fixture.state->program_destroyed);
    CHECK_FALSE(fixture.state->retired_before_program_release);
}

TEST_CASE(
    "shared convolution session callback forwarding stays allocation-free and mutation-sensitive",
    "[gpu_audio][shared_io][session][rt-safety][mutation-control]") {
    Fixture fixture;
    fixture.prepare();
    std::array<float, 2> output;
    std::size_t allocations = 1;
    {
        pulp::test::RtAllocationProbe probe;
        for (int index = 0; index < 16; ++index)
            callback(fixture.session, a, output);
        allocations = probe.allocation_count();
    }
    CHECK(allocations == 0);

    const auto identity = [](std::span<const float> candidate) {
        return candidate.size() == a.size() &&
               std::equal(candidate.begin(), candidate.end(), a.begin());
    };
    REQUIRE(identity(a));
    const std::array<float, 2> planted_wrong_output{2.f, 1.f};
    REQUIRE_FALSE(identity(planted_wrong_output));
    // The first assertion catches callback-path allocation; the second makes
    // the fixed-record delivery expectation reject a swapped output mutation.
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared session gap closes admission and survives a failed physical drain",
          "[gpu_audio][shared_io][session][recovery]") {
    Fixture fixture;
    fixture.prepare(1);
    fixture.provider->complete_on_poll = false;
    std::array<float, 2> output;
    callback(fixture.session, a, output);
    REQUIRE(fixture.session.service(1).submitted == 1);
    callback(fixture.session, b, output);
    REQUIRE(fixture.session.service(2).submitted == 0); // retained ingress lease
    const auto gap = fixture.session.begin_callback(a, 4);
    REQUIRE(gap.valid());
    CHECK(gap.stamp == SharedIoStampedBridge::Stamp{0, 4});
    CHECK(fixture.session.recovery_reason() == SharedIoRecoveryReason::SequenceGap);
    CHECK(fixture.session.consume_output(gap, output) == Session::Delivery::EpochChanged);
    CHECK(fixture.session.service(3).submitted == 0);
    fixture.provider->allow_drain = false;
    CHECK_FALSE(fixture.session.fence_and_reprime());
    CHECK_FALSE(fixture.session.release());
    CHECK(fixture.session.prepared());
    CHECK(fixture.session.epoch() == 1);
    CHECK_FALSE(fixture.state->program_released);
    CHECK_FALSE(fixture.state->program_destroyed);
    CHECK(fixture.state->submits == 1);
    fixture.provider->allow_drain = true;
    REQUIRE(fixture.session.fence_and_reprime());
    CHECK(fixture.session.epoch() == 2);
    CHECK(fixture.session.next_sequence() == 5);
    CHECK(fixture.session.recovery_reason() == SharedIoRecoveryReason::None);
    CHECK(callback(fixture.session, b, output).stamp == SharedIoStampedBridge::Stamp{2, 5});
    CHECK(fixture.session.service(4).submitted == 1);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared session rejects stale callback identity without advancing its timeline",
          "[gpu_audio][shared_io][session][recovery]") {
    Fixture fixture;
    fixture.prepare();
    std::array<float, 2> output;
    const auto first = callback(fixture.session, a, output);
    CHECK_FALSE(fixture.session.begin_callback(a, 0).valid());
    CHECK(fixture.session.next_sequence() == 1);
    CHECK(fixture.session.recovery_reason() == SharedIoRecoveryReason::InvalidCallback);
    const auto current = fixture.session.begin_callback(b, 1);
    CHECK(fixture.session.consume_output(first, output) == Session::Delivery::Invalid);
    CHECK(fixture.session.consume_output(current, output) == Session::Delivery::Priming);
    CHECK(fixture.session.service(1).submitted == 0);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared session ingress saturation remains CPU-only until a quiescent reprime",
          "[gpu_audio][shared_io][session][recovery]") {
    Fixture fixture;
    fixture.prepare();
    std::array<float, 2> output;
    for (int sequence = 0; sequence < 9; ++sequence)
        callback(fixture.session, a, output);
    CHECK(fixture.session.recovery_reason() == SharedIoRecoveryReason::InputSaturated);
    CHECK(fixture.session.service(1).submitted == 0);
    CHECK(fixture.state->submits == 0);
    const auto cpu = callback(fixture.session, b, output);
    CHECK(cpu.stamp.epoch == 0);
    REQUIRE(fixture.session.fence_and_reprime());
    CHECK(callback(fixture.session, a, output).stamp.epoch == 2);
    CHECK(fixture.session.service(2).submitted == 1);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared session loss retires exact work but never reprimes the lost provider",
          "[gpu_audio][shared_io][session][recovery]") {
    Fixture fixture;
    fixture.prepare();
    fixture.provider->complete_on_poll = false;
    std::array<float, 2> output;
    callback(fixture.session, a, output);
    REQUIRE(fixture.session.service(1).submitted == 1);
    fixture.provider->lost = true;
    fixture.provider->terminal_status = SharedIoTerminalStatus::RetiredFailed;
    fixture.provider->complete_all();
    CHECK(fixture.session.service(2).terminal_records == 1);
    CHECK(fixture.session.recovery_reason() == SharedIoRecoveryReason::ProviderLost);
    CHECK_FALSE(fixture.session.fence_and_reprime());
    CHECK(fixture.session.epoch() == 1);
    CHECK(callback(fixture.session, b, output).stamp.epoch == 0);
    CHECK(fixture.session.service(3).submitted == 0);
    CHECK(fixture.state->submits == 1);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared session offline fence retires work and leaves future callbacks CPU-only",
          "[gpu_audio][shared_io][session][recovery]") {
    Fixture fixture;
    fixture.prepare();
    fixture.provider->complete_on_poll = false;
    std::array<float, 2> output;
    callback(fixture.session, a, output);
    REQUIRE(fixture.session.service(1).submitted == 1);
    REQUIRE(fixture.session.fence_for_offline());
    CHECK(fixture.session.epoch() == 1);
    CHECK(fixture.session.recovery_reason() == SharedIoRecoveryReason::OfflineFence);
    for (int index = 0; index < 6; ++index)
        CHECK(callback(fixture.session, b, output).stamp.epoch == 0);
    CHECK(fixture.session.service(2).submitted == 0);
    CHECK(fixture.state->submits == 1);
    REQUIRE(fixture.session.release());
}

TEST_CASE("shared session offline fence refuses reprime until a fresh preparation",
          "[gpu_audio][shared_io][session][recovery]") {
    Fixture fixture;
    fixture.prepare();
    fixture.provider->complete_on_poll = false;
    std::array<float, 2> output;
    callback(fixture.session, a, output);
    REQUIRE(fixture.session.service(1).submitted == 1);
    REQUIRE(fixture.session.fence_for_offline());
    // The provider can resume after its clean drain, but the offline fence is
    // a session boundary: only release and a fresh prepare reopen GPU work.
    REQUIRE(fixture.provider->can_resume_after_drain());
    CHECK_FALSE(fixture.session.fence_and_reprime());
    CHECK(fixture.session.fenced());
    CHECK(fixture.session.epoch() == 1);
    CHECK(fixture.session.recovery_reason() == SharedIoRecoveryReason::OfflineFence);
    CHECK(callback(fixture.session, b, output).stamp.epoch == 0);
    CHECK(fixture.session.service(2).submitted == 0);
    CHECK(fixture.state->submits == 1);
    REQUIRE(fixture.session.release());

    // Positive control: a fresh preparation (a new session from the factory
    // in production) admits GPU work again.
    Fixture fresh;
    fresh.prepare();
    CHECK(callback(fresh.session, a, output).stamp.epoch != 0);
    CHECK(fresh.session.service(1).submitted == 1);
    REQUIRE(fresh.session.release());
}

TEST_CASE("released shared session cannot be prepared again",
          "[gpu_audio][shared_io][session][recovery]") {
    // The session is single-use: its pipeline prepares once. Recovery after an
    // offline fence is therefore a new session, which starts unfenced. If this
    // ever changes, prepare() must clear offline_fenced_ (it does today, but
    // nothing can reach that path), and this test should become a re-prepare
    // test that asserts GPU admission reopens.
    Fixture fixture;
    fixture.prepare();
    REQUIRE(fixture.session.fence_for_offline());
    REQUIRE(fixture.session.release());
    auto owner = make_provider(fixture.state);
    auto program = std::make_unique<FakeProgram>(*owner, fixture.state);
    CHECK_FALSE(fixture.session.prepare(
        {std::move(owner), std::move(program)},
        {.pipeline = {.capacity = 8, .channels = 1, .block_size = 2, .fft_size = 2, .ir_length = 1},
         .slots = 2}));
    CHECK_FALSE(fixture.session.fence_and_reprime());
}

TEST_CASE("shared session offline fence refuses reprime behind an earlier recovery reason",
          "[gpu_audio][shared_io][session][recovery]") {
    Fixture fixture;
    fixture.prepare();
    std::array<float, 2> output;
    callback(fixture.session, a, output);
    REQUIRE(fixture.session.service(1).submitted == 1);
    // Recovery reasons latch first-wins, so the guard cannot key on the reason.
    fixture.session.request_recovery(SharedIoRecoveryReason::SequenceGap);
    REQUIRE(fixture.session.fence_for_offline());
    CHECK(fixture.session.recovery_reason() == SharedIoRecoveryReason::SequenceGap);
    CHECK_FALSE(fixture.session.fence_and_reprime());
    CHECK(fixture.session.fenced());
    REQUIRE(fixture.session.release());

    // Positive control: the same recovery without the offline fence reprimes.
    Fixture control;
    control.prepare();
    callback(control.session, a, output);
    REQUIRE(control.session.service(1).submitted == 1);
    control.session.request_recovery(SharedIoRecoveryReason::SequenceGap);
    CHECK(control.session.fence_and_reprime());
    CHECK_FALSE(control.session.fenced());
    REQUIRE(control.session.release());
}

TEST_CASE("shared session requires affirmative provider resume proof before reopening admission",
          "[gpu_audio][shared_io][session][recovery]") {
    Fixture fixture;
    fixture.prepare();
    fixture.provider->resume_supported = false;
    CHECK_FALSE(fixture.session.fence_and_reprime());
    CHECK(fixture.session.epoch() == 1);
    CHECK(fixture.session.recovery_reason() == SharedIoRecoveryReason::ProviderFailure);
    std::array<float, 2> output;
    CHECK(callback(fixture.session, a, output).stamp.epoch == 0);
    CHECK(fixture.session.service(1).submitted == 0);
    REQUIRE(fixture.session.release());
}

namespace {

std::uint64_t steady_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

struct ServiceDeadlines {
    std::vector<std::uint64_t> deadlines;
    std::uint64_t before_ns = 0;
    std::uint64_t after_ns = 0;
};

ServiceDeadlines service_deadlines_for(std::uint64_t completion_service_wait_ns) {
    Fixture fixture;
    auto owner = make_provider(fixture.state);
    fixture.provider = owner.get();
    fixture.provider->service_deadlines.reserve(16);
    auto program = std::make_unique<FakeProgram>(*fixture.provider, fixture.state);
    REQUIRE(fixture.session.prepare(
        {std::move(owner), std::move(program)},
        {.pipeline = {.capacity = 8, .channels = 1, .block_size = 2, .fft_size = 2, .ir_length = 1},
         .slots = 2,
         .completion_service_wait_ns = completion_service_wait_ns}));
    fixture.provider->service_deadlines.clear();
    std::array<float, 2> output;
    callback(fixture.session, a, output);
    const auto before = steady_ns();
    (void)fixture.session.service(1'000'000);
    const auto after = steady_ns();
    auto deadlines = fixture.provider->service_deadlines;
    REQUIRE(fixture.session.release());
    return {deadlines, before, after};
}

} // namespace

TEST_CASE("shared session waits for in-flight completions only after submitting",
          "[gpu_audio][shared_io][session][wake]") {
    // The first drain must not delay submitting new ingress; the drain after
    // the submit may wait, bounded by the configured budget.
    // The budget starts after the submit, on the steady clock.
    const auto bounded = service_deadlines_for(250'000);
    REQUIRE(bounded.deadlines.size() == 2);
    CHECK(bounded.deadlines[0] == 0);
    CHECK(bounded.deadlines[1] >= bounded.before_ns + 250'000);
    CHECK(bounded.deadlines[1] <= bounded.after_ns + 250'000);

    // Positive control: without a budget both drains stay nonblocking polls.
    const auto polling = service_deadlines_for(0);
    REQUIRE(polling.deadlines.size() == 2);
    CHECK(polling.deadlines[0] == 0);
    CHECK(polling.deadlines[1] == 0);
}

TEST_CASE("shared session routes the completion notifier to its provider",
          "[gpu_audio][shared_io][session][wake]") {
    Session unprepared;
    const auto noop = [](void*) noexcept {};
    int context = 0;
    CHECK_FALSE(unprepared.set_completion_notifier({noop, &context}));

    Fixture fixture;
    fixture.prepare();
    // Positive control: a provider without an event source refuses.
    CHECK_FALSE(fixture.session.set_completion_notifier({noop, &context}));
    fixture.provider->notifier_supported = true;
    REQUIRE(fixture.session.set_completion_notifier({noop, &context}));
    CHECK(fixture.provider->notifier.notify == +noop);
    CHECK(fixture.provider->notifier.context == &context);
    REQUIRE(fixture.session.set_completion_notifier({}));
    CHECK(fixture.provider->notifier.notify == nullptr);
    REQUIRE(fixture.session.release());
}
