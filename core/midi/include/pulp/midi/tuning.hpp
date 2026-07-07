#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace pulp::midi {

inline constexpr int kUnknownMidiChannel = -1;

bool is_valid_midi_note(int midi_note) noexcept;
bool is_valid_midi_channel(int midi_channel) noexcept;
int normalise_midi_channel(int midi_channel) noexcept;

double equal_temperament_frequency(int midi_note, double a4_hz = 440.0) noexcept;
int equal_temperament_note_for_frequency(double frequency_hz, double a4_hz = 440.0) noexcept;

struct TuningQueryResult {
    bool valid = false;
    double frequency_hz = 0.0;
    double retuning_semitones = 0.0;
    double retuning_ratio = 1.0;
    bool should_filter_note = false;
};

struct TuningNoteResult {
    bool valid = false;
    int midi_note = -1;
    int midi_channel = kUnknownMidiChannel;
};

struct TuningStatus {
    bool has_external_master = false;
    bool has_local_mts_sysex = false;
    bool has_local_file_tuning = false;
    bool has_keyboard_mapping = false;
    bool library_update_recommended = false;
    std::string scale_name;
    double period_ratio = 2.0;
    double period_semitones = 12.0;
    int map_size = -1;
    int map_start_key = -1;
    int reference_key = -1;
};

class TuningProvider {
public:
    virtual ~TuningProvider() = default;

    virtual TuningQueryResult note_to_frequency(
        int midi_note,
        int midi_channel = kUnknownMidiChannel) const = 0;

    virtual TuningNoteResult frequency_to_note(
        double frequency_hz,
        int preferred_midi_channel = kUnknownMidiChannel) const;

    virtual TuningNoteResult frequency_to_note_and_channel(double frequency_hz) const;

    virtual TuningStatus status() const;
    virtual void parse_midi_data(std::span<const std::uint8_t> data);
};

class EqualTemperamentTuningProvider final : public TuningProvider {
public:
    explicit EqualTemperamentTuningProvider(double a4_hz = 440.0) noexcept;

    TuningQueryResult note_to_frequency(
        int midi_note,
        int midi_channel = kUnknownMidiChannel) const override;

    TuningNoteResult frequency_to_note(
        double frequency_hz,
        int preferred_midi_channel = kUnknownMidiChannel) const override;

    TuningNoteResult frequency_to_note_and_channel(double frequency_hz) const override;
    TuningStatus status() const override;

private:
    double a4_hz_ = 440.0;
};

}  // namespace pulp::midi
