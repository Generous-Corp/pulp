#include "detail/shared_io_arena.hpp"
#include "detail/shared_io_slot_ledger.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <barrier>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

using pulp::gpu_audio::detail::SharedIoArena;
using pulp::gpu_audio::detail::SharedIoArenaProvider;
using pulp::gpu_audio::detail::SharedIoSlotLedger;
using pulp::gpu_audio::detail::SharedIoTerminalInbox;
using pulp::gpu_audio::detail::SharedIoTerminalStatus;

static_assert(!std::is_copy_constructible_v<SharedIoSlotLedger>);
static_assert(!std::is_copy_assignable_v<SharedIoSlotLedger>);
static_assert(!std::is_move_constructible_v<SharedIoSlotLedger>);
static_assert(!std::is_move_assignable_v<SharedIoSlotLedger>);
static_assert(!std::is_copy_constructible_v<SharedIoArena>);
static_assert(!std::is_move_constructible_v<SharedIoArena>);

TEST_CASE("shared IO slots preserve identity across multiple slots and reuse",
          "[gpu_audio][shared_io][lifecycle]") {
    SharedIoSlotLedger ledger;
    REQUIRE(ledger.prepare(3));
    REQUIRE(ledger.capacity() == 3);
    REQUIRE(ledger.available_slots() == 3);

    const auto a = ledger.acquire(100);
    const auto b = ledger.acquire(101);
    const auto c = ledger.acquire(102);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(c);
    CHECK(a->slot != b->slot);
    CHECK(a->slot != c->slot);
    CHECK(b->slot != c->slot);
    CHECK_FALSE(ledger.acquire(103));
    CHECK_FALSE(ledger.acquire(101));

    REQUIRE(ledger.publish(*a));
    REQUIRE(ledger.publish(*b));
    REQUIRE(ledger.publish(*c));
    REQUIRE(ledger.claim_submission(*b) == b);
    REQUIRE(ledger.claim_submission(*a) == a);
    REQUIRE(ledger.claim_submission(*c) == c);
    REQUIRE(ledger.complete_gpu(*a, SharedIoSlotLedger::GpuCompletion::Success));
    REQUIRE(ledger.complete_gpu(*b, SharedIoSlotLedger::GpuCompletion::Success));
    REQUIRE(ledger.complete_gpu(*c, SharedIoSlotLedger::GpuCompletion::Success));

    const auto out_a = ledger.acquire_output(a->preparation_epoch, 100);
    REQUIRE(out_a == a);
    REQUIRE(ledger.release(*out_a));

    const auto reused = ledger.acquire(103);
    REQUIRE(reused);
    CHECK(reused->slot == a->slot);
    CHECK(reused->preparation_epoch == a->preparation_epoch);
    CHECK(reused->slot_generation == a->slot_generation + 1);

    // The old token cannot mutate or free its reused physical slot.
    CHECK_FALSE(ledger.publish(*a));
    CHECK_FALSE(ledger.complete_gpu(*a, SharedIoSlotLedger::GpuCompletion::Success));
    CHECK_FALSE(ledger.expire_delivery(*a));
    CHECK_FALSE(ledger.discard(*a));
    CHECK(ledger.available_slots() == 0);

    REQUIRE(ledger.discard(*reused));
    REQUIRE(ledger.discard(*b));
    REQUIRE(ledger.discard(*c));
    CHECK(ledger.quiescent());
}

TEST_CASE("shared IO output acquisition requires the exact expected sequence",
          "[gpu_audio][shared_io][sequence]") {
    SharedIoSlotLedger ledger;
    REQUIRE(ledger.prepare(2));

    const auto ten = ledger.acquire(10);
    const auto twelve = ledger.acquire(12);
    REQUIRE(ten);
    REQUIRE(twelve);
    REQUIRE(ledger.publish(*ten));
    REQUIRE(ledger.publish(*twelve));
    REQUIRE(ledger.claim_submission(*ten) == ten);
    REQUIRE(ledger.claim_submission(*twelve) == twelve);
    REQUIRE(ledger.complete_gpu(*ten, SharedIoSlotLedger::GpuCompletion::Success));
    REQUIRE(ledger.complete_gpu(*twelve, SharedIoSlotLedger::GpuCompletion::Success));

    CHECK_FALSE(ledger.acquire_output(ledger.preparation_epoch(), 11));
    const auto out_twelve = ledger.acquire_output(twelve->preparation_epoch, 12);
    REQUIRE(out_twelve == twelve);
    REQUIRE(ledger.release(*out_twelve));
    const auto out_ten = ledger.acquire_output(ten->preparation_epoch, 10);
    REQUIRE(out_ten == ten);
    REQUIRE(ledger.release(*out_ten));
}

TEST_CASE("shared IO expiry cannot reuse storage before actual GPU completion",
          "[gpu_audio][shared_io][deadline]") {
    SharedIoSlotLedger ledger;
    REQUIRE(ledger.prepare(1));

    const auto token = ledger.acquire(20);
    REQUIRE(token);
    REQUIRE(ledger.publish(*token));
    REQUIRE(ledger.claim_submission(*token) == token);
    REQUIRE(ledger.expire_delivery(*token));

    CHECK_FALSE(ledger.acquire(21));
    CHECK_FALSE(ledger.acquire_output(token->preparation_epoch, 20));
    CHECK_FALSE(ledger.discard(*token));
    CHECK_FALSE(ledger.expire_delivery(*token));

    REQUIRE(ledger.complete_gpu(*token, SharedIoSlotLedger::GpuCompletion::Success));
    CHECK_FALSE(ledger.acquire(21));
    CHECK_FALSE(ledger.complete_gpu(*token, SharedIoSlotLedger::GpuCompletion::Success));
    REQUIRE(ledger.discard(*token));

    const auto reused = ledger.acquire(21);
    REQUIRE(reused);
    CHECK(reused->slot == token->slot);
    CHECK(reused->slot_generation == token->slot_generation + 1);
    CHECK_FALSE(ledger.complete_gpu(*token, SharedIoSlotLedger::GpuCompletion::Failed));
    CHECK_FALSE(ledger.discard(*token));
    REQUIRE(ledger.discard(*reused));
}

TEST_CASE("shared IO completion separates consumption release from failure discard",
          "[gpu_audio][shared_io][completion]") {
    SharedIoSlotLedger ledger;
    REQUIRE(ledger.prepare(2));

    const auto good = ledger.acquire(30);
    const auto failed = ledger.acquire(31);
    REQUIRE(good);
    REQUIRE(failed);
    REQUIRE(ledger.publish(*good));
    REQUIRE(ledger.publish(*failed));
    REQUIRE(ledger.claim_submission(*good) == good);
    REQUIRE(ledger.claim_submission(*failed) == failed);
    REQUIRE(ledger.complete_gpu(*good, SharedIoSlotLedger::GpuCompletion::Success));
    REQUIRE(ledger.complete_gpu(*failed, SharedIoSlotLedger::GpuCompletion::Failed));

    CHECK_FALSE(ledger.acquire_output(failed->preparation_epoch, 31));
    CHECK_FALSE(ledger.release(*failed));
    REQUIRE(ledger.discard(*failed));

    const auto output = ledger.acquire_output(good->preparation_epoch, 30);
    REQUIRE(output == good);
    CHECK_FALSE(ledger.discard(*output));
    REQUIRE(ledger.release(*output));
    CHECK(ledger.quiescent());
}

