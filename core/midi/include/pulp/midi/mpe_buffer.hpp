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
#include <cstdint>
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
    MpeBuffer() { events_.reserve(kInitialCapacity); }

    void add(const MpeExpressionEvent& e) { events_.push_back(e); }
    void add(MpeExpressionEvent&& e) { events_.push_back(std::move(e)); }

    void clear() { events_.clear(); }
    bool empty() const { return events_.empty(); }
    std::size_t size() const { return events_.size(); }

    void sort() {
        std::sort(events_.begin(), events_.end(),
            [](const MpeExpressionEvent& a, const MpeExpressionEvent& b) {
                return a.sample_offset < b.sample_offset;
            });
    }

    auto begin()       { return events_.begin(); }
    auto end()         { return events_.end(); }
    auto begin() const { return events_.begin(); }
    auto end() const   { return events_.end(); }

    const MpeExpressionEvent& operator[](std::size_t i) const { return events_[i]; }

private:
    static constexpr std::size_t kInitialCapacity = 128;
    std::vector<MpeExpressionEvent> events_;
};

/// Convenience: install tracker callbacks that forward events to `out` with
/// the given sample offset. Call once on the host thread during setup.
inline void bind_tracker_to_buffer(MpeVoiceTracker& tracker,
                                   MpeBuffer& out,
                                   int32_t& current_sample_offset) {
    using K = MpeExpressionEvent::Kind;
    tracker.on_note_on = [&out, &current_sample_offset](const MpeNoteState& s) {
        out.add({current_sample_offset, K::NoteOn, s});
    };
    tracker.on_note_off = [&out, &current_sample_offset](const MpeNoteState& s) {
        out.add({current_sample_offset, K::NoteOff, s});
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
