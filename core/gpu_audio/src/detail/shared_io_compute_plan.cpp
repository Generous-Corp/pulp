#include "shared_io_compute_plan.hpp"

#include <algorithm>
#include <chrono>

namespace pulp::gpu_audio::detail {

bool SharedIoComputePlan::prepare(SharedIoArenaProvider& provider, const Config& config) {
    if (config.slots == 0 || config.input_bytes_per_slot == 0 ||
        config.output_bytes_per_slot == 0 || !arena_.prepare(
            provider, {.slots = config.slots,
                       .input_bytes_per_slot = config.input_bytes_per_slot,
                       .output_bytes_per_slot = config.output_bytes_per_slot}))
        return false;
    pending_.assign(config.slots, {});
    completions_.assign(static_cast<std::size_t>(config.slots) * 2 + 1, {});
    completion_read_ = completion_write_ = 0;
    telemetry_ = {};
    return true;
}

std::optional<SharedIoArena::WriteLease>
SharedIoComputePlan::acquire_input(std::uint64_t sequence, std::uint64_t deadline_ns) noexcept {
    auto lease = arena_.grant_write(sequence);
    if (!lease)
        return std::nullopt;
    if (lease->token.slot >= pending_.size()) {
        arena_.discard(lease->token);
        return std::nullopt;
    }
    pending_[lease->token.slot] = {SubmitToken{lease->token, deadline_ns}, true};
    std::size_t active = 0;
    for (const auto& item : pending_)
        active += item.active;
    telemetry_.high_water_in_flight = std::max<std::uint64_t>(
        telemetry_.high_water_in_flight, active);
    return lease;
}

bool SharedIoComputePlan::submit(const SubmitToken& token) noexcept {
    if (token.slot.slot >= pending_.size() ||
        !pending_[token.slot.slot].active ||
        !(pending_[token.slot.slot].token.slot == token.slot))
        return false;
    if (!arena_.publish_written({token.slot})) {
        // The lease was granted by this plan but could not be published. Drop
        // it immediately so a saturated dispatcher cannot strand a slot.
        pending_[token.slot.slot].active = false;
        arena_.discard(token.slot);
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    if (!arena_.submit(token.slot)) {
        pending_[token.slot.slot].active = false;
        return false;
    }
    telemetry_.encode_submit_ns += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    return true;
}

bool SharedIoComputePlan::cancel(const SubmitToken& token) noexcept {
    if (token.slot.slot >= pending_.size() ||
        !pending_[token.slot.slot].active ||
        !(pending_[token.slot.slot].token.slot == token.slot))
        return false;
    pending_[token.slot.slot].active = false;
    return arena_.discard(token.slot);
}

void SharedIoComputePlan::on_terminal(void* context,
                                      const SharedIoSlotLedger::SlotToken& token,
                                      SharedIoArena::CompletionStatus status) noexcept {
    static_cast<SharedIoComputePlan*>(context)->record_terminal(token, status);
}

void SharedIoComputePlan::record_terminal(const SharedIoSlotLedger::SlotToken& token,
                                          SharedIoArena::CompletionStatus status) noexcept {
    if (token.slot >= pending_.size() || !pending_[token.slot].active ||
        !(pending_[token.slot].token.slot == token))
        return;
    const auto pending = pending_[token.slot].token;
    pending_[token.slot].active = false;
    const auto next = (completion_write_ + 1) % completions_.size();
    if (next == completion_read_) {
        ++telemetry_.misses;
        return;
    }
    completions_[completion_write_] = {pending, status, false};
    completion_write_ = next;
}

std::size_t SharedIoComputePlan::drain(std::uint64_t now_ns) noexcept {
    const auto started = std::chrono::steady_clock::now();
    const auto before = completion_write_;
    arena_.drain_completions({this, &SharedIoComputePlan::on_terminal});
    std::size_t count = 0;
    auto cursor = before;
    while (cursor != completion_write_) {
        auto& completion = completions_[cursor];
        completion.late = completion.token.deadline_ns != 0 &&
                          now_ns > completion.token.deadline_ns;
        if (completion.late)
            ++telemetry_.late_completions;
        ++count;
        cursor = (cursor + 1) % completions_.size();
    }
    telemetry_.completion_wall_ns += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    return count;
}

std::optional<SharedIoComputePlan::Completion> SharedIoComputePlan::pop_completion() noexcept {
    if (completion_read_ == completion_write_)
        return std::nullopt;
    auto value = completions_[completion_read_];
    completion_read_ = (completion_read_ + 1) % completions_.size();
    return value;
}

} // namespace pulp::gpu_audio::detail
