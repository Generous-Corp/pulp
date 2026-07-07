#include <pulp/midi/tuning.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::midi {

bool is_valid_midi_note(int midi_note) noexcept {
    return midi_note >= 0 && midi_note <= 127;
}

bool is_valid_midi_channel(int midi_channel) noexcept {
    return midi_channel >= 0 && midi_channel <= 15;
}

int normalise_midi_channel(int midi_channel) noexcept {
    return is_valid_midi_channel(midi_channel) ? midi_channel : kUnknownMidiChannel;
}

double equal_temperament_frequency(int midi_note, double a4_hz) noexcept {
    if (!is_valid_midi_note(midi_note) || !(a4_hz > 0.0) || !std::isfinite(a4_hz))
        return 0.0;
    return a4_hz * std::pow(2.0, (static_cast<double>(midi_note) - 69.0) / 12.0);
}

int equal_temperament_note_for_frequency(double frequency_hz, double a4_hz) noexcept {
    if (!(frequency_hz > 0.0) || !(a4_hz > 0.0) || !std::isfinite(frequency_hz) ||
        !std::isfinite(a4_hz))
        return -1;

    const double semitones = 69.0 + 12.0 * std::log2(frequency_hz / a4_hz);
    const int note = static_cast<int>(std::lround(semitones));
    return std::clamp(note, 0, 127);
}

TuningNoteResult TuningProvider::frequency_to_note(
    double frequency_hz,
    int preferred_midi_channel) const {
    const int note = equal_temperament_note_for_frequency(frequency_hz);
    return {
        .valid = is_valid_midi_note(note),
        .midi_note = note,
        .midi_channel = is_valid_midi_note(note)
            ? normalise_midi_channel(preferred_midi_channel)
            : kUnknownMidiChannel,
    };
}

TuningNoteResult TuningProvider::frequency_to_note_and_channel(double frequency_hz) const {
    const int note = equal_temperament_note_for_frequency(frequency_hz);
    return {
        .valid = is_valid_midi_note(note),
        .midi_note = note,
        .midi_channel = is_valid_midi_note(note) ? 0 : kUnknownMidiChannel,
    };
}

TuningStatus TuningProvider::status() const {
    return {};
}

void TuningProvider::parse_midi_data(std::span<const std::uint8_t>) {}

EqualTemperamentTuningProvider::EqualTemperamentTuningProvider(double a4_hz) noexcept
    : a4_hz_((a4_hz > 0.0 && std::isfinite(a4_hz)) ? a4_hz : 440.0) {}

TuningQueryResult EqualTemperamentTuningProvider::note_to_frequency(
    int midi_note,
    int) const {
    if (!is_valid_midi_note(midi_note))
        return {};

    return {
        .valid = true,
        .frequency_hz = equal_temperament_frequency(midi_note, a4_hz_),
        .retuning_semitones = 0.0,
        .retuning_ratio = 1.0,
        .should_filter_note = false,
    };
}

TuningNoteResult EqualTemperamentTuningProvider::frequency_to_note(
    double frequency_hz,
    int preferred_midi_channel) const {
    const int note = equal_temperament_note_for_frequency(frequency_hz, a4_hz_);
    return {
        .valid = is_valid_midi_note(note),
        .midi_note = note,
        .midi_channel = is_valid_midi_note(note)
            ? normalise_midi_channel(preferred_midi_channel)
            : kUnknownMidiChannel,
    };
}

TuningNoteResult EqualTemperamentTuningProvider::frequency_to_note_and_channel(
    double frequency_hz) const {
    const int note = equal_temperament_note_for_frequency(frequency_hz, a4_hz_);
    return {
        .valid = is_valid_midi_note(note),
        .midi_note = note,
        .midi_channel = is_valid_midi_note(note) ? 0 : kUnknownMidiChannel,
    };
}

TuningStatus EqualTemperamentTuningProvider::status() const {
    TuningStatus result;
    result.scale_name = "12-TET";
    return result;
}

}  // namespace pulp::midi
