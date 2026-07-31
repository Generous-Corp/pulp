#include "midi_latency_queue.hpp"

#include <algorithm>
#include <limits>

namespace pulp::sequence::detail {

bool MidiLatencyQueue::prepare(std::size_t aggregate_event_capacity, std::uint32_t latency_samples,
                               std::size_t maximum_delayed_events) {
    if (aggregate_event_capacity == 0 || maximum_delayed_events == 0 ||
        static_cast<std::uint64_t>(aggregate_event_capacity) >
            std::numeric_limits<std::size_t>::max() /
                (static_cast<std::uint64_t>(latency_samples) + 1u))
        return false;
    const auto worst_partition_capacity =
        aggregate_event_capacity * (static_cast<std::size_t>(latency_samples) + 1u);
    const auto capacity = std::min(worst_partition_capacity, maximum_delayed_events);
#if defined(__cpp_exceptions)
    try {
#endif
        midi_.assign(capacity, {});
        ump_.assign(capacity, {});
#if defined(__cpp_exceptions)
    } catch (...) {
        reset();
        return false;
    }
#endif
    latency_samples_ = latency_samples;
    clear_pending();
    output_frontier_ = 0;
    return true;
}

void MidiLatencyQueue::reset() noexcept {
    midi_.clear();
    ump_.clear();
    midi_read_ = 0;
    midi_write_ = 0;
    midi_count_ = 0;
    ump_read_ = 0;
    ump_write_ = 0;
    ump_count_ = 0;
    output_frontier_ = 0;
    latency_samples_ = 0;
}

void MidiLatencyQueue::clear_pending() noexcept {
    midi_read_ = 0;
    midi_write_ = 0;
    midi_count_ = 0;
    ump_read_ = 0;
    ump_write_ = 0;
    ump_count_ = 0;
}

bool MidiLatencyQueue::process(const midi::MidiBuffer& source, midi::MidiBuffer& destination,
                               std::uint32_t frames) noexcept {
    if (midi_.empty() || ump_.empty() ||
        output_frontier_ > std::numeric_limits<std::uint64_t>::max() - frames ||
        source.dropped_event_count() != 0 || source.dropped_sysex_count() != 0 ||
        !source.sysex().empty() || source.size() > midi_.size() - midi_count_)
        return false;
    const auto block_end = output_frontier_ + frames;
    const auto* source_ump = source.ump();
    if (source_ump != nullptr &&
        (source_ump->dropped_event_count() != 0 || source_ump->size() > ump_.size() - ump_count_))
        return false;

    std::size_t midi_due_count = 0;
    for (std::size_t offset = 0; offset < midi_count_; ++offset) {
        const auto index = (midi_read_ + offset) % midi_.size();
        if (midi_[index].due_sample >= block_end)
            break;
        ++midi_due_count;
    }
    for (const auto& event : source) {
        if (event.sample_offset < 0 || static_cast<std::uint32_t>(event.sample_offset) >= frames ||
            output_frontier_ >
                std::numeric_limits<std::uint64_t>::max() -
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(event.sample_offset)) +
                     latency_samples_))
            return false;
        const auto due =
            output_frontier_ + static_cast<std::uint32_t>(event.sample_offset) + latency_samples_;
        if (due < block_end)
            ++midi_due_count;
    }
    if (midi_due_count > destination.event_capacity() - destination.size())
        return false;

    auto* destination_ump = destination.ump();
    if (destination_ump == nullptr &&
        (ump_count_ != 0 || (source_ump != nullptr && !source_ump->empty())))
        return false;

    std::size_t ump_due_count = 0;
    for (std::size_t offset = 0; offset < ump_count_; ++offset) {
        const auto index = (ump_read_ + offset) % ump_.size();
        if (ump_[index].due_sample >= block_end)
            break;
        ++ump_due_count;
    }
    if (source_ump != nullptr) {
        for (const auto& event : *source_ump) {
            if (event.sample_offset < 0 ||
                static_cast<std::uint32_t>(event.sample_offset) >= frames ||
                output_frontier_ > std::numeric_limits<std::uint64_t>::max() -
                                       (static_cast<std::uint64_t>(
                                            static_cast<std::uint32_t>(event.sample_offset)) +
                                        latency_samples_))
                return false;
            const auto due = output_frontier_ + static_cast<std::uint32_t>(event.sample_offset) +
                             latency_samples_;
            if (due < block_end)
                ++ump_due_count;
        }
    }
    if (destination_ump != nullptr &&
        ump_due_count > destination_ump->capacity() - destination_ump->size())
        return false;

    for (const auto& event : source) {
        const auto due =
            output_frontier_ + static_cast<std::uint32_t>(event.sample_offset) + latency_samples_;
        midi_[midi_write_] = {event, due};
        midi_write_ = (midi_write_ + 1u) % midi_.size();
        ++midi_count_;
    }
    if (source_ump != nullptr) {
        for (const auto& event : *source_ump) {
            const auto due = output_frontier_ + static_cast<std::uint32_t>(event.sample_offset) +
                             latency_samples_;
            ump_[ump_write_] = {event, due};
            ump_write_ = (ump_write_ + 1u) % ump_.size();
            ++ump_count_;
        }
    }

    while (midi_count_ != 0) {
        auto& queued = midi_[midi_read_];
        if (queued.due_sample >= block_end)
            break;
        if (queued.due_sample < output_frontier_)
            return false;
        queued.event.sample_offset =
            static_cast<std::int32_t>(queued.due_sample - output_frontier_);
        if (!destination.add(queued.event))
            return false;
        midi_read_ = (midi_read_ + 1u) % midi_.size();
        --midi_count_;
    }
    while (ump_count_ != 0) {
        auto& queued = ump_[ump_read_];
        if (queued.due_sample >= block_end)
            break;
        if (queued.due_sample < output_frontier_)
            return false;
        queued.event.sample_offset =
            static_cast<std::int32_t>(queued.due_sample - output_frontier_);
        if (destination_ump != nullptr && !destination_ump->add(queued.event))
            return false;
        ump_read_ = (ump_read_ + 1u) % ump_.size();
        --ump_count_;
    }
    output_frontier_ = block_end;
    return true;
}

} // namespace pulp::sequence::detail
