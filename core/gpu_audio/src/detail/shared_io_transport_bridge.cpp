#include "shared_io_transport_bridge.hpp"

namespace pulp::gpu_audio::detail {

bool SharedIoTransportBridge::prepare(std::uint32_t capacity,
                                      std::uint64_t first_sequence) {
    if (capacity == 0)
        return false;
    entries_.assign(capacity, {});
    next_sequence_ = first_sequence;
    return true;
}

bool SharedIoTransportBridge::record(const SharedIoComputePlan::Completion& completion) noexcept {
    if (entries_.empty() || completion.token.slot.stream_sequence < next_sequence_ ||
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
    return result;
}

void SharedIoTransportBridge::reset(std::uint64_t first_sequence) noexcept {
    for (auto& entry : entries_)
        entry = {};
    next_sequence_ = first_sequence;
}

std::size_t SharedIoTransportBridge::pending() const noexcept {
    std::size_t count = 0;
    for (const auto& entry : entries_)
        count += entry.present;
    return count;
}

} // namespace pulp::gpu_audio::detail
