#include "detail/shared_io_compute_plan.hpp"
#include "detail/shared_io_transport_bridge.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <memory>

using namespace pulp::gpu_audio::detail;

namespace {
class FakeProvider final : public SharedIoArenaProvider {
    struct Slot {
        std::byte* input;
        std::byte* output;
        std::uint64_t generation = 1;
        bool retired = false;
    };

  public:
    bool create_slot(std::uint32_t, std::size_t in, std::size_t out,
                     SlotResources& resources) noexcept override {
        auto* slot = new (std::nothrow)
            Slot{new (std::nothrow) std::byte[in], new (std::nothrow) std::byte[out]};
        if (!slot || !slot->input || !slot->output) {
            if (slot) {
                delete[] slot->input;
                delete[] slot->output;
                delete slot;
            }
            return false;
        }
        resources.input = slot->input;
        resources.input_size = in;
        resources.output = slot->output;
        resources.output_size = out;
        resources.opaque = slot;
        slots_.push_back(slot);
        resources.input_lifecycle = {
            .allocated = true, .import_attempted = true, .import_succeeded = true};
        resources.output_lifecycle = resources.input_lifecycle;
        return true;
    }
    void retire_slot(SlotResources& resources) noexcept override {
        if (require_program_release && !program_released)
            retired_before_program_release = true;
        auto* slot = static_cast<Slot*>(resources.opaque);
        if (slot) {
            slot->retired = true;
            ++slot->generation;
        }
        resources.input_lifecycle.dispose_observed = true;
        resources.output_lifecycle.dispose_observed = true;
    }
    void destroy_slot(SlotResources& resources) noexcept override {
        auto* slot = static_cast<Slot*>(resources.opaque);
        if (!slot)
            return;
        delete[] slot->input;
        delete[] slot->output;
        std::erase(slots_, slot);
        delete slot;
        resources.input_lifecycle.host_freed = true;
        resources.output_lifecycle.host_freed = true;
        resources.opaque = nullptr;
    }
    bool acquire_slot_buffers(const SlotResources& resources,
                              SlotBufferHandle& handle) const noexcept override {
        auto* slot = static_cast<Slot*>(resources.opaque);
        if (!slot || slot->retired)
            return false;
        handle = {};
        handle.provider = this;
        handle.device = this;
        handle.input_buffer = slot->input;
        handle.output_buffer = slot->output;
        handle.slot = 0;
        handle.generation = slot->generation;
        handle.lifetime = lifetime_;
        return true;
    }
    bool validate_slot_buffers(const SlotBufferHandle& handle) const noexcept override {
        if (handle.provider != this || handle.device != this || handle.lifetime.expired())
            return false;
        for (const auto* slot : slots_) {
            if (!slot->retired && handle.input_buffer == slot->input &&
                handle.output_buffer == slot->output && handle.generation == slot->generation)
                return true;
        }
        return false;
    }

    bool submit(const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> inbox) noexcept override {
        if (!accept_submissions)
            return false;
        auto* slot = static_cast<Slot*>(resources.opaque);
        const auto count = resources.input_size / sizeof(float);
        auto* in = reinterpret_cast<const float*>(slot->input);
        auto* out = reinterpret_cast<float*>(slot->output);
        for (std::size_t i = 0; i < count && i < resources.output_size / sizeof(float); ++i)
            out[i] = in[i] * 2.0f;
        return inbox->push(token, terminal_status) == SharedIoTerminalInbox::PushResult::Accepted;
    }
    void poll() noexcept override {}
    bool drain() noexcept override {
        return true;
    }
    void cleanup_abandoned_for_test() noexcept {
        while (!slots_.empty()) {
            SlotResources resources;
            resources.opaque = slots_.back();
            retire_slot(resources);
            destroy_slot(resources);
        }
    }
    bool accept_submissions = true;
    bool require_program_release = false;
    bool program_released = false;
    bool program_destroyed = false;
    bool retired_before_program_release = false;
    CompletionStatus terminal_status = CompletionStatus::RetiredSuccess;
    std::shared_ptr<const void> lifetime_ = std::make_shared<int>(0);
    std::vector<Slot*> slots_;
};

class FakePreparedProgram final : public SharedIoPreparedProgram {
  public:
    explicit FakePreparedProgram(FakeProvider& expected) : expected_(expected) {}
    ~FakePreparedProgram() override {
        expected_.program_destroyed = true;
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
        return inbox->push(token, SharedIoTerminalStatus::RetiredSuccess) ==
               SharedIoTerminalInbox::PushResult::Accepted;
    }

    bool release() noexcept override {
        if (!allow_release)
            return false;
        expected_.program_released = true;
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
    CHECK_FALSE(provider.program_released);
    CHECK_FALSE(provider.retired_before_program_release);
    control->allow_release = true;
    REQUIRE(plan.release());
    CHECK(provider.program_released);
    CHECK_FALSE(provider.retired_before_program_release);
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
    CHECK_FALSE(provider.program_released);
    CHECK_FALSE(provider.retired_before_program_release);

    control->allow_release = true;
    REQUIRE(plan.release());
    CHECK(provider.program_released);
    CHECK_FALSE(provider.retired_before_program_release);
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
    CHECK_FALSE(provider.program_released);
    CHECK_FALSE(provider.program_destroyed);
    CHECK_FALSE(provider.retired_before_program_release);
    REQUIRE(provider.slots_.size() == 1);

    // Test-only reclamation after observing the quarantine. Production has no
    // recovery claim once an owner is destructed with an unproven barrier.
    control->allow_release = true;
    REQUIRE(control->release());
    delete control;
    provider.cleanup_abandoned_for_test();
    CHECK(provider.program_destroyed);
    CHECK(provider.slots_.empty());
    CHECK_FALSE(provider.retired_before_program_release);
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
    REQUIRE(plan.release());
}
