#pragma once

#include <pulp/music/pitch.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace pulp::music {

enum class ChordQuality : std::uint8_t {
    major = 0,
    minor = 1,
    diminished = 2,
    augmented = 3,
    dominant7 = 4,
    major7 = 5,
    minor7 = 6,
    half_diminished7 = 7,
    suspended2 = 8,
    suspended4 = 9,
    power = 10,
    octave = 11,
};

inline constexpr std::size_t kChordQualityCount = 12;

struct ChordCompatibilityEntry {
    std::string_view stored_name;
    ChordQuality quality;
};

// These indices match timeline::ChordQuality and its persisted spelling.
inline constexpr std::array<ChordCompatibilityEntry, 10> kPulpTimelineChordQualities{{
    {"major", ChordQuality::major},
    {"minor", ChordQuality::minor},
    {"diminished", ChordQuality::diminished},
    {"augmented", ChordQuality::augmented},
    {"dominant7", ChordQuality::dominant7},
    {"major7", ChordQuality::major7},
    {"minor7", ChordQuality::minor7},
    {"half_diminished7", ChordQuality::half_diminished7},
    {"suspended2", ChordQuality::suspended2},
    {"suspended4", ChordQuality::suspended4},
}};

// These indices and short names match Forge's chord transform table.
inline constexpr std::array<ChordCompatibilityEntry, 11> kForgeChordQualities{{
    {"maj", ChordQuality::major},
    {"min", ChordQuality::minor},
    {"dim", ChordQuality::diminished},
    {"aug", ChordQuality::augmented},
    {"sus2", ChordQuality::suspended2},
    {"sus4", ChordQuality::suspended4},
    {"maj7", ChordQuality::major7},
    {"min7", ChordQuality::minor7},
    {"dom7", ChordQuality::dominant7},
    {"power", ChordQuality::power},
    {"octave", ChordQuality::octave},
}};

class ChordFormula {
  public:
    static constexpr std::size_t kMaxNotes = 8;
    static constexpr int kMaxInterval = 48;

    static constexpr std::optional<ChordFormula>
    from_intervals(std::span<const int> intervals) noexcept {
        if (intervals.empty() || intervals.size() > kMaxNotes || intervals.front() != 0)
            return std::nullopt;

        ChordFormula result;
        int previous = -1;
        for (std::size_t i = 0; i < intervals.size(); ++i) {
            const int interval = intervals[i];
            if (interval <= previous || interval > kMaxInterval)
                return std::nullopt;
            result.intervals_[i] = static_cast<std::int8_t>(interval);
            previous = interval;
        }
        result.count_ = static_cast<std::uint8_t>(intervals.size());
        return result;
    }

    static constexpr std::optional<ChordFormula> for_quality(ChordQuality quality) noexcept {
        constexpr std::array<std::array<std::int8_t, kMaxNotes>, kChordQualityCount> intervals{{
            {{0, 4, 7}},
            {{0, 3, 7}},
            {{0, 3, 6}},
            {{0, 4, 8}},
            {{0, 4, 7, 10}},
            {{0, 4, 7, 11}},
            {{0, 3, 7, 10}},
            {{0, 3, 6, 10}},
            {{0, 2, 7}},
            {{0, 5, 7}},
            {{0, 7}},
            {{0, 12}},
        }};
        constexpr std::array<std::uint8_t, kChordQualityCount> counts{{
            3,
            3,
            3,
            3,
            4,
            4,
            4,
            4,
            3,
            3,
            2,
            2,
        }};
        const auto index = static_cast<std::size_t>(quality);
        if (index >= intervals.size())
            return std::nullopt;
        ChordFormula result;
        result.intervals_ = intervals[index];
        result.count_ = counts[index];
        result.quality_ = quality;
        return result;
    }

    constexpr std::size_t size() const noexcept {
        return count_;
    }
    constexpr std::optional<int> interval(std::size_t index) const noexcept {
        if (index >= count_)
            return std::nullopt;
        return static_cast<int>(intervals_[index]);
    }
    constexpr std::optional<ChordQuality> quality() const noexcept {
        return quality_;
    }

    constexpr PitchClassSet pitch_classes() const noexcept {
        std::uint16_t mask = 0;
        for (std::size_t i = 0; i < count_; ++i)
            mask = static_cast<std::uint16_t>(
                mask | (1u << static_cast<unsigned>(wrap_pitch_class(intervals_[i]))));
        return *PitchClassSet::from_mask(mask);
    }

    constexpr auto operator<=>(const ChordFormula&) const = default;

  private:
    std::array<std::int8_t, kMaxNotes> intervals_{};
    std::uint8_t count_ = 0;
    std::optional<ChordQuality> quality_;
};

class Chord {
  public:
    static constexpr std::optional<Chord> construct(int root_midi, const ChordFormula& formula,
                                                    std::size_t inversion = 0) noexcept {
        if (root_midi < 0 || root_midi > 127 || formula.size() == 0 || inversion >= formula.size())
            return std::nullopt;

        Chord result;
        result.root_midi_ = static_cast<std::uint8_t>(root_midi);
        result.formula_ = formula;
        result.inversion_ = static_cast<std::uint8_t>(inversion);
        result.count_ = static_cast<std::uint8_t>(formula.size());

        int previous = -1;
        for (std::size_t output = 0; output < formula.size(); ++output) {
            const std::size_t source = (output + inversion) % formula.size();
            int pitch = root_midi + *formula.interval(source);
            if (source < inversion)
                while (pitch <= previous)
                    pitch += kPitchClassesPerOctave;
            if (pitch > 127)
                return std::nullopt;
            result.pitches_[output] = static_cast<std::uint8_t>(pitch);
            previous = pitch;
        }
        return result;
    }

    constexpr int root_midi() const noexcept {
        return root_midi_;
    }
    constexpr PitchClass root_pitch_class() const noexcept {
        return wrap_pitch_class(root_midi_);
    }
    constexpr const ChordFormula& formula() const noexcept {
        return formula_;
    }
    constexpr std::size_t inversion() const noexcept {
        return inversion_;
    }
    constexpr std::size_t size() const noexcept {
        return count_;
    }
    constexpr std::optional<int> pitch(std::size_t index) const noexcept {
        if (index >= count_)
            return std::nullopt;
        return pitches_[index];
    }

    constexpr PitchClassSet pitch_classes() const noexcept {
        std::uint16_t mask = 0;
        for (std::size_t i = 0; i < count_; ++i)
            mask = static_cast<std::uint16_t>(
                mask | (1u << static_cast<unsigned>(wrap_pitch_class(pitches_[i]))));
        return *PitchClassSet::from_mask(mask);
    }

    constexpr std::optional<Chord> transposed(int semitones) const noexcept {
        const auto transposed_root = static_cast<std::int64_t>(root_midi_) + semitones;
        if (transposed_root < 0 || transposed_root > 127)
            return std::nullopt;
        return construct(static_cast<int>(transposed_root), formula_, inversion_);
    }

    constexpr auto operator<=>(const Chord&) const = default;

  private:
    ChordFormula formula_{};
    std::array<std::uint8_t, ChordFormula::kMaxNotes> pitches_{};
    std::uint8_t root_midi_ = 0;
    std::uint8_t count_ = 0;
    std::uint8_t inversion_ = 0;
};

} // namespace pulp::music
