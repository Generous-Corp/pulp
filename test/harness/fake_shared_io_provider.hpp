#pragma once

// The one deterministic SharedIoArenaProvider used by the shared-I/O tests and
// the provider conformance kit. Every knob is a plain public field so a test
// states the exact provider behaviour it depends on. Prepared submission,
// service, completion and drain paths allocate nothing: create_slot() sizes
// every per-slot record, and the terminal inbox is shared, never copied.

#include "detail/shared_io_arena.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <vector>

namespace pulp::test {

class FakeSharedIoProvider : public pulp::gpu_audio::detail::SharedIoArenaProvider {
  public:
    using SharedIoProviderCapabilities = pulp::gpu_audio::detail::SharedIoProviderCapabilities;
    using SharedIoProviderIdentity = pulp::gpu_audio::detail::SharedIoProviderIdentity;
    using SharedIoTerminalInbox = pulp::gpu_audio::detail::SharedIoTerminalInbox;
    using SharedIoTerminalStatus = pulp::gpu_audio::detail::SharedIoTerminalStatus;

    // Observable lifecycle facts that must outlive the provider (a session may
    // destroy it) or be shared with a test-owned prepared program.
    struct Lifecycle {
        bool program_released = false;
        bool program_destroyed = false;
        bool retired_before_program_release = false;
        bool provider_destroyed = false;
        std::uint64_t retired = 0;
        std::uint32_t submits = 0;
        std::uint32_t polls = 0;
        std::uint32_t drains = 0;
    };

    // What the provider writes into the output slot when work retires.
    // MarkFirstByte writes 0x6b to byte 0 only; Affine maps every float
    // through `scale * x + bias` (scale 1, bias 0 is an exact byte copy).
    enum class Transform : std::uint8_t { MarkFirstByte, Affine };

    // Provider-visible faults with the same observable contract as the Dawn
    // provider's fault injection: loss makes every accepted submission retire
    // failed, only at the drain boundary, and leaves the provider
    // non-resumable; DelayCompletion withholds the
    // fault slot from service until drain(); RejectBeforeSubmit refuses the
    // fault slot; HoldTerminalBusy makes the first terminal publication find
    // its inbox cell busy so it must be retained and retried.
    enum class Fault : std::uint8_t {
        None,
        ForceLossBeforeSubmit,
        ForceLossBetweenSubmitAndCompletionRegistration,
        ForceLossAfterCompletionRegistration,
        DelayCompletion,
        RejectBeforeSubmit,
        HoldTerminalBusy,
    };

    // Planted contract violations (mutant providers). The conformance kit
    // must turn each of these red; none is reachable unless a test selects it.
    enum class Violation : std::uint8_t {
        None,
        // A second push for an already retired token.
        DuplicateTerminal,
        // Output is off by a scale factor.
        ScaleError,
        // Lost work retires failed on service, before the drain boundary.
        EarlyFailedTerminal,
        // Lost work retires successfully at drain.
        SuccessAfterLoss,
        // drain() publishes the withheld terminal twice.
        DuplicateLateTerminal,
        // drain() returns true without retiring withheld work.
        DrainSkipsWithheld,
        // A refused submission still publishes a terminal.
        PushOnRefusal,
        // A busy publication is dropped instead of retained.
        DropBusyTerminal,
        // drain() returns before publishing its terminals; the next service
        // publishes them.
        DeferDrainTerminalToPoll,
        // Service after a drain re-publishes a terminal it already retired.
        CallbackAfterDrain,
        // drain() returns true but leaves lost work pinned without a terminal.
        PinLostWork,
        // can_resume_after_drain() is true after device loss.
        ResumeAfterLoss,
        // can_resume_after_drain() is true, then the next epoch is refused.
        FalseResumeClaim,
        // A resubmission re-publishes the slot's previous token.
        ReplayStaleOnResubmit,
    };

    struct Allocation {
        std::uint32_t slot = 0;
        std::uint64_t generation = 1;
        bool retired = false;
        bool partial = false;
        std::unique_ptr<std::byte[]> input;
        std::unique_ptr<std::byte[]> output;
    };

