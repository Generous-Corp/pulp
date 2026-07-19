#include <pulp/audio/sample_key_map.hpp>

#include <cmath>

namespace pulp::audio {

namespace {

bool note_in_midi_range(int note) noexcept {
    return note >= 0 && note <= 127;
}

bool finite_positive(double value) noexcept {
    return value > 0.0 && std::isfinite(value);
}

}  // namespace

bool SampleKeyMap::config_valid(const SampleKeyMapConfig& config) noexcept {
    if (!note_in_midi_range(config.root_note) ||
        !note_in_midi_range(config.lowest_note) ||
        !note_in_midi_range(config.highest_note) ||
        !note_in_midi_range(config.first_slice_note)) {
        return false;
    }
    if (config.lowest_note > config.highest_note) return false;
    return std::isfinite(config.keytrack_cents_per_key);
}

bool SampleKeyMap::configure(const SampleKeyMapConfig& config) noexcept {
    if (!config_valid(config)) return false;
    config_ = config;
    return true;
}

bool SampleKeyMap::accepts_note(int note) const noexcept {
    return config_valid(config_) &&
           note >= config_.lowest_note &&
           note <= config_.highest_note;
}

double SampleKeyMap::semitone_offset_for_note(int note,
                                              double tune_semitones) const noexcept {
    if (!accepts_note(note) || !std::isfinite(tune_semitones)) return 0.0;
    const auto key_delta = static_cast<double>(note - config_.root_note);
    return tune_semitones + key_delta * (config_.keytrack_cents_per_key / 100.0);
}

double SampleKeyMap::pitch_ratio_for_note(int note,
                                          double tune_semitones) const noexcept {
    if (!accepts_note(note) || !std::isfinite(tune_semitones)) return 0.0;
    return std::pow(2.0, semitone_offset_for_note(note, tune_semitones) / 12.0);
}

double SampleKeyMap::playback_rate_for_note(int note,
                                            double source_sample_rate,
                                            double host_sample_rate,
                                            double tune_semitones) const noexcept {
    if (!finite_positive(source_sample_rate) || !finite_positive(host_sample_rate)) {
        return 0.0;
    }
    const auto ratio = pitch_ratio_for_note(note, tune_semitones);
    if (ratio == 0.0) return 0.0;
    return ratio * (source_sample_rate / host_sample_rate);
}

SampleKeySliceResolution SampleKeyMap::resolve_slice_for_note(
    int note,
    const SliceMap& map) const noexcept {
    SampleKeySliceResolution result;
    if (!validate_slice_map(map)) return result;
    const auto slice_index = slice_index_for_note(note, map.regions.size());
    if (!slice_index) return result;

    result.valid = true;
    result.slice_index = *slice_index;
    result.region = map.regions[*slice_index];
    return result;
}

std::optional<std::uint32_t> SampleKeyMap::slice_index_for_note(
    int note,
    std::size_t slice_count) const noexcept {
    if (!accepts_note(note)) return std::nullopt;
    const auto slice_offset = note - config_.first_slice_note;
    if (slice_offset < 0) return std::nullopt;
    const auto slice_index = static_cast<std::uint32_t>(slice_offset);
    if (slice_index >= slice_count) return std::nullopt;
    return slice_index;
}

}  // namespace pulp::audio
