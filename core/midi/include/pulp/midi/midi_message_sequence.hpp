#pragma once

// MidiMessageSequence — ordered, timestamped MIDI event list.
// Complement to MidiBuffer: MidiBuffer is for real-time process callbacks,
// MidiMessageSequence is for editing, file I/O, and offline processing.

#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <optional>

namespace pulp::midi {

/// A MIDI event with timestamp (in seconds or samples)
struct TimestampedMidiEvent {
    double timestamp = 0;          // Seconds (or samples, depending on context)
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    std::vector<uint8_t> sysex;    // For SysEx messages

    bool is_note_on() const { return (status & 0xF0) == 0x90 && data2 > 0; }
    bool is_note_off() const { return (status & 0xF0) == 0x80 || ((status & 0xF0) == 0x90 && data2 == 0); }
    bool is_cc() const { return (status & 0xF0) == 0xB0; }
    bool is_sysex() const { return status == 0xF0; }
    int channel() const { return status & 0x0F; }
    int note() const { return data1; }
    int velocity() const { return data2; }
};

/// Ordered sequence of MIDI events with editing operations
class MidiMessageSequence {
public:
    MidiMessageSequence() = default;

    /// Add an event (maintains sorted order by timestamp)
    void add_event(const TimestampedMidiEvent& event) {
        auto it = std::lower_bound(events_.begin(), events_.end(), event,
            [](const TimestampedMidiEvent& a, const TimestampedMidiEvent& b) {
                return a.timestamp < b.timestamp;
            });
        events_.insert(it, event);
    }

    /// Add a note-on event
    void add_note_on(double timestamp, int channel, int note, int velocity) {
        TimestampedMidiEvent e;
        e.timestamp = timestamp;
        e.status = static_cast<uint8_t>(0x90 | (channel & 0x0F));
        e.data1 = static_cast<uint8_t>(note & 0x7F);
        e.data2 = static_cast<uint8_t>(velocity & 0x7F);
        add_event(e);
    }

    /// Add a note-off event
    void add_note_off(double timestamp, int channel, int note) {
        TimestampedMidiEvent e;
        e.timestamp = timestamp;
        e.status = static_cast<uint8_t>(0x80 | (channel & 0x0F));
        e.data1 = static_cast<uint8_t>(note & 0x7F);
        e.data2 = 0;
        add_event(e);
    }

    /// Add a CC event
    void add_cc(double timestamp, int channel, int cc, int value) {
        TimestampedMidiEvent e;
        e.timestamp = timestamp;
        e.status = static_cast<uint8_t>(0xB0 | (channel & 0x0F));
        e.data1 = static_cast<uint8_t>(cc & 0x7F);
        e.data2 = static_cast<uint8_t>(value & 0x7F);
        add_event(e);
    }

    /// Number of events
    int size() const { return static_cast<int>(events_.size()); }

    /// Access event by index
    const TimestampedMidiEvent& operator[](int i) const { return events_[static_cast<size_t>(i)]; }

    /// Clear all events
    void clear() { events_.clear(); }

    /// Duration (timestamp of last event)
    double duration() const { return events_.empty() ? 0 : events_.back().timestamp; }

    /// Find events in a time range [start, end)
    std::vector<const TimestampedMidiEvent*> events_in_range(double start, double end) const {
        std::vector<const TimestampedMidiEvent*> result;
        for (auto& e : events_)
            if (e.timestamp >= start && e.timestamp < end)
                result.push_back(&e);
        return result;
    }

    /// Find matching note-off for a note-on event
    std::optional<int> find_note_off(int note_on_index) const {
        if (note_on_index < 0 || note_on_index >= size()) return std::nullopt;
        auto& on = events_[static_cast<size_t>(note_on_index)];
        if (!on.is_note_on()) return std::nullopt;

        for (int i = note_on_index + 1; i < size(); ++i) {
            auto& e = events_[static_cast<size_t>(i)];
            if (e.is_note_off() && e.note() == on.note() && e.channel() == on.channel())
                return i;
        }
        return std::nullopt;
    }

    /// Shift all timestamps by an offset
    void offset_timestamps(double offset) {
        for (auto& e : events_)
            e.timestamp += offset;
    }

    /// Get all events (const)
    const std::vector<TimestampedMidiEvent>& events() const { return events_; }

    /// Iterator support
    auto begin() const { return events_.begin(); }
    auto end() const { return events_.end(); }

private:
    std::vector<TimestampedMidiEvent> events_;
};

}  // namespace pulp::midi