    struct Pending {
        SlotToken token;
        std::shared_ptr<SharedIoTerminalInbox> inbox;
        bool accepted = false;
        bool terminal = false;
        bool completion_pending = false;
        bool lost = false;
        bool deferred_by_drain = false;
        SharedIoTerminalStatus completion_status = SharedIoTerminalStatus::RetiredFailed;
        const std::byte* input = nullptr;
        std::size_t input_size = 0;
        std::byte* output = nullptr;
        std::size_t output_size = 0;
        bool transform = true;
    };

    explicit FakeSharedIoProvider(
        std::shared_ptr<Lifecycle> lifecycle = std::make_shared<Lifecycle>())
        : lifecycle_state(std::move(lifecycle)) {}

    ~FakeSharedIoProvider() override {
        lifecycle_state->provider_destroyed = true;
    }

    FakeSharedIoProvider(const FakeSharedIoProvider&) = delete;
    FakeSharedIoProvider& operator=(const FakeSharedIoProvider&) = delete;

    static SharedIoProviderIdentity authenticated_fake_identity() {
        SharedIoProviderIdentity value;
        value.authenticated = true;
        value.provider_revision = "fake-provider-v1";
        value.adapter_name = "fake-adapter";
        value.adapter_backend = "fake";
        value.adapter_vendor_id = 1;
        value.adapter_device_id = 1;
        value.native_runtime_authenticated = true;
        value.native_runtime_name = "fake-native";
        value.native_runtime_backend = "fake";
        return value;
    }

    SharedIoProviderIdentity provider_identity() const noexcept override {
        return identity;
    }
    SharedIoProviderCapabilities provider_capabilities() const noexcept override {
        return capabilities;
    }

    bool create_slot(std::uint32_t slot, std::size_t input_bytes, std::size_t output_bytes,
                     SlotResources& resources) noexcept override {
        ++create_calls;
        if (slot == refuse_creation_at)
            return false;
        try {
            pending.resize(static_cast<std::size_t>(slot) + 1);
            previous.resize(static_cast<std::size_t>(slot) + 1);
            retiring.resize(static_cast<std::size_t>(slot) + 1, nullptr);
            slots_.reserve(static_cast<std::size_t>(slot) + 1);
        } catch (...) {
            return false;
        }
        auto allocation = std::unique_ptr<Allocation>(new (std::nothrow) Allocation);
        if (!allocation)
            return false;
        allocation->slot = slot;
        allocation->input.reset(new (std::nothrow) std::byte[input_bytes]());
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
            resources.opaque = adopt(std::move(allocation));
            ++partial_allocations_created;
            return false;
        }
        allocation->output.reset(new (std::nothrow) std::byte[output_bytes]());
        if (!allocation->output) {
            resources.opaque = adopt(std::move(allocation));
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
            resources.opaque = adopt(std::move(allocation));
            ++partial_allocations_created;
            return false;
        }
        resources = {
            .input = allocation->input.get(),
            .input_size = input_bytes,
            .output = allocation->output.get(),
            .output_size = output_bytes,
            .opaque = nullptr,
            .input_lifecycle =
                {
                    .allocated = !invalid_lifecycle,
                    .import_attempted = true,
                    .import_succeeded = true,
                },
            .output_lifecycle =
                {
                    .allocated = !invalid_lifecycle,
                    .import_attempted = true,
                    .import_succeeded = true,
                },
        };
        resources.opaque = adopt(std::move(allocation));
        resources.storage_kind = storage_kind;
        if (storage_kind == StorageKind::Staged) {
            resources.input_lifecycle.import_attempted = false;
            resources.input_lifecycle.import_succeeded = false;
            resources.output_lifecycle.import_attempted = false;
            resources.output_lifecycle.import_succeeded = false;
        }
        if (false_import_claim)
            resources.output_lifecycle.import_attempted = true;
        if (slot == malformed_success_at)
            resources.output = nullptr;
        return true;
    }