TEST_CASE("shared IO retirement drains before reset and rejects stale epochs",
          "[gpu_audio][shared_io][retirement]") {
    SharedIoSlotLedger ledger;
    REQUIRE(ledger.prepare(3));
    const auto old_epoch = ledger.preparation_epoch();

    const auto local = ledger.acquire(40);
    const auto submitted = ledger.acquire(41);
    REQUIRE(local);
    REQUIRE(submitted);
    REQUIRE(ledger.publish(*submitted));
    REQUIRE(ledger.claim_submission(*submitted) == submitted);
    REQUIRE(ledger.available_slots() == 1);

    ledger.begin_retirement();
    CHECK(ledger.retiring());
    CHECK(ledger.available_slots() == 0);
    CHECK_FALSE(ledger.acquire(42));
    CHECK_FALSE(ledger.publish(*local));
    CHECK_FALSE(ledger.discard(*submitted));
    CHECK_FALSE(ledger.quiescent());
    CHECK_FALSE(ledger.reset_when_quiescent());

    REQUIRE(ledger.discard(*local));
    REQUIRE(ledger.complete_gpu(*submitted, SharedIoSlotLedger::GpuCompletion::Failed));
    REQUIRE(ledger.discard(*submitted));
    REQUIRE(ledger.quiescent());
    CHECK(ledger.available_slots() == 0);
    REQUIRE(ledger.reset_when_quiescent());
    CHECK_FALSE(ledger.retiring());
    CHECK(ledger.available_slots() == 3);
    CHECK(ledger.preparation_epoch() == old_epoch + 1);

    const auto current = ledger.acquire(42);
    REQUIRE(current);
    CHECK(current->preparation_epoch != local->preparation_epoch);
    CHECK_FALSE(ledger.publish(*local));
    CHECK_FALSE(ledger.complete_gpu(*submitted, SharedIoSlotLedger::GpuCompletion::Success));
    CHECK_FALSE(ledger.discard(*local));
    REQUIRE(ledger.discard(*current));
}

TEST_CASE("shared IO lookups reject a retired epoch without claiming current work",
          "[gpu_audio][shared_io][retirement]") {
    SharedIoSlotLedger ledger;
    REQUIRE(ledger.prepare(1));
    const auto old = ledger.acquire(50);
    REQUIRE(old);
    REQUIRE(ledger.publish(*old));
    REQUIRE(ledger.claim_submission(*old) == old);
    REQUIRE(ledger.complete_gpu(*old, SharedIoSlotLedger::GpuCompletion::Success));
    REQUIRE(ledger.discard(*old));

    ledger.begin_retirement();
    REQUIRE(ledger.reset_when_quiescent());
    const auto current = ledger.acquire(50);
    REQUIRE(current);
    REQUIRE(current->slot == old->slot);
    REQUIRE(current->stream_sequence == old->stream_sequence);
    REQUIRE(current->slot_generation == old->slot_generation);
    REQUIRE(current->preparation_epoch != old->preparation_epoch);
    REQUIRE(ledger.publish(*current));

    CHECK_FALSE(ledger.claim_submission(*old));
    REQUIRE(ledger.claim_submission(*current) == current);
    CHECK_FALSE(ledger.complete_gpu(*old, SharedIoSlotLedger::GpuCompletion::Success));
    REQUIRE(ledger.complete_gpu(*current, SharedIoSlotLedger::GpuCompletion::Success));

    CHECK_FALSE(ledger.acquire_output(old->preparation_epoch, old->stream_sequence));
    const auto output = ledger.acquire_output(current->preparation_epoch, current->stream_sequence);
    REQUIRE(output == current);
    REQUIRE(ledger.release(*output));
    CHECK(ledger.quiescent());
}

TEST_CASE("prepared shared IO slot lifecycle operations allocate nothing",
          "[gpu_audio][shared_io][allocation]") {
    SharedIoSlotLedger ledger;
    REQUIRE(ledger.prepare(2));

    bool lifecycle_ok = true;
    std::size_t allocations = 1;
    {
        pulp::test::RtAllocationProbe probe;

        lifecycle_ok = ledger.prepared() && !ledger.retiring() && ledger.capacity() == 2 &&
                       ledger.available_slots() == 2;

        const auto expired = ledger.acquire(70);
        lifecycle_ok = lifecycle_ok && expired.has_value();
        if (expired) {
            lifecycle_ok = ledger.publish(*expired) && lifecycle_ok;
            lifecycle_ok = ledger.claim_submission(*expired).has_value() && lifecycle_ok;
            lifecycle_ok = ledger.expire_delivery(*expired) && lifecycle_ok;
            lifecycle_ok =
                ledger.complete_gpu(*expired, SharedIoSlotLedger::GpuCompletion::Success) &&
                lifecycle_ok;
            lifecycle_ok = ledger.discard(*expired) && lifecycle_ok;
        }

        const auto delivered = ledger.acquire(71);
        lifecycle_ok = lifecycle_ok && delivered.has_value();
        if (delivered) {
            lifecycle_ok = ledger.publish(*delivered) && lifecycle_ok;
            lifecycle_ok = ledger.claim_submission(*delivered).has_value() && lifecycle_ok;
            lifecycle_ok =
                ledger.complete_gpu(*delivered, SharedIoSlotLedger::GpuCompletion::Success) &&
                lifecycle_ok;
            const auto output =
                ledger.acquire_output(delivered->preparation_epoch, delivered->stream_sequence);
            lifecycle_ok = lifecycle_ok && output == delivered;
            if (output)
                lifecycle_ok = ledger.release(*output) && lifecycle_ok;
        }

        ledger.begin_retirement();
        lifecycle_ok = ledger.retiring() && ledger.available_slots() == 0 && ledger.quiescent() &&
                       ledger.reset_when_quiescent() && lifecycle_ok;
        const auto after_reset = ledger.acquire(72);
        lifecycle_ok = lifecycle_ok && after_reset.has_value();
        if (after_reset)
            lifecycle_ok = ledger.discard(*after_reset) && lifecycle_ok;

        allocations = probe.allocation_count();
    }

    REQUIRE(lifecycle_ok);
    REQUIRE(allocations == 0);
}

TEST_CASE("shared IO submission rejects the wrong generation without claiming the slot",
          "[gpu_audio][shared_io][lifecycle]") {
    SharedIoSlotLedger ledger;
    REQUIRE(ledger.prepare(1));
    const auto old = ledger.acquire(60);
    REQUIRE(old);
    REQUIRE(ledger.discard(*old));
    const auto current = ledger.acquire(61);
    REQUIRE(current);
    REQUIRE(ledger.publish(*current));

    auto wrong_generation = *current;
    wrong_generation.slot_generation = old->slot_generation;
    REQUIRE(wrong_generation.slot_generation != current->slot_generation);
    CHECK_FALSE(ledger.claim_submission(wrong_generation));
    REQUIRE(ledger.claim_submission(*current) == current);
    REQUIRE(ledger.complete_gpu(*current, SharedIoSlotLedger::GpuCompletion::Failed));
    REQUIRE(ledger.discard(*current));
    CHECK(ledger.quiescent());
}

namespace {

class FakeSharedIoProvider final : public SharedIoArenaProvider {
  public:
    struct Allocation {
        std::uint32_t slot = 0;
        std::unique_ptr<std::byte[]> input;
        std::unique_ptr<std::byte[]> output;
        bool partial = false;
    };

