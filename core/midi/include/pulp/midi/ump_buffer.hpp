#pragma once

/// @file ump_buffer.hpp
/// Sample-accurate MIDI 2.0 UMP packet stream.
///
/// `UmpBuffer` parallels `MidiBuffer`: while `MidiBuffer` carries MIDI 1.0
/// short messages, `UmpBuffer` carries native Universal MIDI Packets with
/// full MIDI 2.0 resolution (16-bit velocity, per-note pitch bend,
/// per-note CCs). Processors that set `PluginDescriptor::supports_ump`
/// access the buffer via `Processor::ump_input()` during `process()`.
///
/// Packets carry a `sample_offset` so hosts can deliver sample-accurate
/// timing (same semantics as `MidiEvent::sample_offset`).

#include <pulp/midi/ump.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace pulp::midi {

/// One UMP packet with a block-relative sample offset.
struct UmpEvent {
    UmpPacket packet;
    int32_t sample_offset = 0;
};

/// A time-ordered list of UMP events for one audio block.
///
/// Ownership and lifetime mirror `MidiBuffer`/`MpeBuffer`: the format
/// adapter owns the buffer, fills it before `process()`, passes a
/// pointer to the processor, and clears it afterwards. Not thread-safe.
///
/// Realtime contract: reserve() storage and enable
/// set_realtime_capacity_limit(true) before using add() on the audio thread.
/// With the limit enabled, add() drops and counts overflow instead of growing
/// the backing vector. Without that preparation, append operations may
/// allocate and belong on a non-realtime path.
class UmpBuffer {
public:
    UmpBuffer() { reserve(kInitialCapacity); }
    UmpBuffer(const UmpBuffer& other)
        : events_(other.events_),
          limit_to_reserved_capacity_(other.limit_to_reserved_capacity_),
          dropped_events_(other.dropped_events_) {
        reserve(other.capacity());
    }
    UmpBuffer& operator=(const UmpBuffer& other) {
        if (this != &other) {
            events_ = other.events_;
            limit_to_reserved_capacity_ = other.limit_to_reserved_capacity_;
            dropped_events_ = other.dropped_events_;
            reserve(other.capacity());
        }
        return *this;
    }
    UmpBuffer(UmpBuffer&&) noexcept = default;
    UmpBuffer& operator=(UmpBuffer&&) noexcept = default;

    bool add(const UmpEvent& e) {
        if (!can_append()) {
            record_drop();
            return false;
        }
        events_.push_back(e);
        prepare_sort_after_growth();
        return true;
    }
    bool add(UmpEvent&& e) {
        if (!can_append()) {
            record_drop();
            return false;
        }
        events_.push_back(std::move(e));
        prepare_sort_after_growth();
        return true;
    }
    bool add(const UmpPacket& p, int32_t sample_offset = 0) {
        if (!can_append()) {
            record_drop();
            return false;
        }
        events_.push_back({p, sample_offset});
        prepare_sort_after_growth();
        return true;
    }

    void clear() {
        events_.clear();
        dropped_events_ = 0;
    }
    bool empty() const { return events_.empty(); }
    std::size_t size() const { return events_.size(); }
    std::size_t capacity() const { return events_.capacity(); }
    std::uint32_t dropped_event_count() const { return dropped_events_; }
    void reserve(std::size_t capacity) {
        events_.reserve(capacity);
        sort_indices_.reserve(events_.capacity());
        sort_events_.reserve(events_.capacity());
    }
    void set_realtime_capacity_limit(bool enabled = true) {
        limit_to_reserved_capacity_ = enabled;
    }
    bool realtime_capacity_limited() const { return limit_to_reserved_capacity_; }

    /// Sort by sample offset, preserving arrival order for equal offsets.
    /// Scratch storage is prepared by reserve(); sorting a prepared buffer
    /// does not allocate. As with add(), unreserved growth is non-realtime.
    void sort() {
        const auto count = events_.size();
        if (count < 2) return;
        sort_indices_.resize(count);
        for (std::size_t i = 0; i < count; ++i) sort_indices_[i] = i;
        std::sort(sort_indices_.begin(), sort_indices_.end(),
            [this](std::size_t a, std::size_t b) {
                if (events_[a].sample_offset != events_[b].sample_offset)
                    return events_[a].sample_offset < events_[b].sample_offset;
                return a < b;
            });
        sort_events_.resize(count);
        for (std::size_t i = 0; i < count; ++i)
            sort_events_[i] = events_[sort_indices_[i]];
        events_.swap(sort_events_);
    }

    auto begin()       { return events_.begin(); }
    auto end()         { return events_.end(); }
    auto begin() const { return events_.begin(); }
    auto end() const   { return events_.end(); }

    const UmpEvent& operator[](std::size_t i) const { return events_[i]; }

private:
    void prepare_sort_after_growth() {
        if (!limit_to_reserved_capacity_) {
            sort_indices_.reserve(events_.capacity());
            sort_events_.reserve(events_.capacity());
        }
    }
    bool can_append() const {
        return !limit_to_reserved_capacity_ || events_.size() < events_.capacity();
    }
    void record_drop() {
        if (dropped_events_ < std::numeric_limits<std::uint32_t>::max()) {
            ++dropped_events_;
        }
    }

    static constexpr std::size_t kInitialCapacity = 128;
    std::vector<UmpEvent> events_;
    std::vector<std::size_t> sort_indices_;
    std::vector<UmpEvent> sort_events_;
    bool limit_to_reserved_capacity_ = false;
    std::uint32_t dropped_events_ = 0;
};

} // namespace pulp::midi
