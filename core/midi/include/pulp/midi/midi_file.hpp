#pragma once

#include <pulp/midi/message.hpp>
#include <string>
#include <vector>
#include <optional>

namespace pulp::midi {

// A MIDI event with absolute time position
struct TimedMidiEvent {
    double time_seconds = 0;
    MidiEvent event;
};

// A sequence of timed MIDI events (one track)
struct MidiTrack {
    std::string name;
    std::vector<TimedMidiEvent> events;
};

// A complete MIDI file
struct MidiFileData {
    double tempo_bpm = 120.0;
    int ticks_per_quarter = 480;
    std::vector<MidiTrack> tracks;

    double duration_seconds() const;
    size_t total_events() const;
};

// Read a standard MIDI file (.mid)
std::optional<MidiFileData> read_midi_file(const std::string& path);

// Write a MIDI file
bool write_midi_file(const std::string& path, const MidiFileData& data);

} // namespace pulp::midi