    struct Pending {
        SlotToken token;
        std::shared_ptr<SharedIoTerminalInbox> inbox;
        bool accepted = false;
        bool terminal = false;
        bool completion_pending = false;
        SharedIoTerminalStatus completion_status = SharedIoTerminalStatus::RetiredFailed;
        std::byte* output = nullptr;
    };

    bool create_slot(std::uint32_t slot, std::size_t input_bytes, std::size_t output_bytes,
                     SlotResources& resources) noexcept override {
        ++create_calls;
        if (slot == refuse_creation_at)
            return false;
        try {
            pending.resize(static_cast<std::size_t>(slot) + 1);
            previous.resize(static_cast<std::size_t>(slot) + 1);
            retiring.resize(static_cast<std::size_t>(slot) + 1, nullptr);
        } catch (...) {
            return false;
        }
        auto allocation = std::unique_ptr<Allocation>(new (std::nothrow) Allocation);
        if (!allocation)
            return false;
        allocation->slot = slot;
        allocation->input.reset(new (std::nothrow) std::byte[input_bytes]);
        if (!allocation->input)
            return false;
        resources.input = allocation->input.get();
        resources.input_size = input_bytes;
        resources.input_lifecycle = {
            .allocated = true,
            .import_attempted = true,
            .import_succeeded = true,
        };
        if (slot == fail_after_partial_at) {
            allocation->partial = true;
            resources.opaque = allocation.release();
            ++partial_allocations_created;
            ++live_allocations;
            return false;
        }
        allocation->output.reset(new (std::nothrow) std::byte[output_bytes]);
        if (!allocation->output) {
            resources.opaque = allocation.release();
            ++live_allocations;
            return false;
        }
        if (slot == fail_output_import_at) {
            allocation->partial = true;
            resources.output = allocation->output.get();
            resources.output_size = output_bytes;
            resources.output_lifecycle = {
                .allocated = true,
                .import_attempted = true,
                .import_succeeded = false,
            };
            resources.opaque = allocation.release();
            ++partial_allocations_created;
            ++live_allocations;
            return false;
        }
        resources = {
            .input = allocation->input.get(),
            .input_size = input_bytes,
            .output = allocation->output.get(),
            .output_size = output_bytes,
            .opaque = allocation.release(),
            .input_lifecycle =
                {
                    .allocated = true,
                    .import_attempted = true,
                    .import_succeeded = true,
                },
            .output_lifecycle =
                {
                    .allocated = true,
                    .import_attempted = true,
                    .import_succeeded = true,
                },
        };
        ++live_allocations;
        if (slot == malformed_success_at)
            resources.output = nullptr;
        return true;
    }

    void retire_slot(SlotResources& resources) noexcept override {
        if (resources.opaque == nullptr)
            return;
        auto* allocation = static_cast<Allocation*>(resources.opaque);
        retiring[allocation->slot] = &resources;
        ++retire_calls;
    }

    void destroy_slot(SlotResources& resources) noexcept override {
        if (resources.opaque == nullptr)
            return;
        auto* allocation = static_cast<Allocation*>(resources.opaque);
        const auto disposal_ready = [](const AllocationLifecycle& lifecycle) {
            return !lifecycle.import_attempted || lifecycle.dispose_observed;
        };
        if (!disposal_ready(resources.input_lifecycle) ||
            !disposal_ready(resources.output_lifecycle)) {
            destroy_before_dispose = true;
        }
        resources.input_lifecycle.host_freed = resources.input_lifecycle.allocated;
        resources.output_lifecycle.host_freed = resources.output_lifecycle.allocated;
        host_frees += static_cast<std::uint32_t>(resources.input_lifecycle.allocated) +
                      static_cast<std::uint32_t>(resources.output_lifecycle.allocated);
        if (allocation->partial)
            ++partial_allocations_freed;
        delete allocation;
        resources.input = nullptr;
        resources.input_size = 0;
        resources.output = nullptr;
        resources.output_size = 0;
        resources.opaque = nullptr;
        ++destroy_calls;
        --live_allocations;
    }

    bool submit(const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> inbox) noexcept override {
        if (reject_submissions) {
            if (hold_stale_claim_on_reject) {
                auto attempt = inbox->try_claim(token.slot);
                if (attempt.claim) {
                    held_claim = *attempt.claim;
                    held_inbox = std::move(inbox);
                    held_stale_token = token;
                    if (held_stale_token.slot_generation > 0)
                        --held_stale_token.slot_generation;
                } else {
                    held_claim_failed = true;
                }
            }
            return false;
        }
        if (token.slot >= pending.size())
            return false;
        Pending& request = pending[token.slot];
        if (request.accepted && !request.terminal)
            return false;
        if (request.accepted)
            previous[token.slot] = request;
        request = Pending{
            .token = token,
            .inbox = std::move(inbox),
            .accepted = true,
            .output = resources.output,
        };
        return true;
    }

    void poll() noexcept override {
        ++poll_calls;
        if (!complete_on_poll)
            return;
        for (std::size_t index = 0; index < pending.size(); ++index) {
            auto& request = pending[index];
            if (!request.accepted || (request.terminal && !request.completion_pending))
                continue;
            const auto status =
                request.completion_pending ? request.completion_status : completion_on_poll;
            if (complete(index, status) == SharedIoTerminalInbox::PushResult::Busy)
                ++poll_busy_publications;
        }
    }

    bool drain() noexcept override {
        ++drain_calls;
        if (force_drain_failure)
            return false;
        for (auto& request : pending) {
            if (!request.accepted || request.terminal)
                continue;
            const auto result =
                request.inbox->push(request.token, SharedIoTerminalStatus::RetiredFailed);
            if (result != SharedIoTerminalInbox::PushResult::Accepted) {
                drain_push_failed = true;
                continue;
            }
            request.terminal = true;
        }
        for (auto*& resource : retiring) {
            if (resource == nullptr)
                continue;
            if (resource->input_lifecycle.import_attempted &&
                !resource->input_lifecycle.dispose_observed) {
                resource->input_lifecycle.dispose_observed = true;
                ++disposals_observed;
            }
            if (resource->output_lifecycle.import_attempted &&
                !resource->output_lifecycle.dispose_observed) {
                resource->output_lifecycle.dispose_observed = true;
                ++disposals_observed;
            }
            resource = nullptr;
        }
        return !drain_push_failed;
    }

    SharedIoTerminalInbox::PushResult complete(std::size_t index, SharedIoTerminalStatus status) {
        Pending& request = pending.at(index);
        if (!request.accepted)
            return SharedIoTerminalInbox::PushResult::Rejected;
        // The payload must be fully visible before Ready is release-published.
        if (status == SharedIoTerminalStatus::RetiredSuccess && request.output)
            request.output[0] = std::byte{0x6b};
        // Finish the success path's access to Pending before publishing Ready.
        // Immediate dispatcher reuse after the release store may overwrite this
        // record. Busy publishes nothing, so that path may safely restore the
        // retained retry state afterward.
        request.completion_status = status;
        request.completion_pending = false;
        request.terminal = true;
        const auto inbox = request.inbox;
        const auto token = request.token;
        const auto result = inbox->push(token, status);
        if (result == SharedIoTerminalInbox::PushResult::Busy) {
            request.terminal = false;
            request.completion_pending = true;
        }
        return result;
    }