    void retire_slot(SlotResources& resources) noexcept override {
        if (resources.opaque == nullptr)
            return;
        if (require_program_release && !lifecycle_state->program_released)
            lifecycle_state->retired_before_program_release = true;
        auto* allocation = static_cast<Allocation*>(resources.opaque);
        allocation->retired = true;
        ++allocation->generation;
        if (allocation->slot < retiring.size())
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
        if (allocation->slot < retiring.size() && retiring[allocation->slot] == &resources)
            retiring[allocation->slot] = nullptr;
        for (auto cursor = slots_.begin(); cursor != slots_.end(); ++cursor) {
            if (cursor->get() == allocation) {
                slots_.erase(cursor);
                break;
            }
        }
        resources.input = nullptr;
        resources.input_size = 0;
        resources.output = nullptr;
        resources.output_size = 0;
        resources.opaque = nullptr;
        ++destroy_calls;
        --live_allocations;
    }

    bool acquire_slot_buffers(const SlotResources& resources,
                              SlotBufferHandle& handle) const noexcept override {
        const auto* allocation = static_cast<const Allocation*>(resources.opaque);
        if (!allocation || allocation->retired)
            return false;
        handle = {};
        handle.provider = this;
        handle.device = this;
        handle.input_buffer = allocation->input.get();
        handle.output_buffer = allocation->output.get();
        handle.slot = allocation->slot;
        handle.generation = allocation->generation;
        handle.lifetime = lifetime_;
        return true;
    }

    bool validate_slot_buffers(const SlotBufferHandle& handle) const noexcept override {
        if (handle.provider != this || handle.device != this || handle.lifetime.expired())
            return false;
        for (const auto& allocation : slots_) {
            if (!allocation->retired && handle.slot == allocation->slot &&
                handle.input_buffer == allocation->input.get() &&
                handle.output_buffer == allocation->output.get() &&
                handle.generation == allocation->generation)
                return true;
        }
        return false;
    }

    bool submit(const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> inbox) noexcept override {
        if (require_program) {
            ++lifecycle_state->submits;
            return false;
        }
        return submit_program(resources, token, std::move(inbox));
    }

    // Entry point for a test-owned SharedIoPreparedProgram; identical to the
    // provider's own submit() contract. A program that writes its own output
    // passes apply_transform = false.
    bool submit_program(const SlotResources& resources, SlotToken token,
                        std::shared_ptr<SharedIoTerminalInbox> inbox,
                        bool apply_transform = true) noexcept {
        ++lifecycle_state->submits;
        const bool refused = !accept_submissions ||
                             (fault == Fault::RejectBeforeSubmit && token.slot == fault_slot) ||
                             (violation == Violation::FalseResumeClaim && drained_once_);
        if (refused) {
            if (hold_stale_claim_on_reject && inbox)
                hold_rejected_claim(token, inbox);
            if (violation == Violation::PushOnRefusal && inbox)
                (void)inbox->push(token, SharedIoTerminalStatus::RetiredFailed);
            return false;
        }
        if (!inbox || token.slot >= pending.size())
            return false;
        Pending& request = pending[token.slot];
        if (request.accepted && !request.terminal)
            return false;
        if (request.accepted)
            previous[token.slot] = request;
        if (violation == Violation::ReplayStaleOnResubmit && previous[token.slot].accepted)
            (void)previous[token.slot].inbox->push(previous[token.slot].token,
                                                   SharedIoTerminalStatus::RetiredSuccess);
        if (fault == Fault::ForceLossBeforeSubmit ||
            fault == Fault::ForceLossBetweenSubmitAndCompletionRegistration ||
            fault == Fault::ForceLossAfterCompletionRegistration) {
            force_lost_ = true;
            ++fault_injections;
        }
        request = Pending{
            .token = token,
            .inbox = std::move(inbox),
            .accepted = true,
            .lost = force_lost_,
            .input = resources.input,
            .input_size = resources.input_size,
            .output = resources.output,
            .output_size = resources.output_size,
            .transform = apply_transform,
        };
        if (transform_at_submit)
            write_output(request);
        if (complete_in_submit && !defer_terminals) {
            if (complete(token.slot, terminal_status) ==
                SharedIoTerminalInbox::PushResult::Accepted)
                return true;
            // A refusal must never push later: forget the record entirely.
            request = Pending{};
            return false;
        }
        return true;
    }

