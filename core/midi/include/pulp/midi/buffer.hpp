#pragma once

#include <pulp/midi/message.hpp>
#include <vector>
#include <algorithm>

namespace pulp::midi {

/// Collection of timestamped MIDI events within a single audio buffer period.
///
/// Events are appended via add() and should be sorted by sample_offset
/// (call sort()) before iterating in the audio callback. Supports
/// range-based for loops.
///
/// @code
/// MidiBuffer buf;
/// buf.add(MidiEvent::note_on(0, 60, 100));
/// buf.sort();
/// for (const auto& ev : buf) { /* process */ }
/// @endcode
class MidiBuffer {
public:
    MidiBuffer() = default;

    /// Append a MIDI event to the buffer.
    void add(const MidiEvent& event) {
        events_.push_back(event);
    }

    /// @copydoc add(const MidiEvent&)
    void add(MidiEvent&& event) {
        events_.push_back(std::move(event));
    }

    /// Remove all events.
    void clear() { events_.clear(); }
    bool empty() const { return events_.empty(); }
    std::size_t size() const { return events_.size(); }

    /// Sort events by sample_offset for sample-accurate processing.
    /// Call this before iterating in the audio callback.
    void sort() {
        std::sort(events_.begin(), events_.end(),
            [](const MidiEvent& a, const MidiEvent& b) {
                return a.sample_offset < b.sample_offset;
            });
    }

    auto begin() { return events_.begin(); }
    auto end() { return events_.end(); }
    auto begin() const { return events_.begin(); }
    auto end() const { return events_.end(); }

    const MidiEvent& operator[](std::size_t index) const { return events_[index]; }

private:
    std::vector<MidiEvent> events_;
};

} // namespace pulp::midi