    SharedIoTerminalInbox::PushResult replay(std::size_t index, SharedIoTerminalStatus status) {
        Pending& old = previous.at(index);
        Pending& request = old.accepted ? old : pending.at(index);
        if (!request.accepted)
            return SharedIoTerminalInbox::PushResult::Rejected;
        return request.inbox->push(request.token, status);
    }

    SharedIoTerminalInbox::PushResult finish_held_stale_claim() {
        if (!held_claim || !held_inbox)
            return SharedIoTerminalInbox::PushResult::Rejected;
        const auto result = held_inbox->finish_claim(*held_claim, held_stale_token,
                                                     SharedIoTerminalStatus::RetiredFailed);
        held_claim.reset();
        held_inbox.reset();
        return result;
    }

    bool hold_stale_completion_claim(std::size_t index) {
        Pending& request = pending.at(index);
        if (!request.accepted || held_claim || held_inbox)
            return false;
        auto attempt = request.inbox->try_claim(request.token.slot);
        if (!attempt.claim)
            return false;
        held_claim = *attempt.claim;
        held_inbox = request.inbox;
        held_stale_token = request.token;
        if (held_stale_token.slot_generation > 0)
            --held_stale_token.slot_generation;
        return true;
    }

    std::uint32_t refuse_creation_at = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t fail_after_partial_at = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t fail_output_import_at = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t malformed_success_at = std::numeric_limits<std::uint32_t>::max();
    bool reject_submissions = false;
    bool hold_stale_claim_on_reject = false;
    bool held_claim_failed = false;
    bool complete_on_poll = false;
    SharedIoTerminalStatus completion_on_poll = SharedIoTerminalStatus::RetiredSuccess;
    std::uint32_t create_calls = 0;
    std::uint32_t destroy_calls = 0;
    std::uint32_t retire_calls = 0;
    std::uint32_t live_allocations = 0;
    std::uint32_t poll_calls = 0;
    std::uint32_t poll_busy_publications = 0;
    std::uint32_t drain_calls = 0;
    std::uint32_t partial_allocations_created = 0;
    std::uint32_t partial_allocations_freed = 0;
    std::uint32_t disposals_observed = 0;
    std::uint32_t host_frees = 0;
    bool drain_push_failed = false;
    bool force_drain_failure = false;
    bool destroy_before_dispose = false;
    std::vector<Pending> pending;
    std::vector<Pending> previous;
    std::vector<SlotResources*> retiring;
    std::optional<SharedIoTerminalInbox::CompletionClaim> held_claim;
    std::shared_ptr<SharedIoTerminalInbox> held_inbox;
    SlotToken held_stale_token;
};

constexpr SharedIoArena::Config arena_config(std::uint32_t slots = 2) {
    return {.slots = slots, .input_bytes_per_slot = 64, .output_bytes_per_slot = 64};
}

SharedIoArena::SlotToken publish_and_submit(SharedIoArena& arena, std::uint64_t sequence) {
    const auto lease = arena.grant_write(sequence);
    REQUIRE(lease);
    REQUIRE(lease->bytes.size() == 64);
    lease->bytes.front() = std::byte{0x2a};
    REQUIRE(arena.publish_written({lease->token}));
    REQUIRE(arena.submit(lease->token));
    return lease->token;
}

} // namespace

TEST_CASE("shared IO arena creation refusal rolls back exact resource ownership",
          "[gpu_audio][shared_io][arena][creation]") {
    FakeSharedIoProvider provider;
    provider.refuse_creation_at = 1;
    SharedIoArena arena;

    CHECK_FALSE(arena.prepare(provider, arena_config(3)));
    CHECK_FALSE(arena.prepared());
    CHECK(provider.create_calls == 2);
    CHECK(provider.destroy_calls == 1);
    CHECK(provider.retire_calls == 1);
    CHECK(provider.disposals_observed == 2);
    CHECK(provider.host_frees == 2);
    CHECK_FALSE(provider.destroy_before_dispose);
    CHECK(provider.live_allocations == 0);
}

TEST_CASE("shared IO arena pre-import refusal needs no disposal callback",
          "[gpu_audio][shared_io][arena][creation]") {
    FakeSharedIoProvider provider;
    provider.refuse_creation_at = 0;
    SharedIoArena arena;

    CHECK_FALSE(arena.prepare(provider, arena_config(1)));
    CHECK(provider.create_calls == 1);
    CHECK(provider.retire_calls == 0);
    CHECK(provider.disposals_observed == 0);
    CHECK(provider.host_frees == 0);
    CHECK(provider.destroy_calls == 0);
    CHECK_FALSE(provider.destroy_before_dispose);
}

TEST_CASE("shared IO arena rolls back partial failure and malformed success",
          "[gpu_audio][shared_io][arena][creation]") {
    SECTION("provider retains and frees a partial failed transaction") {
        FakeSharedIoProvider provider;
        provider.fail_after_partial_at = 1;
        SharedIoArena arena;
        CHECK_FALSE(arena.prepare(provider, arena_config(3)));
        CHECK(provider.partial_allocations_created == 1);
        CHECK(provider.partial_allocations_freed == 1);
        CHECK(provider.destroy_calls == 2);
        CHECK(provider.retire_calls == 2);
        CHECK(provider.disposals_observed == 3);
        CHECK(provider.host_frees == 3);
        CHECK_FALSE(provider.destroy_before_dispose);
        CHECK(provider.live_allocations == 0);
    }

    SECTION("failed native-like output import drains deferred disposal") {
        FakeSharedIoProvider provider;
        provider.fail_output_import_at = 1;
        SharedIoArena arena;
        CHECK_FALSE(arena.prepare(provider, arena_config(3)));
        CHECK(provider.partial_allocations_created == 1);
        CHECK(provider.partial_allocations_freed == 1);
        CHECK(provider.destroy_calls == 2);
        CHECK(provider.retire_calls == 2);
        CHECK(provider.disposals_observed == 4);
        CHECK(provider.host_frees == 4);
        CHECK_FALSE(provider.destroy_before_dispose);
        CHECK(provider.live_allocations == 0);
    }

    SECTION("arena returns a malformed successful transaction") {
        FakeSharedIoProvider provider;
        provider.malformed_success_at = 1;
        SharedIoArena arena;
        CHECK_FALSE(arena.prepare(provider, arena_config(3)));
        CHECK(provider.destroy_calls == 2);
        CHECK(provider.retire_calls == 2);
        CHECK(provider.disposals_observed == 4);
        CHECK(provider.host_frees == 4);
        CHECK_FALSE(provider.destroy_before_dispose);
        CHECK(provider.live_allocations == 0);
    }
}

