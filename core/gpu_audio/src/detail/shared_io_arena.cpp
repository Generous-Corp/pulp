#include "shared_io_arena.hpp"

#include <new>

namespace pulp::gpu_audio::detail {

namespace {

bool owns_host_allocation(const SharedIoArenaProvider::SlotResources& resources) noexcept {
    return resources.opaque != nullptr || resources.input_lifecycle.allocated ||
           resources.output_lifecycle.allocated;
}

bool valid_import(const SharedIoArenaProvider::AllocationLifecycle& lifecycle) noexcept {
    return lifecycle.allocated && lifecycle.import_attempted && lifecycle.import_succeeded &&
           !lifecycle.dispose_observed && !lifecycle.host_freed;
}

void retire_drain_destroy(SharedIoArenaProvider& provider,
                          std::vector<SharedIoArenaProvider::SlotResources>& resources) noexcept {
    for (auto& resource : resources) {
        if (owns_host_allocation(resource))
            provider.retire_slot(resource);
    }
    // retire_slot() releases imported handles. Their dispose callbacks can be
    // deferred even when buffer creation itself failed, so cross the callback
    // drain before freeing any host allocation.
    provider.drain();
    for (auto& resource : resources) {
        if (owns_host_allocation(resource))
            provider.destroy_slot(resource);
    }
}

} // namespace

SharedIoArena::~SharedIoArena() {
    // A conforming provider's terminal drain makes this succeed. If it violates
    // that contract, destruction deliberately leaks the backing allocations,
    // with no recovery claim, rather than free storage the GPU may still access.
    release();
}

bool SharedIoTerminalInbox::prepare(std::uint32_t capacity) {
    if (capacity == 0)
        return false;
    cells_ = std::make_unique<Cell[]>(capacity);
    capacity_ = capacity;
    read_cursor_ = 0;
    rejected_callbacks_.store(0, std::memory_order_relaxed);
    return true;
}

bool SharedIoTerminalInbox::reserve(const SlotToken& token) noexcept {
    if (token.slot >= capacity_)
        return false;
    Cell& cell = cells_[token.slot];
    std::uint64_t observed = cell.control.load(std::memory_order_acquire);
    if (state_of(observed) != CellState::Free || stamp_of(observed) == kMaxStamp)
        return false;
    const std::uint64_t stamp = stamp_of(observed) + 1;
    if (!cell.control.compare_exchange_strong(
            observed, control(stamp, CellState::WritingReservation), std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    cell.token = token;
    cell.status = SharedIoTerminalStatus::RetiredFailed;
    cell.control.store(control(stamp, CellState::Reserved), std::memory_order_release);
    return true;
}

bool SharedIoTerminalInbox::cancel(const SlotToken& token) noexcept {
    const auto claim = try_claim_cancellation(token);
    return claim && finish_cancellation(*claim);
}

std::optional<SharedIoTerminalInbox::CancellationClaim>
SharedIoTerminalInbox::try_claim_cancellation(const SlotToken& token) noexcept {
    if (token.slot >= capacity_)
        return std::nullopt;
    Cell& cell = cells_[token.slot];
    std::uint64_t observed = cell.control.load(std::memory_order_acquire);
    if (state_of(observed) != CellState::Reserved)
        return std::nullopt;
    const std::uint64_t stamp = stamp_of(observed);
    if (!cell.control.compare_exchange_strong(observed, control(stamp, CellState::Cancelling),
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        return std::nullopt;
    }
    if (cell.token != token) {
        cell.control.store(control(stamp, CellState::Reserved), std::memory_order_release);
        return std::nullopt;
    }
    return CancellationClaim{token.slot, control(stamp, CellState::Cancelling)};
}

bool SharedIoTerminalInbox::finish_cancellation(CancellationClaim claim) noexcept {
    if (claim.slot >= capacity_)
        return false;
    Cell& cell = cells_[claim.slot];
    std::uint64_t observed = claim.cancelling_control;
    return cell.control.compare_exchange_strong(
        observed, control(stamp_of(claim.cancelling_control), CellState::Free),
        std::memory_order_acq_rel, std::memory_order_acquire);
}

SharedIoTerminalInbox::ClaimAttempt SharedIoTerminalInbox::try_claim(std::uint32_t slot) noexcept {
    if (slot >= capacity_) {
        rejected_callbacks_.fetch_add(1, std::memory_order_relaxed);
        return {PushResult::Rejected, std::nullopt};
    }
    Cell& cell = cells_[slot];
    std::uint64_t observed = cell.control.load(std::memory_order_acquire);
    if (state_of(observed) == CellState::Completing ||
        state_of(observed) == CellState::Publishing ||
        state_of(observed) == CellState::Cancelling ||
        state_of(observed) == CellState::WritingReservation ||
        state_of(observed) == CellState::Consuming) {
        return {PushResult::Busy, std::nullopt};
    }
    if (state_of(observed) != CellState::Reserved) {
        rejected_callbacks_.fetch_add(1, std::memory_order_relaxed);
        return {PushResult::Rejected, std::nullopt};
    }
    const std::uint64_t stamp = stamp_of(observed);
    if (!cell.control.compare_exchange_strong(observed, control(stamp, CellState::Completing),
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        return {PushResult::Busy, std::nullopt};
    }
    return {PushResult::Accepted, CompletionClaim{slot, control(stamp, CellState::Completing)}};
}

SharedIoTerminalInbox::PushResult
SharedIoTerminalInbox::finish_claim(CompletionClaim claim, const SlotToken& token,
                                    SharedIoTerminalStatus status) noexcept {
    if (claim.slot >= capacity_)
        return PushResult::Rejected;
    Cell& cell = cells_[claim.slot];
    const std::uint64_t stamp = stamp_of(claim.completing_control);
    std::uint64_t observed = claim.completing_control;
    if (!cell.control.compare_exchange_strong(observed, control(stamp, CellState::Publishing),
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        const bool same_claim_can_retry =
            stamp_of(observed) == stamp && (state_of(observed) == CellState::Publishing ||
                                            state_of(observed) == CellState::Reserved);
        return same_claim_can_retry ? PushResult::Busy : PushResult::Rejected;
    }
    if (cell.token != token) {
        cell.control.store(control(stamp, CellState::Reserved), std::memory_order_release);
        rejected_callbacks_.fetch_add(1, std::memory_order_relaxed);
        return PushResult::Rejected;
    }
    cell.status = status;
    cell.control.store(control(stamp, CellState::Ready), std::memory_order_release);
    return PushResult::Accepted;
}

SharedIoTerminalInbox::PushResult
SharedIoTerminalInbox::push(const SlotToken& token, SharedIoTerminalStatus status) noexcept {
    auto attempt = try_claim(token.slot);
    if (!attempt.claim)
        return attempt.result;
    return finish_claim(*attempt.claim, token, status);
}

bool SharedIoTerminalInbox::try_pop(Record& record) noexcept {
    for (std::uint32_t offset = 0; offset < capacity_; ++offset) {
        const std::uint32_t index = (read_cursor_ + offset) % capacity_;
        Cell& cell = cells_[index];
        std::uint64_t observed = cell.control.load(std::memory_order_acquire);
        if (state_of(observed) != CellState::Ready)
            continue;
        const std::uint64_t stamp = stamp_of(observed);
        if (!cell.control.compare_exchange_strong(observed, control(stamp, CellState::Consuming),
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
            continue;
        }
        record = {cell.token, cell.status};
        cell.control.store(control(stamp, CellState::Free), std::memory_order_release);
        read_cursor_ = (index + 1) % capacity_;
        return true;
    }
    return false;
}

bool SharedIoTerminalInbox::quiescent() const noexcept {
    for (std::uint32_t index = 0; index < capacity_; ++index) {
        if (state_of(cells_[index].control.load(std::memory_order_acquire)) != CellState::Free)
            return false;
    }
    return true;
}

bool SharedIoArena::prepare(SharedIoArenaProvider& provider, const Config& config) {
    if (prepared_ && !release())
        return false;
    if (config.slots == 0 || config.input_bytes_per_slot == 0 ||
        config.output_bytes_per_slot == 0) {
        return false;
    }

    try {
        if (!ledger_.prepare(config.slots))
            return false;
        if (config.prepare_fault == Config::PrepareFault::AfterLedger)
            throw std::bad_alloc{};
        resources_.assign(config.slots, {});
        rejected_submissions_.assign(config.slots, {});
        if (config.prepare_fault == Config::PrepareFault::AfterResourceTables)
            throw std::bad_alloc{};
        terminal_inbox_ = std::make_shared<SharedIoTerminalInbox>();
        if (!terminal_inbox_->prepare(config.slots))
            throw std::bad_alloc{};
        if (config.prepare_fault == Config::PrepareFault::AfterInbox)
            throw std::bad_alloc{};

        provider_ = &provider;
        for (std::uint32_t slot = 0; slot < config.slots; ++slot) {
            const bool created = provider_->create_slot(
                slot, config.input_bytes_per_slot, config.output_bytes_per_slot, resources_[slot]);
            const auto& resource = resources_[slot];
            if (created && resource.input != nullptr && resource.output != nullptr &&
                resource.input_size == config.input_bytes_per_slot &&
                resource.output_size == config.output_bytes_per_slot &&
                resource.opaque != nullptr && valid_import(resource.input_lifecycle) &&
                valid_import(resource.output_lifecycle)) {
                continue;
            }
            throw std::bad_alloc{};
        }
    } catch (...) {
        // A failed or malformed creation can still own successful imports and
        // deferred disposal callbacks. Roll the whole attempted transaction
        // through release, drain, then host free before making prepare retryable.
        retire_drain_destroy(provider, resources_);
        resources_.clear();
        rejected_submissions_.clear();
        terminal_inbox_.reset();
        provider_ = nullptr;
        return false;
    }

    prepared_ = true;
    return true;
}

std::optional<SharedIoArena::WriteLease>
SharedIoArena::grant_write(std::uint64_t stream_sequence) noexcept {
    if (!prepared_)
        return std::nullopt;
    const auto token = ledger_.acquire(stream_sequence);
    if (!token)
        return std::nullopt;
    const auto& resource = resources_[token->slot];
    return WriteLease{*token, {resource.input, resource.input_size}};
}

const SharedIoArenaProvider::SlotResources*
SharedIoArena::resources_for(const SlotToken& token) const noexcept {
    if (!prepared_ || token.preparation_epoch != ledger_.preparation_epoch() ||
        token.slot >= resources_.size()) {
        return nullptr;
    }
    return &resources_[token.slot];
}

bool SharedIoArena::submit(const SlotToken& token) noexcept {
    const auto* resource = resources_for(token);
    if (resource == nullptr || provider_ == nullptr)
        return false;
    const auto claimed = ledger_.claim_submission(token);
    if (!claimed)
        return false;
    if (!terminal_inbox_->reserve(*claimed)) {
        ledger_.complete_gpu(*claimed, SharedIoSlotLedger::GpuCompletion::Failed);
        ledger_.discard(*claimed);
        return false;
    }
    if (provider_->submit(*resource, *claimed, terminal_inbox_))
        return true;

    // A callback for an older generation may temporarily own Completing. Keep
    // this generation and its terminal credit pinned until its exact reservation
    // can be cancelled; freeing the ledger slot first would orphan that credit.
    if (!rollback_rejected_submission(*claimed))
        rejected_submissions_[claimed->slot] = {*claimed, true};
    return false;
}

bool SharedIoArena::rollback_rejected_submission(const SlotToken& token) noexcept {
    if (!terminal_inbox_->cancel(token))
        return false;
    if (!ledger_.complete_gpu(token, SharedIoSlotLedger::GpuCompletion::Failed))
        return false;
    return ledger_.discard(token);
}

void SharedIoArena::retry_rejected_submissions() noexcept {
    for (auto& rejected : rejected_submissions_) {
        if (!rejected.pending)
            continue;
        if (rollback_rejected_submission(rejected.token))
            rejected = {};
    }
}

SharedIoArena::CompletionDrain SharedIoArena::drain_completions() noexcept {
    CompletionDrain result;
    if (!terminal_inbox_)
        return result;
    retry_rejected_submissions();
    SharedIoTerminalInbox::Record record;
    while (terminal_inbox_->try_pop(record)) {
        auto& rejected = rejected_submissions_[record.token.slot];
        if (rejected.pending && rejected.token == record.token) {
            const bool completed =
                ledger_.complete_gpu(record.token, SharedIoSlotLedger::GpuCompletion::Failed);
            const bool discarded = completed && ledger_.discard(record.token);
            if (discarded) {
                rejected = {};
                ++result.accepted;
            } else {
                ++result.rejected_stale_or_duplicate;
            }
            continue;
        }
        const auto completion = record.status == CompletionStatus::RetiredSuccess
                                    ? SharedIoSlotLedger::GpuCompletion::Success
                                    : SharedIoSlotLedger::GpuCompletion::Failed;
        if (ledger_.complete_gpu(record.token, completion))
            ++result.accepted;
        else
            ++result.rejected_stale_or_duplicate;
    }
    retry_rejected_submissions();
    return result;
}

std::optional<SharedIoArena::OutputLease>
SharedIoArena::acquire_output(std::uint64_t expected_epoch,
                              std::uint64_t expected_sequence) noexcept {
    if (!prepared_)
        return std::nullopt;
    const auto token = ledger_.acquire_output(expected_epoch, expected_sequence);
    if (!token)
        return std::nullopt;
    const auto& resource = resources_[token->slot];
    return OutputLease{*token, {resource.output, resource.output_size}};
}

void SharedIoArena::discard_cpu_owned_after_quiescence() noexcept {
    for (std::uint32_t slot = 0; slot < ledger_.capacity(); ++slot) {
        const auto token = ledger_.active_token(slot);
        if (token)
            ledger_.discard_after_cpu_quiescence(*token);
    }
}

void SharedIoArena::begin_retirement() noexcept {
    if (!prepared_)
        return;
    ledger_.begin_retirement();
}

bool SharedIoArena::reset_when_quiescent() noexcept {
    if (!prepared_ || !terminal_inbox_->quiescent())
        return false;
    return ledger_.reset_when_quiescent();
}

bool SharedIoArena::release() noexcept {
    if (!prepared_)
        return true;
    begin_retirement();
    provider_->drain();
    drain_completions();
    // release() is the explicit assertion that CPU producers/consumers have
    // quiesced; begin_retirement() alone never invalidates their spans.
    discard_cpu_owned_after_quiescence();
    if (!ledger_.quiescent() || !terminal_inbox_->quiescent())
        return false;

    // The first drain retired accepted submissions. Releasing the imported
    // buffer handles can enqueue disposal callbacks of its own, so drain again
    // before the provider frees the host allocations.
    for (auto& resource : resources_)
        provider_->retire_slot(resource);
    provider_->drain();
    for (auto& resource : resources_)
        provider_->destroy_slot(resource);
    resources_.clear();
    rejected_submissions_.clear();
    terminal_inbox_.reset();
    provider_ = nullptr;
    prepared_ = false;
    return true;
}

} // namespace pulp::gpu_audio::detail
