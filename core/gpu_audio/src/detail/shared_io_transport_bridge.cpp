#include "shared_io_transport_bridge.hpp"

namespace pulp::gpu_audio::detail {

bool SharedIoTransportBridge::prepare(std::uint32_t capacity, std::uint64_t preparation_epoch,
                                      std::uint64_t first_sequence) {
    if (capacity == 0 || preparation_epoch == 0)
        return false;
    entries_.assign(capacity, {});
    preparation_epoch_ = preparation_epoch;
    next_sequence_ = first_sequence;
    reprime_required_ = false;
    return true;
}

bool SharedIoTransportBridge::record(const SharedIoComputePlan::Completion& completion) noexcept {
    if (entries_.empty() || reprime_required_ ||
        completion.token.slot.preparation_epoch != preparation_epoch_ ||
        completion.token.slot.stream_sequence < next_sequence_ ||
        completion.token.slot.stream_sequence >= next_sequence_ + entries_.size())
        return false;
    auto& entry = entries_[completion.token.slot.stream_sequence % entries_.size()];
    if (entry.present)
        return false;
    entry.result = {completion.token.slot.stream_sequence, completion.status, completion.late,
                    completion.status == SharedIoArena::CompletionStatus::RetiredSuccess
                        ? (completion.late ? Disposition::SuppressLate : Disposition::Deliver)
                        : Disposition::Reprime};
    entry.present = true;
    return true;
}

std::optional<SharedIoTransportBridge::Result> SharedIoTransportBridge::collect_next() noexcept {
    if (entries_.empty())
        return std::nullopt;
    auto& entry = entries_[next_sequence_ % entries_.size()];
    if (!entry.present)
        return std::nullopt;
    auto result = entry.result;
    entry = {};
    ++next_sequence_;
    if (result.disposition == Disposition::Reprime) {
        // A terminal failure invalidates stateful GPU history. Successors may
        // be physically retired but cannot become audio delivery candidates.
        for (auto& pending : entries_)
            pending = {};
        reprime_required_ = true;
    }
    return result;
}

void SharedIoTransportBridge::reset(std::uint64_t preparation_epoch,
                                    std::uint64_t first_sequence) noexcept {
    for (auto& entry : entries_)
        entry = {};
    preparation_epoch_ = preparation_epoch;
    next_sequence_ = first_sequence;
    reprime_required_ = false;
}

std::size_t SharedIoTransportBridge::pending() const noexcept {
    std::size_t count = 0;
    for (const auto& entry : entries_)
        count += entry.present;
    return count;
}

} // namespace pulp::gpu_audio::detail