TEST_CASE("shared IO arena prepare allocation failures are fail-closed and retryable",
          "[gpu_audio][shared_io][arena][creation]") {
    constexpr SharedIoArena::Config::PrepareFault faults[] = {
        SharedIoArena::Config::PrepareFault::AfterLedger,
        SharedIoArena::Config::PrepareFault::AfterResourceTables,
        SharedIoArena::Config::PrepareFault::AfterInbox,
    };
    for (const auto fault : faults) {
        FakeSharedIoProvider provider;
        SharedIoArena arena;
        auto faulted = arena_config(2);
        faulted.prepare_fault = fault;
        CHECK_FALSE(arena.prepare(provider, faulted));
        CHECK_FALSE(arena.prepared());
        CHECK(provider.live_allocations == 0);
        REQUIRE(arena.prepare(provider, arena_config(2)));
        REQUIRE(arena.release());
        CHECK(provider.live_allocations == 0);
        CHECK(provider.create_calls == 2);
        CHECK(provider.destroy_calls == 2);
        CHECK(provider.retire_calls == 2);
        CHECK(provider.disposals_observed == 4);
        CHECK(provider.host_frees == 4);
        CHECK_FALSE(provider.destroy_before_dispose);
    }
}

TEST_CASE("shared IO arena expiry retains terminal credit and allocation until late retirement",
          "[gpu_audio][shared_io][arena][deadline]") {
    FakeSharedIoProvider provider;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, arena_config(1)));
    const auto token = publish_and_submit(arena, 100);

    REQUIRE(arena.expire_delivery(token));
    CHECK_FALSE(arena.grant_write(101));
    CHECK(provider.live_allocations == 1);
    REQUIRE(provider.complete(0, SharedIoTerminalStatus::RetiredSuccess) ==
            SharedIoTerminalInbox::PushResult::Accepted);
    const auto drained = arena.drain_completions();
    CHECK(drained.accepted == 1);
    CHECK(drained.rejected_stale_or_duplicate == 0);
    CHECK_FALSE(arena.acquire_output(token.preparation_epoch, token.stream_sequence));
    REQUIRE(arena.discard(token));

    const auto next = arena.grant_write(101);
    REQUIRE(next);
    REQUIRE(arena.discard(next->token));
    REQUIRE(arena.release());
    CHECK(provider.destroy_calls == 1);
    CHECK(provider.retire_calls == 1);
    CHECK(provider.drain_calls == 2);
    CHECK(provider.disposals_observed == 2);
    CHECK(provider.host_frees == 2);
    CHECK_FALSE(provider.destroy_before_dispose);
    CHECK(provider.live_allocations == 0);
}

TEST_CASE("shared IO arena completion drain polls the provider before reading terminals",
          "[gpu_audio][shared_io][arena][completion][poll]") {
    FakeSharedIoProvider provider;
    provider.complete_on_poll = true;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, arena_config(1)));
    const auto token = publish_and_submit(arena, 150);

    CHECK(provider.poll_calls == 0);
    CHECK_FALSE(arena.acquire_output(token.preparation_epoch, token.stream_sequence));

    const auto drained = arena.drain_completions();
    CHECK(provider.poll_calls == 1);
    CHECK(provider.poll_busy_publications == 0);
    REQUIRE(drained.accepted == 1);
    const auto output = arena.acquire_output(token.preparation_epoch, token.stream_sequence);
    REQUIRE(output);
    CHECK(std::to_integer<unsigned>(output->bytes.front()) == 0x6b);
    REQUIRE(arena.release_output({output->token}));
    REQUIRE(arena.release());
}

TEST_CASE("shared IO provider poll retains a busy terminal publication for retry",
          "[gpu_audio][shared_io][arena][completion][poll][concurrency]") {
    FakeSharedIoProvider provider;
    provider.complete_on_poll = true;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, arena_config(1)));
    const auto token = publish_and_submit(arena, 175);
    REQUIRE(provider.hold_stale_completion_claim(0));

    const auto blocked = arena.drain_completions();
    CHECK(blocked.accepted == 0);
    CHECK(provider.poll_calls == 1);
    CHECK(provider.poll_busy_publications == 1);
    CHECK_FALSE(arena.acquire_output(token.preparation_epoch, token.stream_sequence));

    REQUIRE(provider.finish_held_stale_claim() == SharedIoTerminalInbox::PushResult::Rejected);
    const auto retried = arena.drain_completions();
    CHECK(provider.poll_calls == 2);
    CHECK(provider.poll_busy_publications == 1);
    REQUIRE(retried.accepted == 1);
    const auto output = arena.acquire_output(token.preparation_epoch, token.stream_sequence);
    REQUIRE(output);
    CHECK(std::to_integer<unsigned>(output->bytes.front()) == 0x6b);
    REQUIRE(arena.release_output({output->token}));
    REQUIRE(arena.release());
}

TEST_CASE("shared IO arena terminal inbox rejects duplicate and stale generation callbacks",
          "[gpu_audio][shared_io][arena][completion]") {
    FakeSharedIoProvider provider;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, arena_config(1)));
    const auto old = publish_and_submit(arena, 200);

    REQUIRE(provider.complete(0, SharedIoTerminalStatus::RetiredSuccess) ==
            SharedIoTerminalInbox::PushResult::Accepted);
    CHECK(provider.replay(0, SharedIoTerminalStatus::RetiredSuccess) ==
          SharedIoTerminalInbox::PushResult::Rejected);
    CHECK(arena.rejected_terminal_callbacks() == 1);
    REQUIRE(arena.drain_completions().accepted == 1);
    const auto output = arena.acquire_output(old.preparation_epoch, old.stream_sequence);
    REQUIRE(output);
    REQUIRE(arena.release_output({output->token}));

    const auto current = publish_and_submit(arena, 201);
    CHECK(current.slot == old.slot);
    CHECK(current.slot_generation != old.slot_generation);
    CHECK(provider.replay(0, SharedIoTerminalStatus::RetiredFailed) ==
          SharedIoTerminalInbox::PushResult::Rejected);
    CHECK(arena.rejected_terminal_callbacks() == 2);
    REQUIRE(provider.complete(0, SharedIoTerminalStatus::RetiredSuccess) ==
            SharedIoTerminalInbox::PushResult::Accepted);
    REQUIRE(arena.drain_completions().accepted == 1);
    const auto current_output =
        arena.acquire_output(current.preparation_epoch, current.stream_sequence);
    REQUIRE(current_output);
    REQUIRE(arena.release_output({current_output->token}));
    REQUIRE(arena.release());
}

TEST_CASE("shared IO arena retirement waits for terminal drain before reset",
          "[gpu_audio][shared_io][arena][retirement]") {
    FakeSharedIoProvider provider;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, arena_config(1)));
    const auto old_epoch = arena.preparation_epoch();
    const auto old = publish_and_submit(arena, 300);

    arena.begin_retirement();
    CHECK(arena.retiring());
    CHECK_FALSE(arena.grant_write(301));
    CHECK_FALSE(arena.reset_when_quiescent());
    REQUIRE(provider.complete(0, SharedIoTerminalStatus::RetiredFailed) ==
            SharedIoTerminalInbox::PushResult::Accepted);
    REQUIRE(arena.drain_completions().accepted == 1);
    REQUIRE(arena.discard(old));
    REQUIRE(arena.reset_when_quiescent());
    CHECK(arena.preparation_epoch() == old_epoch + 1);

    const auto current = arena.grant_write(301);
    REQUIRE(current);
    CHECK(current->token.preparation_epoch != old.preparation_epoch);
    REQUIRE(arena.discard(current->token));
    REQUIRE(arena.release());
}