    void poll() noexcept override {
        ++poll_calls;
        ++lifecycle_state->polls;
        if (violation == Violation::CallbackAfterDrain && drained_replay_) {
            (void)drained_replay_->inbox->push(drained_replay_->token,
                                               SharedIoTerminalStatus::RetiredSuccess);
        }
        for (std::size_t index = 0; index < pending.size(); ++index) {
            if (pending[index].deferred_by_drain) {
                pending[index].deferred_by_drain = false;
                (void)complete(index, SharedIoTerminalStatus::RetiredFailed);
            }
        }
        // Without complete_on_poll a test thread owns completion, so service
        // leaves every record (including a busy retry) to it and to drain().
        if (!complete_on_poll)
            return;
        for (std::size_t index = 0; index < pending.size(); ++index) {
            auto& request = pending[index];
            if (!request.accepted || (request.terminal && !request.completion_pending))
                continue;
            if (request.completion_pending) {
                if (retry(index) == SharedIoTerminalInbox::PushResult::Busy)
                    ++poll_busy_publications;
                continue;
            }
            if (fault == Fault::DelayCompletion && index == fault_slot)
                continue;
            // Lost work is certified only by the drain boundary.
            if (request.lost && violation != Violation::EarlyFailedTerminal)
                continue;
            if (complete(index, terminal_status) == SharedIoTerminalInbox::PushResult::Busy)
                ++poll_busy_publications;
        }
    }

    bool device_lost() const noexcept override {
        return lost || force_lost_;
    }

    bool can_resume_after_drain() const noexcept override {
        if (violation == Violation::ResumeAfterLoss)
            return true;
        if (!resume_supported || force_lost_)
            return false;
        for (const auto& request : pending) {
            if (request.accepted && !request.terminal)
                return false;
        }
        return true;
    }

