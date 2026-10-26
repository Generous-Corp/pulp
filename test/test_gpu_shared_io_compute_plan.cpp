#include "detail/shared_io_compute_plan.hpp"
#include "detail/shared_io_program_session.hpp"
#include "detail/shared_io_transport_bridge.hpp"
#include "detail/shared_spectral_result.hpp"
#include "harness/fake_shared_io_provider.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

using namespace pulp::gpu_audio::detail;

namespace {
// The plan tests' provider doubles input at submission and retires it at
// once, unless terminals are deferred to the next successful drain.
class FakeProvider final : public pulp::test::FakeSharedIoProvider {
  public:
    FakeProvider() {
        transform = Transform::Affine;
        scale = 2.0f;
        transform_at_submit = true;
        complete_in_submit = true;
        drain_completes = true;
    }

    SharedIoProviderIdentity provider_identity() const noexcept override {
        return authenticated_identity ? authenticated_fake_identity() : identity;
    }

    bool drain() noexcept override {
        if (allow_drain)
            defer_terminals = false;
        return FakeSharedIoProvider::drain();
    }

    bool authenticated_identity = false;
};

class FakePreparedProgram final : public SharedIoPreparedProgram {
  public:
    explicit FakePreparedProgram(FakeProvider& expected) : expected_(expected) {}
    ~FakePreparedProgram() override {
        expected_.lifecycle_state->program_destroyed = true;
    }

    bool prepare(SharedIoArenaProvider& provider,
                 std::span<const SlotBufferHandle> slots) noexcept override {
        prepared = &provider == &expected_ && !slots.empty();
        for (const auto& slot : slots)
            prepared = prepared && expected_.validate_slot_buffers(slot);
        return prepared && allow_prepare;
    }

    bool submit(SharedIoArenaProvider& provider, const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> inbox) noexcept override {
        if (!prepared || &provider != &expected_ || !inbox)
            return false;
        const auto values = std::min(resources.input_size, resources.output_size) / sizeof(float);
        const auto* input = reinterpret_cast<const float*>(resources.input);
        auto* output = reinterpret_cast<float*>(resources.output);
        for (std::size_t index = 0; index < values; ++index)
            output[index] = input[index] * 3.0f;
        return expected_.submit_program(resources, token, std::move(inbox),
                                        /*apply_transform=*/false);
    }

    bool release() noexcept override {
        if (!allow_release)
            return false;
        expected_.lifecycle_state->program_released = true;
        prepared = false;
        return true;
    }

    bool prepared = false;
    bool allow_prepare = true;
    bool allow_release = false;

  private:
    FakeProvider& expected_;
};
} // namespace