TEST_CASE("shared IO arena retirement alone does not invalidate a CPU write lease",
          "[gpu_audio][shared_io][arena][retirement]") {
    FakeSharedIoProvider provider;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, arena_config(1)));
    const auto lease = arena.grant_write(350);
    REQUIRE(lease);

    arena.begin_retirement();
    lease->bytes.front() = std::byte{0x5a};
    CHECK(std::to_integer<unsigned>(lease->bytes.front()) == 0x5a);
    CHECK_FALSE(arena.reset_when_quiescent());

    // Explicit host-quiescent release is the operation that may abandon the
    // lease and destroy its backing allocation.
    REQUIRE(arena.release());
    CHECK(provider.destroy_calls == 1);
}

TEST_CASE("shared IO arena retirement alone preserves a claimed output lease",
          "[gpu_audio][shared_io][arena][retirement]") {
    FakeSharedIoProvider provider;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, arena_config(1)));
    const auto token = publish_and_submit(arena, 360);
    REQUIRE(provider.complete(0, SharedIoTerminalStatus::RetiredSuccess) ==
            SharedIoTerminalInbox::PushResult::Accepted);
    REQUIRE(arena.drain_completions().accepted == 1);
    const auto output = arena.acquire_output(token.preparation_epoch, token.stream_sequence);
    REQUIRE(output);
    REQUIRE(std::to_integer<unsigned>(output->bytes.front()) == 0x6b);

    arena.begin_retirement();
    CHECK(std::to_integer<unsigned>(output->bytes.front()) == 0x6b);
    CHECK_FALSE(arena.reset_when_quiescent());
    REQUIRE(arena.release());
    CHECK(provider.destroy_calls == 1);
}

TEST_CASE("shared IO arena teardown terminally drains pending work before exact destruction",
          "[gpu_audio][shared_io][arena][teardown]") {
    FakeSharedIoProvider provider;
    {
        SharedIoArena arena;
        REQUIRE(arena.prepare(provider, arena_config(2)));
        publish_and_submit(arena, 400);
        publish_and_submit(arena, 401);
        CHECK(provider.live_allocations == 2);
        REQUIRE(arena.release());
    }
    CHECK(provider.drain_calls == 2);
    CHECK(provider.retire_calls == 2);
    CHECK(provider.disposals_observed == 4);
    CHECK(provider.host_frees == 4);
    CHECK_FALSE(provider.destroy_before_dispose);
    CHECK(provider.destroy_calls == 2);
    CHECK(provider.live_allocations == 0);
}

TEST_CASE("shared IO arena preserves imported ownership across a failed drain",
          "[gpu_audio][shared_io][arena][teardown]") {
    FakeSharedIoProvider provider;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, arena_config(1)));
    publish_and_submit(arena, 450);

    provider.force_drain_failure = true;
    CHECK_FALSE(arena.release());
    CHECK(arena.prepared());
    CHECK(provider.live_allocations == 1);
    CHECK(provider.host_frees == 0);
    CHECK(provider.destroy_calls == 0);

    provider.force_drain_failure = false;
    REQUIRE(arena.release());
    CHECK_FALSE(arena.prepared());
    CHECK(provider.live_allocations == 0);
    CHECK(provider.host_frees == 2);
    CHECK(provider.destroy_calls == 1);
    CHECK_FALSE(provider.destroy_before_dispose);
}

TEST_CASE("shared IO arena submission rejection returns reserved terminal credit",
          "[gpu_audio][shared_io][arena][admission]") {
    FakeSharedIoProvider provider;
    provider.reject_submissions = true;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, arena_config(1)));
    const auto lease = arena.grant_write(500);
    REQUIRE(lease);
    REQUIRE(arena.publish_written({lease->token}));
    CHECK_FALSE(arena.submit(lease->token));
    CHECK(arena.available_slots() == 1);
    REQUIRE(arena.release());
}

TEST_CASE("shared IO arena retains rejection rollback across a stale completion claim",
          "[gpu_audio][shared_io][arena][admission][concurrency]") {
    FakeSharedIoProvider provider;
    provider.reject_submissions = true;
    provider.hold_stale_claim_on_reject = true;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, arena_config(1)));
    const auto lease = arena.grant_write(550);
    REQUIRE(lease);
    REQUIRE(arena.publish_written({lease->token}));

    // The provider deterministically holds Completing while returning rejection.
    // submit() must keep the ledger slot and cancellation obligation pinned.
    CHECK_FALSE(arena.submit(lease->token));
    CHECK_FALSE(provider.held_claim_failed);
    CHECK(arena.available_slots() == 0);
    CHECK_FALSE(arena.grant_write(551));

    // The held callback proves stale and restores Reserved. The dispatcher's next
    // drain retries the exact cancellation before returning the physical slot.
    REQUIRE(provider.finish_held_stale_claim() == SharedIoTerminalInbox::PushResult::Rejected);
    const auto drained = arena.drain_completions();
    CHECK(drained.accepted == 0);
    CHECK(arena.available_slots() == 1);
    const auto next = arena.grant_write(551);
    REQUIRE(next);
    REQUIRE(arena.discard(next->token));
    REQUIRE(arena.release());
}

TEST_CASE("shared IO arena completion drain is empty outside a prepared lifetime",
          "[gpu_audio][shared_io][arena][completion]") {
    SharedIoArena arena;
    CHECK(arena.drain_completions().accepted == 0);

    FakeSharedIoProvider refusing;
    refusing.refuse_creation_at = 0;
    CHECK_FALSE(arena.prepare(refusing, arena_config(1)));
    CHECK(arena.drain_completions().accepted == 0);

    FakeSharedIoProvider provider;
    REQUIRE(arena.prepare(provider, arena_config(1)));
    REQUIRE(arena.release());
    CHECK(arena.drain_completions().accepted == 0);
}

TEST_CASE("terminal inbox retained by provider is safe after arena destruction",
          "[gpu_audio][shared_io][arena][teardown]") {
    FakeSharedIoProvider provider;
    {
        SharedIoArena arena;
        REQUIRE(arena.prepare(provider, arena_config(1)));
        publish_and_submit(arena, 600);
    }
    CHECK(provider.live_allocations == 0);
    CHECK(provider.replay(0, SharedIoTerminalStatus::RetiredFailed) ==
          SharedIoTerminalInbox::PushResult::Rejected);
}

TEST_CASE("terminal inbox serializes concurrent duplicate callbacks",
          "[gpu_audio][shared_io][arena][concurrency]") {
    auto inbox = std::make_shared<SharedIoTerminalInbox>();
    REQUIRE(inbox->prepare(1));
    const SharedIoSlotLedger::SlotToken token{
        .slot = 0, .preparation_epoch = 1, .stream_sequence = 700, .slot_generation = 1};
    REQUIRE(inbox->reserve(token));

    std::barrier gate(3);
    SharedIoTerminalInbox::PushResult first = SharedIoTerminalInbox::PushResult::Busy;
    SharedIoTerminalInbox::PushResult second = SharedIoTerminalInbox::PushResult::Busy;
    std::thread a([&] {
        gate.arrive_and_wait();
        first = inbox->push(token, SharedIoTerminalStatus::RetiredSuccess);
    });
    std::thread b([&] {
        gate.arrive_and_wait();
        second = inbox->push(token, SharedIoTerminalStatus::RetiredSuccess);
    });
    gate.arrive_and_wait();
    a.join();
    b.join();

    const unsigned accepted = (first == SharedIoTerminalInbox::PushResult::Accepted ? 1u : 0u) +
                              (second == SharedIoTerminalInbox::PushResult::Accepted ? 1u : 0u);
    CHECK(accepted == 1);
    if (first == SharedIoTerminalInbox::PushResult::Busy)
        CHECK(inbox->push(token, SharedIoTerminalStatus::RetiredSuccess) ==
              SharedIoTerminalInbox::PushResult::Rejected);
    if (second == SharedIoTerminalInbox::PushResult::Busy)
        CHECK(inbox->push(token, SharedIoTerminalStatus::RetiredSuccess) ==
              SharedIoTerminalInbox::PushResult::Rejected);

    SharedIoTerminalInbox::Record record;
    REQUIRE(inbox->try_pop(record));
    CHECK(record.token == token);
    CHECK_FALSE(inbox->try_pop(record));
    CHECK(inbox->quiescent());
}

