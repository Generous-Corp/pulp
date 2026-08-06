#pragma once

/// @file mpe_buffer.hpp
/// Sample-accurate MPE expression event stream.
///
/// `MpeBuffer` is a parallel sidecar to `MidiBuffer`: while `MidiBuffer`
/// carries raw MIDI 1.0 events, `MpeBuffer` carries higher-level per-note
/// expression deltas emitted by an `MpeVoiceTracker`. Plugins that opt into
/// MPE via `PluginDescriptor::supports_mpe` access the buffer through
/// `Processor::mpe_input()` during `process()`.
///
/// A format adapter typically fills this by running the inbound
/// `MidiBuffer` through an `MpeVoiceTracker` whose callbacks append to the
/// buffer with the source event's `sample_offset` attached.

#include <pulp/midi/mpe_voice_tracker.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace pulp::midi {

/// A single per-note expression event.
///
/// `state` is a snapshot of the note's full MPE state at the time the
/// event was produced, so a plugin can read any field without tracking
/// deltas itself.
struct MpeExpressionEvent {
    enum class Kind : uint8_t {
        NoteOn,
        NoteOff,
        PitchBend,
        Pressure,
        Timbre,
    };

    int32_t sample_offset = 0;  ///< Sample position within the current block
    Kind kind = Kind::NoteOn;
    MpeNoteState state;         ///< Snapshot at event time
};

/// A time-ordered list of per-note expression events for one block.
///
/// Ownership and lifetime mirror `MidiBuffer`: the format adapter owns the
/// buffer, fills it before `process()`, passes a pointer to the processor,
/// and clears it afterwards. Not thread-safe; all access is assumed to
/// happen on the audio thread in one producer/consumer pair.
class MpeBuffer {
public:
    MpeBuffer() { reserve(kInitialCapacity); }

    bool add(const MpeExpressionEvent& e) {
        if (!can_append()) {
            record_drop();
            return false;
        }
        events_.push_back(e);
        return true;
    }
    bool add(MpeExpressionEvent&& e) {
        if (!can_append()) {
            record_drop();
            return false;
        }
        events_.push_back(std::move(e));
        return true;
    }

    /// Append a group atomically. Under the realtime capacity limit, either
    /// every event is appended in source order or none are. Rejected events are
    /// all reflected in the saturating drop counter.
    bool add_batch(std::span<const MpeExpressionEvent> events) {
        if (!can_append(events.size())) {
            record_drops(events.size());
            return false;
        }
        events_.insert(events_.end(), events.begin(), events.end());
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
        sort_index_.reserve(capacity);
        sort_reorder_.reserve(capacity);
    }
    void set_realtime_capacity_limit(bool enabled = true) {
        limit_to_reserved_capacity_ = enabled;
    }

    void sort() {
        // Match MidiBuffer's allocation-free insertion-stable index sort.
        // Producer order at equal offsets is semantic for retriggers:
        // retirement must remain immediately before the replacement note-on.
        const std::size_t count = events_.size();
        if (count < 2) return;
        sort_index_.resize(count);
        for (std::size_t i = 0; i < count; ++i) sort_index_[i] = i;
        const auto& source = events_;
        std::sort(sort_index_.begin(), sort_index_.end(),
            [&source](std::size_t a, std::size_t b) {
                if (source[a].sample_offset != source[b].sample_offset) {
                    return source[a].sample_offset < source[b].sample_offset;
                }
                return a < b;
            });
        sort_reorder_.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            sort_reorder_[i] = std::move(events_[sort_index_[i]]);
        }
        events_.swap(sort_reorder_);
    }

    auto begin()       { return events_.begin(); }
    auto end()         { return events_.end(); }
    auto begin() const { return events_.begin(); }
    auto end() const   { return events_.end(); }

    const MpeExpressionEvent& operator[](std::size_t i) const { return events_[i]; }

private:
    bool can_append(std::size_t count = 1) const {
        return !limit_to_reserved_capacity_
            || count <= events_.capacity() - events_.size();
    }
    void record_drop() {
        record_drops(1);
    }
    void record_drops(std::size_t count) {
        const auto max = std::numeric_limits<std::uint32_t>::max();
        const auto available = static_cast<std::uint64_t>(max - dropped_events_);
        const auto increment = std::min<std::uint64_t>(count, available);
        dropped_events_ += static_cast<std::uint32_t>(increment);
    }

    static constexpr std::size_t kInitialCapacity = 128;
    std::vector<MpeExpressionEvent> events_;
    std::vector<std::size_t> sort_index_;
    std::vector<MpeExpressionEvent> sort_reorder_;
    bool limit_to_reserved_capacity_ = false;
    std::uint32_t dropped_events_ = 0;
};

/// Convenience: install tracker callbacks that forward events to `out` with
/// the given sample offset. Call once on the host thread during setup.
inline void bind_tracker_to_buffer(MpeVoiceTracker& tracker,
                                   MpeBuffer& out,
                                   int32_t& current_sample_offset) {
    using K = MpeExpressionEvent::Kind;
    tracker.on_note_lifecycle = [&out, &current_sample_offset](
        const MpeNoteState* note_off, const MpeNoteState* note_on) {
        std::array<MpeExpressionEvent, 2> events{};
        std::size_t count = 0;
        if (note_off) {
            events[count++] = {current_sample_offset, K::NoteOff, *note_off};
        }
        if (note_on) {
            events[count++] = {current_sample_offset, K::NoteOn, *note_on};
        }
        return out.add_batch(std::span<const MpeExpressionEvent>{events.data(), count});
    };
    tracker.on_pitch_bend = [&out, &current_sample_offset](const MpeNoteState& s) {
        out.add({current_sample_offset, K::PitchBend, s});
    };
    tracker.on_pressure = [&out, &current_sample_offset](const MpeNoteState& s) {
        out.add({current_sample_offset, K::Pressure, s});
    };
    tracker.on_timbre = [&out, &current_sample_offset](const MpeNoteState& s) {
        out.add({current_sample_offset, K::Timbre, s});
    };
}

} // namespace pulp::midi
