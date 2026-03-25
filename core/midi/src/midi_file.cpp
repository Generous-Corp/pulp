#include <pulp/midi/midi_file.hpp>
#include <choc/audio/choc_MIDIFile.h>
#include <fstream>

namespace pulp::midi {

double MidiFileData::duration_seconds() const {
    double max_time = 0;
    for (auto& track : tracks)
        for (auto& event : track.events)
            if (event.time_seconds > max_time) max_time = event.time_seconds;
    return max_time;
}

size_t MidiFileData::total_events() const {
    size_t total = 0;
    for (auto& track : tracks) total += track.events.size();
    return total;
}

std::optional<MidiFileData> read_midi_file(const std::string& path) {
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return std::nullopt;

        std::string content((std::istreambuf_iterator<char>(file)), {});

        choc::midi::File midi_file;
        midi_file.load(content.data(), content.size());

        MidiFileData result;
        result.ticks_per_quarter = midi_file.timeFormat > 0 ? midi_file.timeFormat : 480;

        // Use iterateEvents to get timed events
        MidiTrack single_track;
        midi_file.iterateEvents([&](const choc::midi::LongMessage& msg, double time) {
            if (msg.isShortMessage()) {
                TimedMidiEvent te;
                te.time_seconds = time;
                te.event.message = choc::midi::ShortMessage(
                    msg.data()[0],
                    msg.length() > 1 ? msg.data()[1] : uint8_t(0),
                    msg.length() > 2 ? msg.data()[2] : uint8_t(0));
                te.event.timestamp = time;
                single_track.events.push_back(te);
            }
        });

        result.tracks.push_back(std::move(single_track));
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

bool write_midi_file(const std::string& path, const MidiFileData& data) {
    try {
        // Convert our events to a choc::midi::Sequence, then create a File from it
        choc::midi::Sequence seq;
        for (auto& track : data.tracks) {
            for (auto& event : track.events) {
                auto* d = event.event.data();
                choc::midi::ShortMessage msg(d[0], d[1], d[2]);
                seq.events.push_back({event.time_seconds, msg});
            }
        }

        choc::midi::File midi_file(seq);
        midi_file.timeFormat = static_cast<int16_t>(data.ticks_per_quarter);

        auto output = midi_file.save();

        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) return false;
        file.write(reinterpret_cast<const char*>(output.data()),
                   static_cast<std::streamsize>(output.size()));
        return file.good();
    } catch (...) {
        return false;
    }
}

} // namespace pulp::midi
