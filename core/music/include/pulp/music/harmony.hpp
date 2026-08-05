#pragma once

#include <pulp/music/chord.hpp>

#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pulp::music {

struct DiatonicChord {
    PitchClass root = PitchClass::c;
    ChordFormula formula{};
    std::uint8_t scale_degree = 0;

    constexpr auto operator<=>(const DiatonicChord&) const = default;
};

constexpr std::optional<DiatonicChord> diatonic_chord(const Scale& scale, std::size_t degree,
                                                      std::size_t tone_count = 3) noexcept {
    if (degree >= scale.degree_count() || tone_count < 2 || tone_count > ChordFormula::kMaxNotes)
        return std::nullopt;
    const auto root = scale.degree_pitch_class(degree);
    const auto root_offset = scale.degree_to_semitones(static_cast<int>(degree));
    if (!root || !root_offset)
        return std::nullopt;

    std::array<int, ChordFormula::kMaxNotes> intervals{};
    for (std::size_t tone = 0; tone < tone_count; ++tone) {
        const auto offset =
            scale.degree_to_semitones(static_cast<int>(degree) + static_cast<int>(tone * 2));
        if (!offset)
            return std::nullopt;
        intervals[tone] = *offset - *root_offset;
    }
    const auto formula =
        ChordFormula::from_intervals(std::span<const int>(intervals.data(), tone_count));
    if (!formula)
        return std::nullopt;
    return DiatonicChord{*root, *formula, static_cast<std::uint8_t>(degree)};
}

struct RecognitionScore {
    std::uint8_t matched = 0;
    std::uint8_t missing = 0;
    std::uint8_t extra = 0;
    bool root_present = false;

    constexpr bool exact() const noexcept {
        return missing == 0 && extra == 0;
    }
    constexpr auto operator<=>(const RecognitionScore&) const = default;
};

struct ChordRecognition {
    PitchClass root = PitchClass::c;
    ChordQuality quality = ChordQuality::major;
    RecognitionScore score{};
    std::optional<std::uint8_t> inversion;
    std::uint8_t inversion_match_count = 0;
    std::uint8_t inversion_match_mask = 0;

    constexpr auto operator<=>(const ChordRecognition&) const = default;
};

class ChordRecognitionList {
  public:
    static constexpr std::size_t kMaxCandidates = kPitchClassesPerOctave * kChordQualityCount;

    constexpr std::size_t size() const noexcept {
        return count_;
    }
    constexpr std::size_t best_equivalent_count() const noexcept {
        return best_equivalent_count_;
    }
    constexpr bool ambiguous() const noexcept {
        return best_equivalent_count_ > 1;
    }

    constexpr std::optional<ChordRecognition> candidate(std::size_t index) const noexcept {
        if (index >= count_)
            return std::nullopt;
        return candidates_[index];
    }

  private:
    friend constexpr std::optional<ChordRecognitionList>
        recognize_chord(PitchClassSet, std::optional<PitchClass>) noexcept;

    std::array<ChordRecognition, kMaxCandidates> candidates_{};
    std::uint16_t count_ = 0;
    std::uint16_t best_equivalent_count_ = 0;
};

namespace detail {

constexpr bool recognition_better(const ChordRecognition& lhs,
                                  const ChordRecognition& rhs) noexcept {
    const int lhs_distance = lhs.score.missing + lhs.score.extra;
    const int rhs_distance = rhs.score.missing + rhs.score.extra;
    if (lhs_distance != rhs_distance)
        return lhs_distance < rhs_distance;
    if (lhs.score.missing != rhs.score.missing)
        return lhs.score.missing < rhs.score.missing;
    if (lhs.score.extra != rhs.score.extra)
        return lhs.score.extra < rhs.score.extra;
    if (lhs.score.root_present != rhs.score.root_present)
        return lhs.score.root_present;
    const auto inversion_rank = [](const ChordRecognition& candidate) constexpr {
        if (candidate.inversion_match_count == 1)
            return 2;
        if (candidate.inversion_match_count > 1)
            return 1;
        return 0;
    };
    if (inversion_rank(lhs) != inversion_rank(rhs))
        return inversion_rank(lhs) > inversion_rank(rhs);
    if (lhs.inversion_match_count != rhs.inversion_match_count)
        return lhs.inversion_match_count < rhs.inversion_match_count;
    if (lhs.root != rhs.root)
        return static_cast<unsigned>(lhs.root) < static_cast<unsigned>(rhs.root);
    return static_cast<unsigned>(lhs.quality) < static_cast<unsigned>(rhs.quality);
}

constexpr bool recognition_equivalent(const ChordRecognition& lhs,
                                      const ChordRecognition& rhs) noexcept {
    return lhs.score.missing == rhs.score.missing && lhs.score.extra == rhs.score.extra &&
           lhs.score.root_present == rhs.score.root_present &&
           lhs.inversion_match_count == rhs.inversion_match_count;
}

} // namespace detail