    bool drain() noexcept override {
        ++drain_calls;
        ++lifecycle_state->drains;
        if (force_drain_failure || !allow_drain)
            return false;
        drained_once_ = true;
        bool published = true;
        for (std::size_t index = 0; index < pending.size(); ++index) {
            auto& request = pending[index];
            if (!request.accepted || (request.terminal && !request.completion_pending))
                continue;
            if (request.completion_pending) {
                published =
                    retry(index) == SharedIoTerminalInbox::PushResult::Accepted && published;
                continue;
            }
            const bool withheld = fault == Fault::DelayCompletion && index == fault_slot;
            if ((violation == Violation::DrainSkipsWithheld && withheld) ||
                (violation == Violation::PinLostWork && request.lost))
                continue;
            if (violation == Violation::DeferDrainTerminalToPoll) {
                request.deferred_by_drain = true;
                continue;
            }
            const auto status =
                drain_completes ? terminal_status : SharedIoTerminalStatus::RetiredFailed;
            const auto result = complete(index, status);
            if (result != SharedIoTerminalInbox::PushResult::Accepted) {
                drain_push_failed = true;
                published = false;
                continue;
            }
            if (violation == Violation::DuplicateLateTerminal && withheld)
                (void)request.inbox->push(request.token, request.completion_status);
            if (violation == Violation::CallbackAfterDrain)
                drained_replay_ = request;
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
        return published;
    }

    // Publishes the terminal for the accepted request in `index`. Success
    // payload is written before Ready is release-published; a lost request
    // always retires failed. Busy retains the record for poll()/drain().
    SharedIoTerminalInbox::PushResult complete(std::size_t index,
                                               SharedIoTerminalStatus status) noexcept {
        if (index >= pending.size())
            return SharedIoTerminalInbox::PushResult::Rejected;
        Pending& request = pending[index];
        if (!request.accepted)
            return SharedIoTerminalInbox::PushResult::Rejected;
        if (request.lost && violation != Violation::SuccessAfterLoss)
            status = SharedIoTerminalStatus::RetiredFailed;
        if (request.lost && violation == Violation::SuccessAfterLoss)
            status = SharedIoTerminalStatus::RetiredSuccess;
        if (status == SharedIoTerminalStatus::RetiredSuccess && !transform_at_submit)
            write_output(request);
        // Finish every access to Pending before publishing Ready: immediate
        // dispatcher reuse after the release store may overwrite this record.
        request.completion_status = status;
        request.completion_pending = false;
        request.terminal = true;
        const auto inbox = request.inbox;
        const auto token = request.token;
        if (fault == Fault::HoldTerminalBusy && !busy_fault_consumed_) {
            busy_fault_consumed_ = true;
            auto attempt = inbox->try_claim(token.slot);
            if (attempt.claim) {
                held_busy_claim_ = *attempt.claim;
                held_busy_inbox_ = inbox;
                held_busy_token_ = token;
                ++fault_injections;
            }
        }
        const auto result = inbox->push(token, status);
        if (result == SharedIoTerminalInbox::PushResult::Busy) {
            ++terminal_busy_retries;
            if (violation != Violation::DropBusyTerminal) {
                request.terminal = false;
                request.completion_pending = true;
            }
        } else if (result == SharedIoTerminalInbox::PushResult::Accepted) {
            ++lifecycle_state->retired;
            if (violation == Violation::DuplicateTerminal)
                (void)inbox->push(token, status);
        }
        return result;
    }

    // Completes the accepted request carrying `sequence` with terminal_status.
    void complete_sequence(std::uint64_t sequence) noexcept {
        for (std::size_t index = 0; index < pending.size(); ++index) {
            const auto& request = pending[index];
            if (request.accepted && !request.terminal &&
                request.token.stream_sequence == sequence) {
                (void)complete(index, terminal_status);
                return;
            }
        }
    }

    void complete_all() noexcept {
        for (std::size_t index = 0; index < pending.size(); ++index) {
            const auto& request = pending[index];
            if (request.accepted && !request.terminal)
                (void)complete(index, terminal_status);
        }
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

    // Retires and frees every slot a test abandoned without an arena release.
    void cleanup_abandoned_for_test() noexcept {
        while (!slots_.empty()) {
            SlotResources resources;
            resources.opaque = slots_.back().get();
            retire_slot(resources);
            destroy_slot(resources);
        }
    }

    std::size_t live_slot_count() const noexcept {
        return slots_.size();
    }

    // Identity, capabilities and storage.
    SharedIoProviderIdentity identity;
    SharedIoProviderCapabilities capabilities;
    StorageKind storage_kind = StorageKind::ImportedHostPointer;

    // Program semantics.
    Transform transform = Transform::MarkFirstByte;
    float scale = 1.0f;
    float bias = 0.0f;
    bool transform_at_submit = false;
    // submit() refuses; only submit_program() from a prepared program accepts.
    bool require_program = false;
    // retire_slot() before the program's release is recorded as a violation.
    bool require_program_release = false;

    // Completion semantics.
    bool accept_submissions = true;
    bool complete_on_poll = false;
    bool complete_in_submit = false;
    bool defer_terminals = false;
    // drain() retires accepted work with terminal_status; otherwise failed.
    bool drain_completes = false;
    bool allow_drain = true;
    bool force_drain_failure = false;
    bool lost = false;
    bool resume_supported = false;
    SharedIoTerminalStatus terminal_status = SharedIoTerminalStatus::RetiredSuccess;

    // Fault injection.
    Fault fault = Fault::None;
    std::uint32_t fault_slot = 0;
    Violation violation = Violation::None;

    // Creation faults.
    std::uint32_t refuse_creation_at = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t fail_after_partial_at = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t fail_output_import_at = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t malformed_success_at = std::numeric_limits<std::uint32_t>::max();
    bool false_import_claim = false;
    bool invalid_lifecycle = false;
    bool hold_stale_claim_on_reject = false;
    bool held_claim_failed = false;

    // Observations.
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
    std::uint64_t terminal_busy_retries = 0;
    std::uint64_t fault_injections = 0;
    bool drain_push_failed = false;
    bool destroy_before_dispose = false;

    std::vector<Pending> pending;
    std::vector<Pending> previous;
    std::vector<SlotResources*> retiring;
    std::optional<SharedIoTerminalInbox::CompletionClaim> held_claim;
    std::shared_ptr<SharedIoTerminalInbox> held_inbox;
    SlotToken held_stale_token;
    std::shared_ptr<Lifecycle> lifecycle_state;

  private:
    void* adopt(std::unique_ptr<Allocation> allocation) noexcept {
        auto* raw = allocation.get();
        slots_.push_back(std::move(allocation));
        ++live_allocations;
        return raw;
    }

    void write_output(const Pending& request) noexcept {
        if (!request.transform || request.output == nullptr || request.output_size == 0)
            return;
        if (transform == Transform::MarkFirstByte) {
            request.output[0] = std::byte{0x6b};
            return;
        }
        const float bias_value = bias;
        const float scale_value = violation == Violation::ScaleError ? scale * 1.5f : scale;
        const auto bytes = request.input ? std::min(request.input_size, request.output_size) : 0;
        if (scale_value == 1.0f && bias_value == 0.0f) {
            std::memcpy(request.output, request.input, bytes);
            return;
        }
        const auto values = bytes / sizeof(float);
        for (std::size_t index = 0; index < values; ++index) {
            float sample = 0.0f;
            std::memcpy(&sample, request.input + index * sizeof(float), sizeof(float));
            sample = sample * scale_value + bias_value;
            std::memcpy(request.output + index * sizeof(float), &sample, sizeof(float));
        }
    }

    SharedIoTerminalInbox::PushResult retry(std::size_t index) noexcept {
        Pending& request = pending[index];
        if (held_busy_claim_ && held_busy_inbox_) {
            // Release the deliberately held cell as a stale callback would.
            auto stale = held_busy_token_;
            ++stale.slot_generation;
            (void)held_busy_inbox_->finish_claim(*held_busy_claim_, stale,
                                                 request.completion_status);
            held_busy_claim_.reset();
            held_busy_inbox_.reset();
        }
        request.completion_pending = false;
        request.terminal = true;
        const auto inbox = request.inbox;
        const auto result = inbox->push(request.token, request.completion_status);
        if (result == SharedIoTerminalInbox::PushResult::Busy) {
            request.terminal = false;
            request.completion_pending = true;
        } else if (result == SharedIoTerminalInbox::PushResult::Accepted) {
            ++lifecycle_state->retired;
        }
        return result;
    }

    void hold_rejected_claim(const SlotToken& token,
                             const std::shared_ptr<SharedIoTerminalInbox>& inbox) noexcept {
        auto attempt = inbox->try_claim(token.slot);
        if (!attempt.claim) {
            held_claim_failed = true;
            return;
        }
        held_claim = *attempt.claim;
        held_inbox = inbox;
        held_stale_token = token;
        if (held_stale_token.slot_generation > 0)
            --held_stale_token.slot_generation;
    }

    bool force_lost_ = false;
    bool drained_once_ = false;
    bool busy_fault_consumed_ = false;
    std::optional<SharedIoTerminalInbox::CompletionClaim> held_busy_claim_;
    std::shared_ptr<SharedIoTerminalInbox> held_busy_inbox_;
    SlotToken held_busy_token_;
    std::optional<Pending> drained_replay_;
    std::shared_ptr<const void> lifetime_ = std::make_shared<int>(0);
    std::vector<std::unique_ptr<Allocation>> slots_;
};

} // namespace pulp::test