TEST_CASE("shared IO compute plan keeps deadline outside slot token",
          "[gpu_audio][shared_io][p2]") {
    FakeProvider provider;
    SharedIoComputePlan plan;
    REQUIRE(plan.prepare(provider,
                         {.slots = 2, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    auto input = plan.acquire_input(7, 100);
    REQUIRE(input);
    REQUIRE(input->token.stream_sequence == 7);
    REQUIRE(plan.submit({input->token, 100}));
    REQUIRE(plan.drain(101) == 1);
    auto completion = plan.pop_completion();
    REQUIRE(completion);
    CHECK(completion->token.slot.stream_sequence == 7);
    CHECK(completion->late);
    CHECK(completion->status == SharedIoArena::CompletionStatus::RetiredSuccess);
    REQUIRE(plan.release());
}

TEST_CASE("shared IO compute plan delivers a completion only under its admission ticket",
          "[gpu_audio][shared_io][p2][ticket]") {
    FakeProvider provider;
    SharedIoComputePlan plan;
    REQUIRE(plan.prepare(provider,
                         {.slots = 2, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    auto input = plan.acquire_input(7, 0);
    REQUIRE(input);
    const auto token = SharedIoComputePlan::SubmitToken::admit(input->token);
    CHECK(token.ticket.admits(input->token.preparation_epoch, 7));
    // A caller ticket that disagrees with the one the plan admitted is refused.
    auto forged = token;
    forged.ticket.generation += 1;
    CHECK_FALSE(plan.submit(forged));
    REQUIRE(plan.submit(token));
    REQUIRE(plan.drain(0) == 1);
    auto completion = plan.pop_completion();
    REQUIRE(completion);
    CHECK(completion->token.ticket == token.ticket);

    // Planted negative: the same completion carrying a ticket from another
    // generation cannot acquire the output.
    auto stale = *completion;
    stale.token.ticket.generation += 1;
    CHECK_FALSE(plan.acquire_output(stale));
    auto unaccepted = *completion;
    unaccepted.token.ticket = {};
    CHECK_FALSE(plan.acquire_output(unaccepted));

    // Positive control: the admitted ticket acquires it.
    auto lease = plan.acquire_output(*completion);
    REQUIRE(lease);
    REQUIRE(plan.release_output({lease->token}));
    REQUIRE(plan.release());
}

TEST_CASE("shared IO execution predictor is opt-in and conservative",
          "[gpu_audio][shared_io][prediction]") {
    SharedIoExecutionPredictor predictor;
    REQUIRE(predictor.admit(1000, 1001));

    predictor.configure({.enabled = true, .minimum_samples = 2, .safety_margin_ns = 100});
    predictor.observe(1000);
    REQUIRE_FALSE(predictor.estimate().ready);
    REQUIRE_FALSE(predictor.admit(1000, 2000));
    predictor.observe(1200);

    const auto estimate = predictor.estimate();
    REQUIRE(estimate.enabled);
    REQUIRE(estimate.ready);
    REQUIRE(estimate.samples == 2);
    REQUIRE(estimate.mean_ns >= 1000);
    REQUIRE(estimate.conservative_ns > estimate.mean_ns);
    REQUIRE_FALSE(predictor.admit(10'000, 10'000 + estimate.conservative_ns - 1));
    REQUIRE(predictor.admit(10'000, 10'000 + estimate.conservative_ns));

    // A contention spike must not be hidden by the EWMA. The bound remains at
    // or above every observed wall-clock sample, with no GPU timestamp claim.
    predictor.observe(5'000);
    REQUIRE(predictor.estimate().conservative_ns >= 5'100);

    predictor.configure({});
    REQUIRE_FALSE(predictor.estimate().enabled);
    REQUIRE(predictor.admit(10'000, 10'001));
}

TEST_CASE("shared IO plan prediction admits, refuses, and resets at lifecycle boundaries",
          "[gpu_audio][shared_io][prediction]") {
    FakeProvider provider;
    SharedIoComputePlan plan;
    REQUIRE(plan.prepare(provider,
                         {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    plan.set_prediction_policy({.enabled = true, .minimum_samples = 1});

    // A failed terminal is retained in the lifecycle receipt but cannot train
    // the execution predictor.
    provider.terminal_status = SharedIoArena::CompletionStatus::RetiredFailed;
    auto failed_input = plan.acquire_input(1, 0);
    REQUIRE(failed_input);
    REQUIRE(plan.submit({failed_input->token, 0}));
    REQUIRE(plan.drain(0) == 1);
    auto failed = plan.pop_completion();
    REQUIRE(failed);
    CHECK(plan.prediction_estimate().samples == 0);
    REQUIRE(plan.discard_completion(*failed));
    REQUIRE(plan.reprime_when_quiescent());

    provider.terminal_status = SharedIoArena::CompletionStatus::RetiredSuccess;
    auto successful_input = plan.acquire_input(2, 0);
    REQUIRE(successful_input);
    REQUIRE(plan.submit({successful_input->token, 0}));
    REQUIRE(plan.drain(0) == 1);
    auto successful = plan.pop_completion();
    REQUIRE(successful);
    auto successful_output = plan.acquire_output(*successful);
    REQUIRE(successful_output);
    REQUIRE(plan.release_output({successful_output->token}));
    CHECK(plan.prediction_estimate().ready);
    CHECK(plan.telemetry().prediction_samples == 1);

    // A calibrated future deadline is admitted.
    const auto future_deadline =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count()) +
        1'000'000'000;
    auto admitted_input = plan.acquire_input(3, future_deadline);
    REQUIRE(admitted_input);
    REQUIRE(plan.cancel({admitted_input->token, future_deadline}));

    // An expired deadline is refused, which lets the caller take its CPU
    // fallback path without consuming a provider slot.
    const auto expired_deadline = future_deadline - 2'000'000'000;
    CHECK_FALSE(plan.acquire_input(4, expired_deadline));
    CHECK(plan.telemetry().prediction_refusals == 1);
    CHECK(plan.telemetry().last_prediction_refusal == SharedIoPredictionRefusal::DeadlineExpired);
    CHECK(plan.telemetry().last_fallback_reason == SharedIoFallbackReason::PredictionRefused);
    REQUIRE(plan.reprime_when_quiescent());
    CHECK(plan.prediction_estimate().samples == 0);
    CHECK(plan.prediction_estimate().enabled);

    // Disabling the policy restores the established admission behavior.
    plan.set_prediction_policy({});
    auto disabled_input = plan.acquire_input(4, 1);
    REQUIRE(disabled_input);
    REQUIRE(plan.cancel({disabled_input->token, 1}));
    REQUIRE(plan.release());
}

TEST_CASE("G3 calibration packet binds identity and synchronized timestamps",
          "[gpu_audio][shared_io][prediction][g3]") {
    FakeProvider provider;
    provider.authenticated_identity = true;
    SharedIoComputePlan plan;
    const SharedIoCalibrationIdentity identity{.model = 11,
                                               .block_frames = 64,
                                               .lead_blocks = 2,
                                               .slots = 1,
                                               .batch_width = 1,
                                               .policy = 7,
                                               .load = 3};
    REQUIRE(plan.prepare(provider,
                         {.slots = 1,
                          .input_bytes_per_slot = 16,
                          .output_bytes_per_slot = 16,
                          .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer,
                          .calibration_identity = identity,
                          .authenticated_calibration = true,
                          .calibration_source_revision = "test-g3-v1"}));
    plan.set_prediction_policy({.enabled = true, .minimum_samples = 1});
    auto input = plan.acquire_input(1, 0);
    REQUIRE(input);
    REQUIRE(plan.submit({input->token, 0}));
    REQUIRE(plan.drain(0) == 1);
    auto completion = plan.pop_completion();
    REQUIRE(completion);
    auto output = plan.acquire_output(*completion);
    REQUIRE(output);
    REQUIRE(plan.release_output({output->token}));
    const auto observations = plan.take_calibration_observations();
    REQUIRE(observations.size() == 1);
    CHECK(observations.front().identity == identity);
    CHECK(observations.front().provider_authenticated);
    CHECK(observations.front().valid());
    CHECK(observations.front().visible_ns >= observations.front().terminal_ns);
    CHECK(plan.telemetry().calibration_rows == 1);
    REQUIRE(plan.release());
}

TEST_CASE("G3 calibration packet counts predictor under-estimates",
          "[gpu_audio][shared_io][prediction][g3]") {
    const SharedIoCalibrationIdentity identity{.model = 1,
                                               .block_frames = 64,
                                               .lead_blocks = 1,
                                               .slots = 2,
                                               .batch_width = 1,
                                               .policy = 2,
                                               .load = 1};
    SharedIoExecutionPredictor predictor;
    predictor.configure({.enabled = true, .minimum_samples = 1});
    predictor.bind_identity(identity);
    SharedIoCalibrationObservation observation{.identity = identity,
                                               .provenance = {.source_revision = "test-g3-v1",
                                                              .provider_revision = "provider-v1",
                                                              .adapter_name = "adapter",
                                                              .adapter_backend = "backend",
                                                              .adapter_vendor_id = 1,
                                                              .adapter_device_id = 1,
                                                              .native_runtime_name = "native",
                                                              .native_runtime_backend = "backend",
                                                              .native_runtime_authenticated = true},
                                               .generation = 1,
                                               .sequence = 1,
                                               .worker_ns = 10,
                                               .submitted_ns = 20,
                                               .terminal_ns = 120,
                                               .visible_ns = 130,
                                               .predicted_ns = 1,
                                               .provider_authenticated = true,
                                               .terminal_success = true,
                                               .output_acquired = true,
                                               .output_released = true};
    REQUIRE(observation.valid());
    predictor.observe(observation);
    CHECK(predictor.estimate().underestimates == 1);
    CHECK(predictor.estimate().conservative_ns >= observation.execution_ns());
    auto cross_cell = observation;
    cross_cell.identity.load += 1;
    CHECK_FALSE(predictor.observe(cross_cell));
    CHECK(predictor.estimate().rejected_observations == 1);
    observation.visible_ns = observation.terminal_ns - 1;
    CHECK_FALSE(observation.timestamps_synchronized());
    CHECK_FALSE(observation.valid());
    observation.visible_ns = 130;
    observation.output_released = false;
    CHECK_FALSE(observation.valid());
    observation.output_released = true;
    observation.terminal_success = false;
    CHECK_FALSE(observation.valid());
}

TEST_CASE("G3 calibration packet refuses stale and unauthenticated cells and records CPU fallback",
          "[gpu_audio][shared_io][prediction][g3]") {
    const SharedIoCalibrationIdentity identity{.model = 1,
                                               .block_frames = 64,
                                               .lead_blocks = 1,
                                               .slots = 1,
                                               .batch_width = 1,
                                               .policy = 2,
                                               .load = 1};
    FakeProvider unauthenticated;
    SharedIoComputePlan refused;
    REQUIRE(refused.prepare(unauthenticated, {.slots = 1,
                                              .input_bytes_per_slot = 16,
                                              .output_bytes_per_slot = 16,
                                              .calibration_identity = identity,
                                              .authenticated_calibration = true,
                                              .calibration_source_revision = "test-g3-v1"}));
    CHECK_FALSE(refused.acquire_input(1, 0));
    CHECK(refused.telemetry().last_prediction_refusal ==
          SharedIoPredictionRefusal::MissingCalibration);
    CHECK(refused.telemetry().last_fallback_reason == SharedIoFallbackReason::CalibrationMissing);
    CHECK(refused.telemetry().cpu_fallbacks == 1);
    refused.record_cpu_fallback(SharedIoFallbackReason::DeadlineExceeded);
    CHECK(refused.telemetry().last_fallback_reason == SharedIoFallbackReason::DeadlineExceeded);
    REQUIRE(refused.release());

    FakeProvider authenticated;
    authenticated.authenticated_identity = true;
    SharedIoComputePlan stale;
    REQUIRE(stale.prepare(authenticated, {.slots = 1,
                                          .input_bytes_per_slot = 16,
                                          .output_bytes_per_slot = 16,
                                          .calibration_identity = identity,
                                          .authenticated_calibration = true,
                                          .calibration_max_age_ns = 1,
                                          .calibration_source_revision = "test-g3-v1"}));
    stale.set_prediction_policy({.enabled = true, .minimum_samples = 1});
    auto warmup = stale.acquire_input(1, 0);
    REQUIRE(warmup);
    REQUIRE(stale.submit({warmup->token, 0}));
    REQUIRE(stale.drain(0) == 1);
    auto done = stale.pop_completion();
    REQUIRE(done);
    auto stale_output = stale.acquire_output(*done);
    REQUIRE(stale_output);
    REQUIRE(stale.release_output({stale_output->token}));
    authenticated.authenticated_identity = false;
    CHECK_FALSE(stale.acquire_input(2, 0));
    CHECK(stale.telemetry().last_prediction_refusal ==
          SharedIoPredictionRefusal::MissingCalibration);
    authenticated.authenticated_identity = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK_FALSE(stale.acquire_input(3, 0));
    CHECK(stale.telemetry().last_prediction_refusal == SharedIoPredictionRefusal::StaleCalibration);
    CHECK(stale.telemetry().last_fallback_reason == SharedIoFallbackReason::CalibrationStale);
    REQUIRE(stale.release());
}

TEST_CASE("G3 receipt overflow poisons a warmed predictor and refuses admission",
          "[gpu_audio][shared_io][prediction][g3][adversarial]") {
    FakeProvider provider;
    provider.authenticated_identity = true;
    const SharedIoCalibrationIdentity identity{.model = 9,
                                               .block_frames = 64,
                                               .lead_blocks = 1,
                                               .slots = 1,
                                               .batch_width = 1,
                                               .policy = 2,
                                               .load = 1};
    SharedIoComputePlan plan;
    REQUIRE(plan.prepare(provider, {.slots = 1,
                                    .input_bytes_per_slot = 16,
                                    .output_bytes_per_slot = 16,
                                    .calibration_identity = identity,
                                    .authenticated_calibration = true,
                                    .calibration_source_revision = "test-g3-overflow"}));
    plan.set_prediction_policy({.enabled = true, .minimum_samples = 1});
    for (std::uint64_t sequence = 1; sequence <= SharedIoComputePlan::kLifecycleReceiptCapacity;
         ++sequence) {
        auto input = plan.acquire_input(sequence, 0);
        REQUIRE(input);
        REQUIRE(plan.submit({input->token, 0}));
        REQUIRE(plan.drain(0) == 1);
        auto completion = plan.pop_completion();
        REQUIRE(completion);
        auto output = plan.acquire_output(*completion);
        REQUIRE(output);
        REQUIRE(plan.release_output({output->token}));
    }
    REQUIRE(plan.prediction_estimate().samples > 0);
    CHECK_FALSE(plan.acquire_input(SharedIoComputePlan::kLifecycleReceiptCapacity + 1, 0));
    CHECK(plan.lifecycle_receipt_overflow());
    CHECK(plan.telemetry().calibration_invalidated);
    CHECK(plan.telemetry().last_prediction_refusal ==
          SharedIoPredictionRefusal::MissingCalibration);
    CHECK(plan.prediction_estimate().samples == 0);
    CHECK_FALSE(plan.acquire_input(SharedIoComputePlan::kLifecycleReceiptCapacity + 2, 0));
    REQUIRE(plan.release());
}

TEST_CASE("shared IO compute plan classifies completion after bounded wake",
          "[gpu_audio][shared_io][p2]") {
    FakeProvider provider;
    SharedIoComputePlan plan;
    REQUIRE(plan.prepare(provider,
                         {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    const auto now =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    auto input = plan.acquire_input(9, now + 1);
    REQUIRE(input);
    REQUIRE(plan.submit({input->token, now + 1}));
    REQUIRE(plan.drain_until(now, now + 1'000'000) == 1);
    const auto completion = plan.pop_completion();
    REQUIRE(completion);
    CHECK(completion->late);
    REQUIRE(plan.release());
}

TEST_CASE("shared IO program session owns generic provider lifecycle",
          "[gpu_audio][shared_io][p2]") {
    auto provider = std::make_unique<FakeProvider>();
    const auto lifecycle_state = provider->lifecycle_state;
    auto program = std::make_unique<FakePreparedProgram>(*provider);
    auto* control = program.get();
    control->allow_release = true;

    SharedIoProgramSession session;
    CHECK_FALSE(
        session.prepare({}, {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    REQUIRE(session.prepare({std::move(provider), std::move(program)},
                            {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    REQUIRE(session.prepared());
    auto input = session.acquire_input(7, 100);
    REQUIRE(input);
    reinterpret_cast<float*>(input->bytes.data())[0] = 2.0f;
    REQUIRE(session.submit({input->token, 100}));
    REQUIRE(session.service_until(100, 200) == 1);
    auto completion = session.pop_completion();
    REQUIRE(completion);
    auto output = session.acquire_output(*completion);
    REQUIRE(output);
    CHECK(reinterpret_cast<const float*>(output->bytes.data())[0] == 6.0f);
    REQUIRE(session.release_output({output->token}));
    output.reset();
    REQUIRE(session.release());
    CHECK(lifecycle_state->program_released);
}

TEST_CASE("empty shared IO program session service paths are inert",
          "[gpu_audio][shared_io][lifecycle]") {
    SharedIoProgramSession session;
    CHECK_FALSE(session.prepared());
    CHECK(session.service(1) == 0);
    CHECK(session.service_until(1, 2) == 0);
    CHECK_FALSE(session.acquire_input(1, 2));
    CHECK_FALSE(session.pop_completion());
    CHECK_FALSE(session.acquire_output({}));
    CHECK_FALSE(session.release_output({}));
    CHECK_FALSE(session.expire_delivery({}));
    CHECK_FALSE(session.discard_completion({}));
    CHECK_FALSE(session.reprime_when_quiescent());
    CHECK(session.release());
}

TEST_CASE("shared IO arena rejects invalid backing lifecycle facts",
          "[gpu_audio][shared_io][arena]") {
    {
        FakeProvider provider;
        provider.invalid_lifecycle = true;
        SharedIoArena arena;
        CHECK_FALSE(arena.prepare(
            provider, {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    }
    {
        FakeProvider provider;
        provider.storage_kind = SharedIoArenaProvider::StorageKind::Staged;
        auto config = SharedIoArena::Config{.slots = 1,
                                            .input_bytes_per_slot = 16,
                                            .output_bytes_per_slot = 16,
                                            .storage_kind = provider.storage_kind};
        SharedIoArena arena;
        REQUIRE(arena.prepare(provider, config));
        REQUIRE(arena.release());
    }
    {
        FakeProvider provider;
        provider.storage_kind = static_cast<SharedIoArenaProvider::StorageKind>(0xff);
        SharedIoArena arena;
        CHECK_FALSE(arena.prepare(provider, {.slots = 1,
                                             .input_bytes_per_slot = 16,
                                             .output_bytes_per_slot = 16,
                                             .storage_kind = provider.storage_kind}));
    }
}

TEST_CASE("shared IO provider compatibility defaults remain explicit",
          "[gpu_audio][shared_io][lifecycle]") {
    FakeProvider provider;
    SharedIoArenaProvider& base = provider;
    base.service_until(0);
    CHECK(base.service_wait_ns() == 0);
    CHECK_FALSE(base.device_lost());
    CHECK_FALSE(base.can_resume_after_drain());
}

TEST_CASE("healthy spectral result copies exact output and releases its slot",
          "[gpu_audio][shared_io][spectral]") {
    auto provider = std::make_unique<FakeProvider>();
    auto program = std::make_unique<FakePreparedProgram>(*provider);
    program->allow_release = true;
    SharedIoProgramSession session;
    REQUIRE(session.prepare({std::move(provider), std::move(program)},
                            {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    auto input = session.acquire_input(8, 0);
    REQUIRE(input);
    const std::array<float, 4> source{1, 2, 3, 4};
    std::memcpy(input->bytes.data(), source.data(), sizeof(source));
    REQUIRE(session.submit({input->token, 0}));
    REQUIRE(session.service(0) == 1);
    bool poisoned = false;
    std::uint64_t copied_bytes = 0;
    std::array<float, 4> output{};
    auto result = receive_shared_spectral_result(session, 9, poisoned, output, 4, copied_bytes);
    REQUIRE(result);
    CHECK(result->delivered);
    CHECK(result->epoch == 9);
    CHECK(result->sequence == 8);
    CHECK_FALSE(poisoned);
    CHECK(copied_bytes == sizeof(output));
    CHECK(output == std::array<float, 4>{3, 6, 9, 12});
    REQUIRE(session.release());
}

TEST_CASE("spectral failure poisons later physical successes until recreation",
          "[gpu_audio][shared_io][spectral]") {
    auto provider = std::make_unique<FakeProvider>();
    auto* control = provider.get();
    auto program = std::make_unique<FakePreparedProgram>(*provider);
    program->allow_release = true;
    SharedIoProgramSession session;
    REQUIRE(session.prepare({std::move(provider), std::move(program)},
                            {.slots = 2, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    control->terminal_status = SharedIoTerminalStatus::RetiredFailed;
    auto failed_input = session.acquire_input(40, 0);
    REQUIRE(failed_input);
    std::fill(failed_input->bytes.begin(), failed_input->bytes.end(), std::byte{});
    REQUIRE(session.submit({failed_input->token, 0}));
    control->terminal_status = SharedIoTerminalStatus::RetiredSuccess;
    auto later_input = session.acquire_input(41, 0);
    REQUIRE(later_input);
    std::fill(later_input->bytes.begin(), later_input->bytes.end(), std::byte{});
    reinterpret_cast<float*>(later_input->bytes.data())[0] = 7.0f;
    REQUIRE(session.submit({later_input->token, 0}));
    REQUIRE(session.service(0) == 2);
    bool poisoned = false;
    std::uint64_t copied_bytes = 0;
    std::array<float, 4> output{99, 99, 99, 99};
    auto first = receive_shared_spectral_result(session, 7, poisoned, output, 4, copied_bytes);
    REQUIRE(first);
    CHECK(first->sequence == 40);
    CHECK_FALSE(first->delivered);
    CHECK(poisoned);
    auto second = receive_shared_spectral_result(session, 7, poisoned, output, 4, copied_bytes);
    REQUIRE(second);
    CHECK(second->sequence == 41);
    CHECK(second->epoch == 7);
    CHECK_FALSE(second->delivered);
    CHECK(copied_bytes == 0);
    CHECK(output == std::array<float, 4>{99, 99, 99, 99});
    CHECK_FALSE(receive_shared_spectral_result(session, 7, poisoned, output, 4, copied_bytes));
    // Both discarded results released their CPU claims; physical release is
    // still possible even though this epoch can never deliver again.
    REQUIRE(session.release());
}

TEST_CASE("spectral result rejects short output and quarantines undersized provider output",
          "[gpu_audio][shared_io][spectral]") {
    auto provider = std::make_unique<FakeProvider>();
    auto program = std::make_unique<FakePreparedProgram>(*provider);
    program->allow_release = true;
    SharedIoProgramSession session;
    REQUIRE(session.prepare({std::move(provider), std::move(program)},
                            {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    auto input = session.acquire_input(50, 0);
    REQUIRE(input);
    std::fill(input->bytes.begin(), input->bytes.end(), std::byte{});
    REQUIRE(session.submit({input->token, 0}));
    REQUIRE(session.service(0) == 1);
    bool failed = false;
    std::uint64_t copied = 0;
    std::array<float, 5> short_output{};
    CHECK_FALSE(receive_shared_spectral_result(session, 2, failed,
                                               std::span<float>(short_output).first(3), 4, copied));
    CHECK_FALSE(failed);
    auto result = receive_shared_spectral_result(session, 2, failed, short_output, 5, copied);
    REQUIRE(result);
    CHECK_FALSE(result->delivered);
    CHECK(failed);
    CHECK(copied == 0);
    REQUIRE(session.release());
}

TEST_CASE("program release observer sees final retirements only after physical barrier",
          "[gpu_audio][shared_io][spectral]") {
    auto provider = std::make_unique<FakeProvider>();
    auto* control = provider.get();
    const auto state = provider->lifecycle_state;
    control->defer_terminals = true;
    auto program = std::make_unique<FakePreparedProgram>(*provider);
    program->allow_release = true;
    SharedIoProgramSession session;
    REQUIRE(session.prepare({std::move(provider), std::move(program)},
                            {.slots = 2, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    auto input = session.acquire_input(1, 0);
    REQUIRE(input);
    std::fill(input->bytes.begin(), input->bytes.end(), std::byte{});
    REQUIRE(session.submit({input->token, 0}));
    CHECK(state->retired == 0);
    struct Observation {
        unsigned calls = 0;
        std::uint64_t retired = 0;
        bool provider_alive = false;
    } observed;
    SharedIoProgramSession::ReleaseObserver observer{
        &observed, [](void* context, const SharedIoArenaProvider& base) noexcept {
            const auto& provider = static_cast<const FakeProvider&>(base);
            auto& result = *static_cast<Observation*>(context);
            ++result.calls;
            result.retired = provider.lifecycle_state->retired;
            result.provider_alive = !provider.lifecycle_state->provider_destroyed;
        }};
    control->allow_drain = false;
    CHECK_FALSE(session.release(observer));
    CHECK(observed.calls == 0);
    CHECK_FALSE(state->provider_destroyed);
    control->allow_drain = true;
    REQUIRE(session.release(observer));
    CHECK(observed.calls == 1);
    CHECK(observed.retired == 1);
    CHECK(observed.provider_alive);
    CHECK(state->provider_destroyed);
    REQUIRE(session.release(observer));
    CHECK(observed.calls == 1);
}

TEST_CASE("shared IO session retains ownership when release barrier fails",
          "[gpu_audio][shared_io][lifecycle]") {
    auto provider = std::make_unique<FakeProvider>();
    auto* control = provider.get();
    auto state = provider->lifecycle_state;
    auto program = std::make_unique<FakePreparedProgram>(*provider);
    program->allow_release = true;
    SharedIoProgramSession session;
    REQUIRE(session.prepare({std::move(provider), std::move(program)},
                            {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    control->allow_drain = false;
    CHECK_FALSE(session.release_to_owner());
    CHECK(session.prepared());
    CHECK_FALSE(state->provider_destroyed);
    control->allow_drain = true;
    auto owner = session.release_to_owner();
    REQUIRE(owner);
    CHECK_FALSE(session.prepared());
    CHECK_FALSE(state->provider_destroyed);
    owner.reset();
    CHECK(state->provider_destroyed);
}

TEST_CASE("shared IO prepared program retains generic lifecycle and releases before slots",
          "[gpu_audio][shared_io][p2]") {
    FakeProvider provider;
    provider.require_program_release = true;
    auto program = std::make_unique<FakePreparedProgram>(provider);
    auto* control = program.get();
    SharedIoComputePlan plan;
    REQUIRE(plan.prepare(provider,
                         {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16},
                         std::move(program)));

    auto input = plan.acquire_input(9, 100);
    REQUIRE(input);
    reinterpret_cast<float*>(input->bytes.data())[0] = 1.5f;
    REQUIRE(plan.submit({input->token, 100}));
    REQUIRE(plan.drain(100) == 1);
    auto completion = plan.pop_completion();
    REQUIRE(completion);
    auto output = plan.acquire_output(*completion);
    REQUIRE(output);
    CHECK(reinterpret_cast<const float*>(output->bytes.data())[0] == 4.5f);
    REQUIRE(plan.release_output({output->token}));
    output.reset();

    CHECK_FALSE(plan.release());
    CHECK(plan.prepared());
    CHECK_FALSE(provider.lifecycle_state->program_released);
    CHECK_FALSE(provider.lifecycle_state->retired_before_program_release);
    control->allow_release = true;
    REQUIRE(plan.release());
    CHECK(provider.lifecycle_state->program_released);
    CHECK_FALSE(provider.lifecycle_state->retired_before_program_release);
}

TEST_CASE("shared IO prepared program keeps failed preparation alive until cleanup succeeds",
          "[gpu_audio][shared_io][p2]") {
    FakeProvider provider;
    provider.require_program_release = true;
    auto program = std::make_unique<FakePreparedProgram>(provider);
    auto* control = program.get();
    control->allow_prepare = false;
    SharedIoComputePlan plan;
    CHECK_FALSE(plan.prepare(provider,
                             {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16},
                             std::move(program)));
    CHECK(plan.prepared());
    CHECK_FALSE(provider.lifecycle_state->program_released);
    CHECK_FALSE(provider.lifecycle_state->retired_before_program_release);

    control->allow_release = true;
    REQUIRE(plan.release());
    CHECK(provider.lifecycle_state->program_released);
    CHECK_FALSE(provider.lifecycle_state->retired_before_program_release);
}

TEST_CASE("shared IO destructor quarantines a program when release cannot prove safety",
          "[gpu_audio][shared_io][p2]") {
    FakeProvider provider;
    provider.require_program_release = true;
    FakePreparedProgram* control = nullptr;
    {
        auto program = std::make_unique<FakePreparedProgram>(provider);
        control = program.get();
        SharedIoComputePlan plan;
        REQUIRE(plan.prepare(provider,
                             {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16},
                             std::move(program)));
        // allow_release remains false. Destruction must quarantine the complete
        // transaction instead of destroying the program or retiring its slots.
    }
    REQUIRE(control != nullptr);
    CHECK_FALSE(provider.lifecycle_state->program_released);
    CHECK_FALSE(provider.lifecycle_state->program_destroyed);
    CHECK_FALSE(provider.lifecycle_state->retired_before_program_release);
    REQUIRE(provider.live_slot_count() == 1);

    // Test-only reclamation after observing the quarantine. Production has no
    // recovery claim once an owner is destructed with an unproven barrier.
    control->allow_release = true;
    REQUIRE(control->release());
    delete control;
    provider.cleanup_abandoned_for_test();
    CHECK(provider.lifecycle_state->program_destroyed);
    CHECK(provider.live_slot_count() == 0);
    CHECK_FALSE(provider.lifecycle_state->retired_before_program_release);
}

TEST_CASE("shared IO bridge never leapfrogs a delayed head", "[gpu_audio][shared_io][p2]") {
    SharedIoTransportBridge bridge;
    REQUIRE(bridge.prepare(3, 1, 10));
    SharedIoComputePlan::Completion second{
        {{1, 1, 11, 1}, 100}, SharedIoArena::CompletionStatus::RetiredSuccess, false};
    SharedIoComputePlan::Completion first{
        {{0, 1, 10, 1}, 100}, SharedIoArena::CompletionStatus::RetiredSuccess, true};
    REQUIRE(bridge.record(second));
    CHECK_FALSE(bridge.collect_next());
    REQUIRE(bridge.record(first));
    auto result = bridge.collect_next();
    REQUIRE(result);
    CHECK(result->sequence == 10);
    CHECK(result->disposition == SharedIoTransportBridge::Disposition::SuppressLate);
    result = bridge.collect_next();
    REQUIRE(result);
    CHECK(result->sequence == 11);
    CHECK(result->disposition == SharedIoTransportBridge::Disposition::Deliver);
}

TEST_CASE("shared IO bridge fences later work after a failed chronological head",
          "[gpu_audio][shared_io][p2]") {
    SharedIoTransportBridge bridge;
    REQUIRE(bridge.prepare(3, 4, 40));
    SharedIoComputePlan::Completion successor{
        {{1, 4, 41, 1}, 100}, SharedIoArena::CompletionStatus::RetiredSuccess, false};
    SharedIoComputePlan::Completion failure{
        {{0, 4, 40, 1}, 100}, SharedIoArena::CompletionStatus::RetiredFailed, false};
    REQUIRE(bridge.record(successor));
    REQUIRE(bridge.record(failure));

    auto result = bridge.collect_next();
    REQUIRE(result);
    CHECK(result->sequence == 40);
    CHECK(result->disposition == SharedIoTransportBridge::Disposition::Reprime);
    CHECK(bridge.reprime_required());
    CHECK(bridge.pending() == 0);
    CHECK_FALSE(bridge.collect_next());
    CHECK_FALSE(bridge.record(successor));

    bridge.reset(5, 50);
    CHECK_FALSE(bridge.reprime_required());
    SharedIoComputePlan::Completion stale{
        {{0, 4, 50, 1}, 100}, SharedIoArena::CompletionStatus::RetiredSuccess, false};
    SharedIoComputePlan::Completion reprime{
        {{0, 5, 50, 1}, 100}, SharedIoArena::CompletionStatus::RetiredSuccess, false};
    CHECK_FALSE(bridge.record(stale));
    REQUIRE(bridge.record(reprime));
    result = bridge.collect_next();
    REQUIRE(result);
    CHECK(result->disposition == SharedIoTransportBridge::Disposition::Deliver);
}

TEST_CASE("shared IO compute plan exposes retired output and cancels refused leases",
          "[gpu_audio][shared_io][p2]") {
    FakeProvider provider;
    SharedIoComputePlan plan;
    REQUIRE(plan.prepare(provider,
                         {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    auto input = plan.acquire_input(3, 0);
    REQUIRE(input);
    auto* samples = reinterpret_cast<float*>(input->bytes.data());
    samples[0] = 1.5f;
    REQUIRE(plan.submit({input->token, 0}));
    REQUIRE(plan.drain(0) == 1);
    auto completion = plan.pop_completion();
    REQUIRE(completion);
    auto output = plan.acquire_output(*completion);
    REQUIRE(output);
    CHECK(reinterpret_cast<const float*>(output->bytes.data())[0] == 3.0f);
    REQUIRE(plan.release_output({output->token}));

    const auto receipts = plan.take_lifecycle_receipts();
    REQUIRE(receipts.size() == 1);
    CHECK(receipts[0].generation == input->token.preparation_epoch);
    CHECK(receipts[0].sequence == 3);
    CHECK(receipts[0].submit_ns >= 0);
    CHECK(receipts[0].terminal);
    CHECK(receipts[0].terminal_ns >= 0);
    CHECK(receipts[0].service_ns >= receipts[0].terminal_ns);
    CHECK(receipts[0].delivery_ns >= 0);
    CHECK(receipts[0].output_acquired);
    CHECK(receipts[0].output_released);
    CHECK_FALSE(receipts[0].gpu_timestamp_available);
    CHECK(receipts[0].gpu_elapsed_ns == -1);
    CHECK_FALSE(receipts[0].device_lost);
    CHECK(plan.lifecycle_receipt_valid());

    auto refused = plan.acquire_input(4, 0);
    REQUIRE(refused);
    REQUIRE(plan.cancel({refused->token, 0}));
    auto next = plan.acquire_input(5, 0);
    REQUIRE(next);
    REQUIRE(plan.cancel({next->token, 0}));
    REQUIRE(plan.release());
}

TEST_CASE("shared IO slot buffer capabilities expire at retirement", "[gpu_audio][shared_io][p2]") {
    FakeProvider provider;
    SharedIoArenaProvider::SlotResources resources;
    REQUIRE(provider.create_slot(0, 16, 16, resources));
    SharedIoArenaProvider::SlotBufferHandle handle;
    REQUIRE(provider.acquire_slot_buffers(resources, handle));
    REQUIRE(provider.validate_slot_buffers(handle));
    provider.retire_slot(resources);
    CHECK_FALSE(provider.validate_slot_buffers(handle));
    provider.destroy_slot(resources);
    CHECK_FALSE(provider.validate_slot_buffers(handle));
}

TEST_CASE("shared IO slot buffer capability lifetime expires with provider",
          "[gpu_audio][shared_io][p2]") {
    SharedIoArenaProvider::SlotBufferHandle handle;
    {
        FakeProvider provider;
        SharedIoArenaProvider::SlotResources resources;
        REQUIRE(provider.create_slot(0, 16, 16, resources));
        REQUIRE(provider.acquire_slot_buffers(resources, handle));
        CHECK(handle.has_lifetime());
    }
    CHECK_FALSE(handle.has_lifetime());
}

TEST_CASE("shared IO compute plan reprimes persistent slots only after quiescence",
          "[gpu_audio][shared_io][p2]") {
    FakeProvider provider;
    SharedIoComputePlan plan;
    REQUIRE(plan.prepare(provider,
                         {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    const auto first_epoch = plan.preparation_epoch();
    auto first = plan.acquire_input(20, 0);
    REQUIRE(first);
    REQUIRE(plan.submit({first->token, 0}));
    REQUIRE(plan.drain(0) == 1);
    CHECK_FALSE(plan.reprime_when_quiescent());

    auto completion = plan.pop_completion();
    REQUIRE(completion);
    auto output = plan.acquire_output(*completion);
    REQUIRE(output);
    REQUIRE(plan.release_output({output->token}));
    output.reset();

    REQUIRE(plan.reprime_when_quiescent());
    CHECK(plan.preparation_epoch() == first_epoch + 1);
    CHECK_FALSE(plan.submit({first->token, 0}));

    auto reprime = plan.acquire_input(21, 0);
    REQUIRE(reprime);
    CHECK(reprime->token.preparation_epoch == first_epoch + 1);
    REQUIRE(plan.cancel({reprime->token, 0}));
    const auto receipts = plan.take_lifecycle_receipts();
    REQUIRE(receipts.size() == 2);
    CHECK(receipts[0].generation == first_epoch);
    CHECK(receipts[0].sequence == 20);
    CHECK(receipts[0].output_released);
    CHECK(receipts[0].gpu_elapsed_ns == -1);
    CHECK(receipts[1].generation == first_epoch + 1);
    CHECK(receipts[1].sequence == 21);
    CHECK(receipts[1].discarded);
    CHECK_FALSE(receipts[1].terminal);
    REQUIRE(plan.release());
}

TEST_CASE("shared IO compute plan bounds saturation and retires refused or failed input",
          "[gpu_audio][shared_io][p2]") {
    FakeProvider provider;
    SharedIoComputePlan plan;
    REQUIRE(plan.prepare(provider,
                         {.slots = 2, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));

    auto first = plan.acquire_input(30, 0);
    auto second = plan.acquire_input(31, 0);
    REQUIRE(first);
    REQUIRE(second);
    CHECK_FALSE(plan.acquire_input(32, 0));
    REQUIRE(plan.submit({first->token, 0}));
    REQUIRE(plan.submit({second->token, 0}));
    REQUIRE(plan.drain(0) == 2);
    CHECK_FALSE(plan.acquire_input(32, 0));

    for (std::uint64_t sequence : {30u, 31u}) {
        auto completion = plan.pop_completion();
        REQUIRE(completion);
        CHECK(completion->token.slot.stream_sequence == sequence);
        auto output = plan.acquire_output(*completion);
        REQUIRE(output);
        REQUIRE(plan.release_output({output->token}));
    }

    provider.accept_submissions = false;
    auto refused = plan.acquire_input(32, 0);
    REQUIRE(refused);
    CHECK_FALSE(plan.submit({refused->token, 0}));
    auto after_refusal = plan.acquire_input(33, 0);
    REQUIRE(after_refusal);
    REQUIRE(plan.cancel({after_refusal->token, 0}));

    provider.accept_submissions = true;
    provider.terminal_status = SharedIoArena::CompletionStatus::RetiredFailed;
    auto failed = plan.acquire_input(34, 0);
    REQUIRE(failed);
    REQUIRE(plan.submit({failed->token, 0}));
    REQUIRE(plan.drain(0) == 1);
    auto failure = plan.pop_completion();
    REQUIRE(failure);
    CHECK(failure->status == SharedIoArena::CompletionStatus::RetiredFailed);
    CHECK_FALSE(plan.acquire_output(*failure));
    REQUIRE(plan.discard_completion(*failure));
    auto after_failure = plan.acquire_input(35, 0);
    REQUIRE(after_failure);
    REQUIRE(plan.cancel({after_failure->token, 0}));
    const auto receipts = plan.take_lifecycle_receipts();
    REQUIRE(receipts.size() == 6);
    CHECK(receipts[0].sequence == 30);
    CHECK(receipts[1].sequence == 31);
    CHECK(receipts[2].sequence == 32);
    CHECK(receipts[2].discarded);
    CHECK(receipts[3].sequence == 33);
    CHECK(receipts[3].discarded);
    CHECK(receipts[4].sequence == 34);
    CHECK(receipts[4].terminal);
    CHECK(receipts[4].discarded);
    CHECK_FALSE(receipts[4].gpu_timestamp_available);
    CHECK(receipts[4].gpu_elapsed_ns == -1);
    CHECK(receipts[5].sequence == 35);
    CHECK(receipts[5].discarded);
    REQUIRE(plan.release());
}

TEST_CASE("shared IO lifecycle receipts fail closed on bounded retention overflow",
          "[gpu_audio][shared_io][p3]") {
    FakeProvider provider;
    SharedIoComputePlan plan;
    REQUIRE(plan.prepare(provider,
                         {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    for (std::uint64_t sequence = 0; sequence <= SharedIoComputePlan::kLifecycleReceiptCapacity;
         ++sequence) {
        auto input = plan.acquire_input(sequence, 0);
        REQUIRE(input);
        REQUIRE(plan.cancel({input->token, 0}));
    }
    CHECK(plan.lifecycle_receipt_overflow());
    CHECK_FALSE(plan.lifecycle_receipt_valid());
    CHECK(plan.take_lifecycle_receipts().size() == SharedIoComputePlan::kLifecycleReceiptCapacity);
    REQUIRE(plan.release());
}

TEST_CASE("shared IO lifecycle receipt separates observed service time from delivery",
          "[gpu_audio][shared_io][p3]") {
    FakeProvider provider;
    SharedIoComputePlan plan;
    REQUIRE(plan.prepare(provider,
                         {.slots = 1, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    auto input = plan.acquire_input(90, 0);
    REQUIRE(input);
    REQUIRE(plan.submit({input->token, 0}));
    CHECK(plan.take_lifecycle_receipts().empty());
    REQUIRE(plan.drain_until(0, 1) == 1);
    auto completion = plan.pop_completion();
    REQUIRE(completion);
    auto output = plan.acquire_output(*completion);
    REQUIRE(output);
    REQUIRE(plan.release_output({output->token}));
    const auto receipts = plan.take_lifecycle_receipts();
    REQUIRE(receipts.size() == 1);
    CHECK(receipts[0].submit_ns >= 0);
    CHECK(receipts[0].terminal_ns >= receipts[0].submit_ns);
    CHECK(receipts[0].service_ns >= receipts[0].terminal_ns);
    CHECK(receipts[0].delivery_ns >= receipts[0].service_ns);
    CHECK(receipts[0].gpu_elapsed_ns == -1);
    REQUIRE(plan.release());
}

TEST_CASE("program session returns the same owner only after physical release",
          "[gpu_audio][shared_io][same_device]") {
    auto provider = std::make_unique<FakeProvider>();
    auto* identity = provider.get();
    auto state = provider->lifecycle_state;
    auto program = std::make_unique<FakePreparedProgram>(*provider);
    program->allow_release = true;
    SharedIoProgramSession session;
    REQUIRE(session.prepare({std::move(provider), std::move(program)},
                            {.slots = 2, .input_bytes_per_slot = 16, .output_bytes_per_slot = 16}));
    identity->allow_drain = false;
    CHECK_FALSE(session.release_to_owner());
    CHECK(session.owned_provider() == identity);
    CHECK_FALSE(state->provider_destroyed);
    identity->allow_drain = true;
    auto returned = session.release_to_owner();
    REQUIRE(returned.get() == identity);
    CHECK_FALSE(session.prepared());
    CHECK(session.owned_provider() == nullptr);
    CHECK_FALSE(state->provider_destroyed);
    CHECK_FALSE(session.release_to_owner());
    returned.reset();
    CHECK(state->provider_destroyed);
}