constexpr std::optional<ChordRecognitionList>
recognize_chord(PitchClassSet pitch_classes,
                std::optional<PitchClass> bass = std::nullopt) noexcept {
    if (pitch_classes.empty() || (bass && !is_valid(*bass)))
        return std::nullopt;

    ChordRecognitionList result;
    for (int root = 0; root < kPitchClassesPerOctave; ++root) {
        for (std::size_t quality_index = 0; quality_index < kChordQualityCount; ++quality_index) {
            const auto quality = static_cast<ChordQuality>(quality_index);
            const auto formula = ChordFormula::for_quality(quality);
            const auto candidate_set = formula->pitch_classes().transposed(root);
            const auto common =
                static_cast<std::uint16_t>(candidate_set.mask() & pitch_classes.mask());
            const auto missing =
                static_cast<std::uint16_t>(candidate_set.mask() & ~pitch_classes.mask());
            const auto extra =
                static_cast<std::uint16_t>(pitch_classes.mask() & ~candidate_set.mask());

            ChordRecognition candidate;
            candidate.root = static_cast<PitchClass>(root);
            candidate.quality = quality;
            candidate.score =
                RecognitionScore{static_cast<std::uint8_t>(std::popcount(common)),
                                 static_cast<std::uint8_t>(std::popcount(missing)),
                                 static_cast<std::uint8_t>(std::popcount(extra)),
                                 pitch_classes.contains(static_cast<PitchClass>(root))};
            if (bass) {
                for (std::size_t inversion = 0; inversion < formula->size(); ++inversion) {
                    if (transpose(static_cast<PitchClass>(root), *formula->interval(inversion)) ==
                        *bass) {
                        candidate.inversion_match_mask = static_cast<std::uint8_t>(
                            candidate.inversion_match_mask | (1u << inversion));
                        ++candidate.inversion_match_count;
                    }
                }
                if (candidate.inversion_match_count == 1)
                    candidate.inversion =
                        static_cast<std::uint8_t>(std::countr_zero(candidate.inversion_match_mask));
            }

            std::size_t insert = result.count_;
            while (insert > 0 &&
                   detail::recognition_better(candidate, result.candidates_[insert - 1])) {
                result.candidates_[insert] = result.candidates_[insert - 1];
                --insert;
            }
            result.candidates_[insert] = candidate;
            ++result.count_;
        }
    }
    result.best_equivalent_count_ = 1;
    while (result.best_equivalent_count_ < result.count_ &&
           detail::recognition_equivalent(result.candidates_[0],
                                          result.candidates_[result.best_equivalent_count_]))
        ++result.best_equivalent_count_;
    return result;
}

constexpr std::optional<ChordRecognitionList>
recognize_chord(std::span<const int> midi_pitches) noexcept {
    if (midi_pitches.empty() || midi_pitches.size() > ChordFormula::kMaxNotes)
        return std::nullopt;
    std::uint16_t mask = 0;
    int bass = 128;
    for (const int pitch : midi_pitches) {
        if (pitch < 0 || pitch > 127)
            return std::nullopt;
        bass = pitch < bass ? pitch : bass;
        mask = static_cast<std::uint16_t>(mask |
                                          (1u << static_cast<unsigned>(wrap_pitch_class(pitch))));
    }
    return recognize_chord(*PitchClassSet::from_mask(mask), wrap_pitch_class(bass));
}

} // namespace pulp::music
