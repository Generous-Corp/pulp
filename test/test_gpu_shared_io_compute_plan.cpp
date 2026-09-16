#include "detail/shared_io_compute_plan.hpp"
#include "detail/shared_io_transport_bridge.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>

using namespace pulp::gpu_audio::detail;

namespace {
class FakeProvider final : public SharedIoArenaProvider {
    struct Slot { std::byte* input; std::byte* output; };
  public:
    bool create_slot(std::uint32_t, std::size_t in, std::size_t out,
                     SlotResources& resources) noexcept override {
        auto* slot = new (std::nothrow) Slot{new (std::nothrow) std::byte[in],
                                             new (std::nothrow) std::byte[out]};
        if (!slot || !slot->input || !slot->output) {
            if (slot) { delete[] slot->input; delete[] slot->output; delete slot; }
            return false;
        }
        resources.input = slot->input; resources.input_size = in;
        resources.output = slot->output; resources.output_size = out;
        resources.opaque = slot;
        resources.input_lifecycle = {.allocated=true, .import_attempted=true,
                                     .import_succeeded=true};
        resources.output_lifecycle = resources.input_lifecycle;
        return true;
    }
    void retire_slot(SlotResources& resources) noexcept override {
        resources.input_lifecycle.dispose_observed = true;
        resources.output_lifecycle.dispose_observed = true;
    }
    void destroy_slot(SlotResources& resources) noexcept override {
        auto* slot = static_cast<Slot*>(resources.opaque);
        if (!slot) return;
        delete[] slot->input; delete[] slot->output; delete slot;
        resources.input_lifecycle.host_freed = true;
        resources.output_lifecycle.host_freed = true;
        resources.opaque = nullptr;
    }
    bool submit(const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> inbox) noexcept override {
        auto* slot = static_cast<Slot*>(resources.opaque);
        const auto count = resources.input_size / sizeof(float);
        auto* in = reinterpret_cast<const float*>(slot->input);
        auto* out = reinterpret_cast<float*>(slot->output);
        for (std::size_t i = 0; i < count && i < resources.output_size / sizeof(float); ++i)
            out[i] = in[i] * 2.0f;
        return inbox->push(token, CompletionStatus::RetiredSuccess) ==
               SharedIoTerminalInbox::PushResult::Accepted;
    }
    void poll() noexcept override {}
    bool drain() noexcept override { return true; }
};
} // namespace

TEST_CASE("shared IO compute plan keeps deadline outside slot token",
          "[gpu_audio][shared_io][p2]") {
    FakeProvider provider;
    SharedIoComputePlan plan;
    REQUIRE(plan.prepare(provider, {.slots=2, .input_bytes_per_slot=16,
                                    .output_bytes_per_slot=16}));
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

TEST_CASE("shared IO bridge never leapfrogs a delayed head",
          "[gpu_audio][shared_io][p2]") {
    SharedIoTransportBridge bridge;
    REQUIRE(bridge.prepare(3, 10));
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

TEST_CASE("shared IO compute plan exposes retired output and cancels refused leases",
          "[gpu_audio][shared_io][p2]") {
    FakeProvider provider;
    SharedIoComputePlan plan;
    REQUIRE(plan.prepare(provider, {.slots=1, .input_bytes_per_slot=16,
                                    .output_bytes_per_slot=16}));
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
