#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <pulp/audio/slice_map.hpp>

namespace pulp::audio {

struct SampleKeyMapConfig {
    int root_note = 60;
    int lowest_note = 0;
    int highest_note = 127;
    double keytrack_cents_per_key = 100.0;
    int first_slice_note = 60;
};

struct SampleKeySliceResolution {
    bool valid = false;
    std::uint32_t slice_index = 0;
    SliceRegion region{};
};

class SampleKeyMap {
public:
    static bool config_valid(const SampleKeyMapConfig& config) noexcept;

    bool configure(const SampleKeyMapConfig& config) noexcept;
    const SampleKeyMapConfig& config() const noexcept { return config_; }

    bool accepts_note(int note) const noexcept;
    double semitone_offset_for_note(int note,
                                    double tune_semitones = 0.0) const noexcept;
    double pitch_ratio_for_note(int note,
                                double tune_semitones = 0.0) const noexcept;
    double playback_rate_for_note(int note,
                                  double source_sample_rate,
                                  double host_sample_rate,
                                  double tune_semitones = 0.0) const noexcept;
    std::optional<std::uint32_t> slice_index_for_note(
        int note,
        std::size_t slice_count) const noexcept;
    SampleKeySliceResolution resolve_slice_for_note(
        int note,
        const SliceMap& map) const noexcept;

private:
    SampleKeyMapConfig config_;
};

}  // namespace pulp::audio
