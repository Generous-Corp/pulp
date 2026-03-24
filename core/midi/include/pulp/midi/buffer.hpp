#pragma once

#include <pulp/midi/message.hpp>
#include <vector>
#include <algorithm>

namespace pulp::midi {

// Collection of timestamped MIDI events within a buffer period
// Sorted by sample_offset for audio-thread iteration
class MidiBuffer {
public:
    MidiBuffer() = default;

    void add(const MidiEvent& event) {
        events_.push_back(event);
    }

    void add(MidiEvent&& event) {
        events_.push_back(std::move(event));
    }

    void clear() { events_.clear(); }
    bool empty() const { return events_.empty(); }
    std::size_t size() const { return events_.size(); }

    // Sort by sample offset (call before iterating in audio callback)
    void sort() {
        std::sort(events_.begin(), events_.end(),
            [](const MidiEvent& a, const MidiEvent& b) {
                return a.sample_offset < b.sample_offset;
            });
    }

    // Iterators
    auto begin() { return events_.begin(); }
    auto end() { return events_.end(); }
    auto begin() const { return events_.begin(); }
    auto end() const { return events_.end(); }

    const MidiEvent& operator[](std::size_t index) const { return events_[index]; }

private:
    std::vector<MidiEvent> events_;
};

} // namespace pulp::midi
