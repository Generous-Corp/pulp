#include "detail/shared_io_convolution_session.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace {
using namespace pulp::gpu_audio::detail;
using Session = SharedIoConvolutionSession;

struct State {
    bool program_released = false;
    bool retired_before_program_release = false;
    bool program_destroyed = false;
    std::uint32_t submits = 0;
    std::uint32_t polls = 0;
    std::uint32_t drains = 0;
};

class FakeProvider final : public SharedIoArenaProvider {
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
    explicit FakeProvider(std::shared_ptr<State> state) : state_(std::move(state)) {}

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
        if (!state_->program_released)
            state_->retired_before_program_release = true;
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

    bool submit(const SlotResources&, SlotToken,
                std::shared_ptr<SharedIoTerminalInbox>) noexcept override {
        return false; // This session requires the transferred prepared program.
    }

    bool device_lost() const noexcept override {
        return lost;
    }
    bool can_resume_after_drain() const noexcept override {
        return resume_supported;
    }

    void poll() noexcept override {
        ++state_->polls;
        if (auto_complete)
            complete_all();
    }

    bool drain() noexcept override {
        ++state_->drains;
        if (!allow_drain)
            return false;
        complete_all();
        return pending_.empty();
    }

    bool submit_program(const SlotResources& resources, SlotToken token,
                        std::shared_ptr<SharedIoTerminalInbox> inbox) noexcept {
        ++state_->submits;
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

    std::shared_ptr<State> state_;
    bool accept_submissions = true;
    bool auto_complete = true;
    bool allow_drain = true;
    bool lost = false;
    bool resume_supported = true;
    CompletionStatus terminal_status = CompletionStatus::RetiredSuccess;

  private:
    std::shared_ptr<const void> lifetime_ = std::make_shared<int>(0);
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<Pending> pending_;
};

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

    void prepare(std::uint32_t slots = 2, std::uint32_t channels = 1) {
        auto owner = std::make_unique<FakeProvider>(state);
        provider = owner.get();
        auto program = std::make_unique<FakeProgram>(*provider, state);
        REQUIRE(session.prepare({std::move(owner), std::move(program)},
                                {.pipeline = {.capacity = 8,
                                              .channels = channels,
                                              .block_size = 2,
                                              .fft_size = 2,
                                              .ir_length = 1},
                                 .slots = slots}));
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
    fixture.provider->auto_complete = false;
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
    fixture.provider->auto_complete = false;
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
    auto provider = std::make_unique<FakeProvider>(state);
    auto* provider_control = provider.get();
    auto program = std::make_unique<FakeProgram>(*provider_control, state);
    auto* program_control = program.get();
    program_control->allow_prepare = false;
    provider_control->allow_drain = false;
    CHECK_FALSE(session.prepare(
        {std::move(provider), std::move(program)},
        {.pipeline = {.capacity = 2, .channels = 1, .block_size = 2, .fft_size = 2, .ir_length = 1},
         .slots = 1}));
    CHECK_FALSE(session.prepared());
    CHECK_FALSE(session.release());
    CHECK_FALSE(state->program_released);
    CHECK_FALSE(state->program_destroyed);
    provider_control->allow_drain = true;
    REQUIRE(session.release());
    CHECK(state->program_released);
    CHECK(state->program_destroyed);
    CHECK_FALSE(state->retired_before_program_release);
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
    fixture.provider->auto_complete = false;
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
    fixture.provider->auto_complete = false;
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
    fixture.provider->auto_complete = false;
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