TEST_CASE("terminal completion claim can publish only once",
          "[gpu_audio][shared_io][arena][concurrency]") {
    auto inbox = std::make_shared<SharedIoTerminalInbox>();
    REQUIRE(inbox->prepare(1));
    const SharedIoSlotLedger::SlotToken token{
        .slot = 0, .preparation_epoch = 4, .stream_sequence = 725, .slot_generation = 9};
    REQUIRE(inbox->reserve(token));
    const auto attempt = inbox->try_claim(0);
    REQUIRE(attempt.claim);
    const auto first_claim = *attempt.claim;
    const auto copied_claim = first_claim;

    std::barrier gate(3);
    auto first = SharedIoTerminalInbox::PushResult::Busy;
    auto second = SharedIoTerminalInbox::PushResult::Busy;
    std::thread a([&] {
        gate.arrive_and_wait();
        first = inbox->finish_claim(first_claim, token, SharedIoTerminalStatus::RetiredSuccess);
    });
    std::thread b([&] {
        gate.arrive_and_wait();
        second = inbox->finish_claim(copied_claim, token, SharedIoTerminalStatus::RetiredFailed);
    });
    gate.arrive_and_wait();
    a.join();
    b.join();

    const unsigned accepted = (first == SharedIoTerminalInbox::PushResult::Accepted ? 1u : 0u) +
                              (second == SharedIoTerminalInbox::PushResult::Accepted ? 1u : 0u);
    CHECK(accepted == 1);
    if (first == SharedIoTerminalInbox::PushResult::Busy)
        CHECK(inbox->finish_claim(first_claim, token, SharedIoTerminalStatus::RetiredSuccess) ==
              SharedIoTerminalInbox::PushResult::Rejected);
    if (second == SharedIoTerminalInbox::PushResult::Busy)
        CHECK(inbox->finish_claim(copied_claim, token, SharedIoTerminalStatus::RetiredFailed) ==
              SharedIoTerminalInbox::PushResult::Rejected);

    SharedIoTerminalInbox::Record record;
    REQUIRE(inbox->try_pop(record));
    CHECK(record.token == token);
    CHECK_FALSE(inbox->try_pop(record));
}

TEST_CASE("matching completion survives a concurrent stale copied claim",
          "[gpu_audio][shared_io][arena][concurrency]") {
    auto inbox = std::make_shared<SharedIoTerminalInbox>();
    REQUIRE(inbox->prepare(1));
    const SharedIoSlotLedger::SlotToken token{
        .slot = 0, .preparation_epoch = 5, .stream_sequence = 726, .slot_generation = 10};
    auto stale = token;
    --stale.slot_generation;
    REQUIRE(inbox->reserve(token));
    const auto attempt = inbox->try_claim(0);
    REQUIRE(attempt.claim);
    const auto matching_claim = *attempt.claim;
    const auto stale_claim = matching_claim;

    // A copied stale claim can temporarily win Completing -> Publishing, but a
    // token mismatch must restore Reserved for the real completion. The retained
    // matching claim then observes that restoration as Busy; it is never allowed
    // to turn a matching Rejected result into a hidden retry.
    REQUIRE(inbox->finish_claim(stale_claim, stale, SharedIoTerminalStatus::RetiredFailed) ==
            SharedIoTerminalInbox::PushResult::Rejected);
    REQUIRE(inbox->finish_claim(matching_claim, token, SharedIoTerminalStatus::RetiredSuccess) ==
            SharedIoTerminalInbox::PushResult::Busy);
    REQUIRE(inbox->push(token, SharedIoTerminalStatus::RetiredSuccess) ==
            SharedIoTerminalInbox::PushResult::Accepted);

    SharedIoTerminalInbox::Record record;
    REQUIRE(inbox->try_pop(record));
    CHECK(record.token == token);
    CHECK(record.status == SharedIoTerminalStatus::RetiredSuccess);
}

TEST_CASE("retained completion claim cannot steal cancellation ownership",
          "[gpu_audio][shared_io][arena][concurrency]") {
    auto inbox = std::make_shared<SharedIoTerminalInbox>();
    REQUIRE(inbox->prepare(1));
    const SharedIoSlotLedger::SlotToken current{
        .slot = 0, .preparation_epoch = 6, .stream_sequence = 727, .slot_generation = 11};
    auto stale = current;
    --stale.slot_generation;
    REQUIRE(inbox->reserve(current));

    const auto completion = inbox->try_claim(0);
    REQUIRE(completion.claim);
    const auto retained_copy = *completion.claim;
    REQUIRE(inbox->finish_claim(*completion.claim, stale, SharedIoTerminalStatus::RetiredFailed) ==
            SharedIoTerminalInbox::PushResult::Rejected);

    // Hold the distinct Cancelling state. The retained completion claim has the
    // same reservation stamp but must not transition Cancelling to Publishing.
    const auto cancellation = inbox->try_claim_cancellation(current);
    REQUIRE(cancellation);
    CHECK(inbox->finish_claim(retained_copy, current, SharedIoTerminalStatus::RetiredSuccess) ==
          SharedIoTerminalInbox::PushResult::Rejected);
    REQUIRE(inbox->finish_cancellation(*cancellation));
    CHECK(inbox->quiescent());

    auto next = current;
    ++next.stream_sequence;
    ++next.slot_generation;
    REQUIRE(inbox->reserve(next));
    CHECK(inbox->finish_claim(retained_copy, current, SharedIoTerminalStatus::RetiredSuccess) ==
          SharedIoTerminalInbox::PushResult::Rejected);
    REQUIRE(inbox->cancel(next));
    CHECK(inbox->quiescent());
}

TEST_CASE("terminal publication makes completed payload visible before output claim",
          "[gpu_audio][shared_io][arena][concurrency]") {
    FakeSharedIoProvider provider;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, arena_config(1)));
    const auto token = publish_and_submit(arena, 750);
    std::barrier gate(2);
    auto terminal = SharedIoTerminalInbox::PushResult::Busy;
    std::thread callback([&] {
        gate.arrive_and_wait();
        terminal = provider.complete(0, SharedIoTerminalStatus::RetiredSuccess);
    });
    gate.arrive_and_wait();

    std::size_t accepted = 0;
    for (unsigned attempt = 0; attempt < 1024 && accepted == 0; ++attempt) {
        accepted += arena.drain_completions().accepted;
        std::this_thread::yield();
    }
    REQUIRE(accepted == 1);
    const auto output = arena.acquire_output(token.preparation_epoch, token.stream_sequence);
    REQUIRE(output);
    CHECK(std::to_integer<unsigned>(output->bytes.front()) == 0x6b);
    REQUIRE(arena.release_output({output->token}));
    callback.join();
    REQUIRE(terminal == SharedIoTerminalInbox::PushResult::Accepted);
    REQUIRE(arena.release());
}

TEST_CASE("terminal inbox stamp closes cancel and re-reserve ABA",
          "[gpu_audio][shared_io][arena][concurrency]") {
    auto inbox = std::make_shared<SharedIoTerminalInbox>();
    REQUIRE(inbox->prepare(1));

    for (std::uint64_t iteration = 0; iteration < 128; ++iteration) {
        const SharedIoSlotLedger::SlotToken old{
            .slot = 0,
            .preparation_epoch = 1,
            .stream_sequence = 800 + iteration * 2,
            .slot_generation = iteration * 2 + 1,
        };
        const SharedIoSlotLedger::SlotToken current{
            .slot = 0,
            .preparation_epoch = 1,
            .stream_sequence = old.stream_sequence + 1,
            .slot_generation = old.slot_generation + 1,
        };
        REQUIRE(inbox->reserve(old));
        std::barrier gate(2);
        auto stale_result = SharedIoTerminalInbox::PushResult::Busy;
        std::thread stale([&] {
            gate.arrive_and_wait();
            stale_result = inbox->push(old, SharedIoTerminalStatus::RetiredFailed);
        });
        gate.arrive_and_wait();
        const bool cancelled = inbox->cancel(old);
        if (cancelled)
            REQUIRE(inbox->reserve(current));
        stale.join();

        if (cancelled) {
            if (stale_result == SharedIoTerminalInbox::PushResult::Busy)
                stale_result = inbox->push(old, SharedIoTerminalStatus::RetiredFailed);
            CHECK(stale_result == SharedIoTerminalInbox::PushResult::Rejected);
            REQUIRE(inbox->cancel(current));
        } else {
            if (stale_result == SharedIoTerminalInbox::PushResult::Busy)
                stale_result = inbox->push(old, SharedIoTerminalStatus::RetiredFailed);
            REQUIRE(stale_result == SharedIoTerminalInbox::PushResult::Accepted);
            SharedIoTerminalInbox::Record record;
            REQUIRE(inbox->try_pop(record));
            CHECK(record.token == old);
        }
        CHECK(inbox->quiescent());
    }
}

TEST_CASE("prepared Pulp bookkeeping and fake shared IO provider allocate nothing",
          "[gpu_audio][shared_io][arena][allocation]") {
    FakeSharedIoProvider provider;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, arena_config(2)));

    bool ok = true;
    std::size_t allocations = 1;
    {
        pulp::test::RtAllocationProbe probe;
        const auto delivered = arena.grant_write(900);
        ok = delivered.has_value();
        if (delivered) {
            ok = arena.publish_written({delivered->token}) && ok;
            ok = arena.submit(delivered->token) && ok;
            ok = provider.complete(delivered->token.slot, SharedIoTerminalStatus::RetiredSuccess) ==
                     SharedIoTerminalInbox::PushResult::Accepted &&
                 ok;
            ok = arena.drain_completions().accepted == 1 && ok;
            const auto output = arena.acquire_output(delivered->token.preparation_epoch,
                                                     delivered->token.stream_sequence);
            ok = output.has_value() && ok;
            if (output)
                ok = arena.release_output({output->token}) && ok;
        }

        const auto expired = arena.grant_write(901);
        ok = expired.has_value() && ok;
        if (expired) {
            ok = arena.publish_written({expired->token}) && ok;
            ok = arena.submit(expired->token) && ok;
            ok = arena.expire_delivery(expired->token) && ok;
            ok = provider.complete(expired->token.slot, SharedIoTerminalStatus::RetiredSuccess) ==
                     SharedIoTerminalInbox::PushResult::Accepted &&
                 ok;
            ok = arena.drain_completions().accepted == 1 && ok;
            ok = arena.discard(expired->token) && ok;
        }

        provider.reject_submissions = true;
        const auto rejected = arena.grant_write(902);
        ok = rejected.has_value() && ok;
        if (rejected) {
            ok = arena.publish_written({rejected->token}) && ok;
            ok = !arena.submit(rejected->token) && ok;
        }
        provider.reject_submissions = false;

        arena.begin_retirement();
        ok = arena.quiescent() && arena.reset_when_quiescent() && ok;
        allocations = probe.allocation_count();
    }
    CHECK(ok);
    CHECK(allocations == 0);
    REQUIRE(arena.release());
}

TEST_CASE("shared IO arena handles multi-slot out-of-order terminal publication",
          "[gpu_audio][shared_io][arena][completion]") {
    FakeSharedIoProvider provider;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, arena_config(3)));
    const auto a = publish_and_submit(arena, 1000);
    const auto b = publish_and_submit(arena, 1001);
    const auto c = publish_and_submit(arena, 1002);
    REQUIRE(provider.complete(c.slot, SharedIoTerminalStatus::RetiredSuccess) ==
            SharedIoTerminalInbox::PushResult::Accepted);
    REQUIRE(provider.complete(a.slot, SharedIoTerminalStatus::RetiredSuccess) ==
            SharedIoTerminalInbox::PushResult::Accepted);
    REQUIRE(provider.complete(b.slot, SharedIoTerminalStatus::RetiredSuccess) ==
            SharedIoTerminalInbox::PushResult::Accepted);
    CHECK(arena.drain_completions().accepted == 3);

    for (const auto token : {c, a, b}) {
        const auto output = arena.acquire_output(token.preparation_epoch, token.stream_sequence);
        REQUIRE(output);
        CHECK(output->token == token);
        CHECK(std::to_integer<unsigned>(output->bytes.front()) == 0x6b);
        REQUIRE(arena.release_output({output->token}));
    }
    REQUIRE(arena.release());
}

TEST_CASE("shared IO arena survives ten thousand prepare release cycles",
          "[gpu_audio][shared_io][arena][lifecycle]") {
    FakeSharedIoProvider provider;
    SharedIoArena arena;
    std::uint64_t previous_epoch = 0;
    constexpr std::uint32_t cycles = 10'000;
    for (std::uint32_t cycle = 0; cycle < cycles; ++cycle) {
        REQUIRE(arena.prepare(provider, arena_config(1)));
        CHECK(arena.preparation_epoch() > previous_epoch);
        previous_epoch = arena.preparation_epoch();
        const auto lease = arena.grant_write(cycle);
        REQUIRE(lease);
        REQUIRE(arena.publish_written({lease->token}));
        REQUIRE(arena.submit(lease->token));
        REQUIRE(provider.complete(lease->token.slot, SharedIoTerminalStatus::RetiredSuccess) ==
                SharedIoTerminalInbox::PushResult::Accepted);
        REQUIRE(arena.drain_completions().accepted == 1);
        const auto output =
            arena.acquire_output(lease->token.preparation_epoch, lease->token.stream_sequence);
        REQUIRE(output);
        REQUIRE(arena.release_output({output->token}));
        REQUIRE(arena.release());
        REQUIRE(provider.live_allocations == 0);
    }
    CHECK(provider.create_calls == cycles);
    CHECK(provider.destroy_calls == cycles);
    CHECK(provider.retire_calls == cycles);
    CHECK(provider.disposals_observed == cycles * 2);
    CHECK(provider.host_frees == cycles * 2);
    CHECK_FALSE(provider.destroy_before_dispose);
}
