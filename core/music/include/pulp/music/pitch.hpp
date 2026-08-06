#pragma once

#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace pulp::music {

inline constexpr int kPitchClassesPerOctave = 12;
inline constexpr std::uint16_t kPitchClassMask = 0x0FFFu;

enum class PitchClass : std::uint8_t {
    c = 0,
    c_sharp = 1,
    d = 2,
    d_sharp = 3,
    e = 4,
    f = 5,
    f_sharp = 6,
    g = 7,
    g_sharp = 8,
    a = 9,
    a_sharp = 10,
    b = 11,
};

constexpr bool is_valid(PitchClass pitch_class) noexcept {
    return static_cast<unsigned>(pitch_class) < kPitchClassesPerOctave;
}

constexpr std::optional<PitchClass> pitch_class_from_index(int index) noexcept {
    if (index < 0 || index >= kPitchClassesPerOctave)
        return std::nullopt;
    return static_cast<PitchClass>(index);
}

constexpr PitchClass wrap_pitch_class(int index) noexcept {
    int wrapped = index % kPitchClassesPerOctave;
    if (wrapped < 0)
        wrapped += kPitchClassesPerOctave;
    return static_cast<PitchClass>(wrapped);
}

constexpr PitchClass transpose(PitchClass pitch_class, int semitones) noexcept {
    return wrap_pitch_class(static_cast<int>(pitch_class) +
                            static_cast<int>(wrap_pitch_class(semitones)));
}

class PitchClassSet {
  public:
    constexpr PitchClassSet() noexcept = default;

    static constexpr std::optional<PitchClassSet> from_mask(std::uint16_t mask) noexcept {
        if ((mask & ~kPitchClassMask) != 0)
            return std::nullopt;
        return PitchClassSet(mask, TrustedMask{});
    }

    static constexpr std::optional<PitchClassSet>
    from_pitch_classes(std::span<const PitchClass> pitch_classes) noexcept {
        std::uint16_t mask = 0;
        for (const auto pitch_class : pitch_classes) {
            if (!is_valid(pitch_class))
                return std::nullopt;
            mask = static_cast<std::uint16_t>(mask | (1u << static_cast<unsigned>(pitch_class)));
        }
        return PitchClassSet(mask, TrustedMask{});
    }

    constexpr std::uint16_t mask() const noexcept {
        return mask_;
    }
    constexpr bool empty() const noexcept {
        return mask_ == 0;
    }
    constexpr std::size_t size() const noexcept {
        return std::popcount(mask_);
    }

    constexpr bool contains(PitchClass pitch_class) const noexcept {
        return is_valid(pitch_class) && ((mask_ >> static_cast<unsigned>(pitch_class)) & 1u) != 0;
    }

    constexpr std::optional<PitchClass> at(std::size_t index) const noexcept {
        std::size_t seen = 0;
        for (int pitch = 0; pitch < kPitchClassesPerOctave; ++pitch) {
            const auto pitch_class = static_cast<PitchClass>(pitch);
            if (!contains(pitch_class))
                continue;
            if (seen == index)
                return pitch_class;
            ++seen;
        }
        return std::nullopt;
    }

    constexpr std::optional<std::size_t> index_of(PitchClass pitch_class) const noexcept {
        if (!contains(pitch_class))
            return std::nullopt;
        std::size_t index = 0;
        for (int pitch = 0; pitch < static_cast<int>(pitch_class); ++pitch)
            if ((mask_ & (1u << pitch)) != 0)
                ++index;
        return index;
    }

    constexpr PitchClassSet transposed(int semitones) const noexcept {
        std::uint16_t result = 0;
        for (int pitch = 0; pitch < kPitchClassesPerOctave; ++pitch) {
            if ((mask_ & (1u << pitch)) == 0)
                continue;
            result = static_cast<std::uint16_t>(
                result | (1u << static_cast<unsigned>(
                              transpose(static_cast<PitchClass>(pitch), semitones))));
        }
        return PitchClassSet(result, TrustedMask{});
    }

    constexpr PitchClassSet inverted(PitchClass axis = PitchClass::c) const noexcept {
        std::uint16_t result = 0;
        const int center = 2 * static_cast<int>(axis);
        for (int pitch = 0; pitch < kPitchClassesPerOctave; ++pitch) {
            if ((mask_ & (1u << pitch)) == 0)
                continue;
            result = static_cast<std::uint16_t>(
                result | (1u << static_cast<unsigned>(wrap_pitch_class(center - pitch))));
        }
        return PitchClassSet(result, TrustedMask{});
    }

    constexpr auto operator<=>(const PitchClassSet&) const = default;

  private:
    struct TrustedMask {};
    constexpr PitchClassSet(std::uint16_t mask, TrustedMask) noexcept : mask_(mask) {}

    std::uint16_t mask_ = 0;
};

enum class NamedScale : std::uint8_t {
    major = 0,
    natural_minor = 1,
    dorian = 2,
    phrygian = 3,
    lydian = 4,
    mixolydian = 5,
    harmonic_minor = 6,
    melodic_minor = 7,
    major_pentatonic = 8,
    minor_pentatonic = 9,
    locrian = 10,
    chromatic = 11,
    blues = 12,
    whole_tone = 13,
};

inline constexpr std::size_t kNamedScaleCount = 14;

namespace detail {

constexpr std::uint16_t pitch_class_mask(std::initializer_list<int> semitones) noexcept {
    std::uint16_t mask = 0;
    for (const int semitone : semitones)
        if (semitone >= 0 && semitone < kPitchClassesPerOctave)
            mask = static_cast<std::uint16_t>(mask | (1u << semitone));
    return mask;
}

} // namespace detail

struct ScaleCompatibilityEntry {
    std::string_view stored_name;
    NamedScale scale;
};

// These indices match signal::ScaleType and its stored selector domain.
inline constexpr std::array<ScaleCompatibilityEntry, 10> kPulpSignalScales{{
    {"major", NamedScale::major},
    {"natural_minor", NamedScale::natural_minor},
    {"dorian", NamedScale::dorian},
    {"phrygian", NamedScale::phrygian},
    {"lydian", NamedScale::lydian},
    {"mixolydian", NamedScale::mixolydian},
    {"harmonic_minor", NamedScale::harmonic_minor},
    {"melodic_minor", NamedScale::melodic_minor},
    {"major_pentatonic", NamedScale::major_pentatonic},
    {"minor_pentatonic", NamedScale::minor_pentatonic},
}};

// These indices match timeline::ScaleMode and its persisted spelling.
inline constexpr std::array<ScaleCompatibilityEntry, 10> kPulpTimelineScales{{
    {"major", NamedScale::major},
    {"natural_minor", NamedScale::natural_minor},
    {"harmonic_minor", NamedScale::harmonic_minor},
    {"melodic_minor", NamedScale::melodic_minor},
    {"dorian", NamedScale::dorian},
    {"phrygian", NamedScale::phrygian},
    {"lydian", NamedScale::lydian},
    {"mixolydian", NamedScale::mixolydian},
    {"locrian", NamedScale::locrian},
    {"chromatic", NamedScale::chromatic},
}};

// These indices match Forge's saved ten-scale transform selector.
inline constexpr std::array<ScaleCompatibilityEntry, 10> kForgeRuntimeScales{{
    {"major", NamedScale::major},
    {"minor", NamedScale::natural_minor},
    {"dorian", NamedScale::dorian},
    {"phrygian", NamedScale::phrygian},
    {"lydian", NamedScale::lydian},
    {"mixolydian", NamedScale::mixolydian},
    {"harmonic_minor", NamedScale::harmonic_minor},
    {"pentatonic_major", NamedScale::major_pentatonic},
    {"pentatonic_minor", NamedScale::minor_pentatonic},
    {"chromatic", NamedScale::chromatic},
}};

// These indices match Forge's broader primitive scale table.
inline constexpr std::array<ScaleCompatibilityEntry, 14> kForgePrimitiveScales{{
    {"ionian", NamedScale::major},
    {"dorian", NamedScale::dorian},
    {"phrygian", NamedScale::phrygian},
    {"lydian", NamedScale::lydian},
    {"mixolydian", NamedScale::mixolydian},
    {"aeolian", NamedScale::natural_minor},
    {"locrian", NamedScale::locrian},
    {"harmonic_minor", NamedScale::harmonic_minor},
    {"melodic_minor", NamedScale::melodic_minor},
    {"major_pentatonic", NamedScale::major_pentatonic},
    {"minor_pentatonic", NamedScale::minor_pentatonic},
    {"blues", NamedScale::blues},
    {"whole_tone", NamedScale::whole_tone},
    {"chromatic", NamedScale::chromatic},
}};

constexpr std::optional<PitchClassSet> scale_intervals(NamedScale scale) noexcept {
    constexpr std::array<std::uint16_t, kNamedScaleCount> masks{{
        detail::pitch_class_mask({0, 2, 4, 5, 7, 9, 11}),
        detail::pitch_class_mask({0, 2, 3, 5, 7, 8, 10}),
        detail::pitch_class_mask({0, 2, 3, 5, 7, 9, 10}),
        detail::pitch_class_mask({0, 1, 3, 5, 7, 8, 10}),
        detail::pitch_class_mask({0, 2, 4, 6, 7, 9, 11}),
        detail::pitch_class_mask({0, 2, 4, 5, 7, 9, 10}),
        detail::pitch_class_mask({0, 2, 3, 5, 7, 8, 11}),
        detail::pitch_class_mask({0, 2, 3, 5, 7, 9, 11}),
        detail::pitch_class_mask({0, 2, 4, 7, 9}),
        detail::pitch_class_mask({0, 3, 5, 7, 10}),
        detail::pitch_class_mask({0, 1, 3, 5, 6, 8, 10}),
        detail::pitch_class_mask({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}),
        detail::pitch_class_mask({0, 3, 5, 6, 7, 10}),
        detail::pitch_class_mask({0, 2, 4, 6, 8, 10}),
    }};
    const auto index = static_cast<std::size_t>(scale);
    if (index >= masks.size())
        return std::nullopt;
    return PitchClassSet::from_mask(masks[index]);
}

class Scale {
  public:
    static constexpr std::optional<Scale> from_intervals(PitchClass root,
                                                         PitchClassSet intervals) noexcept {
        if (!is_valid(root))
            return std::nullopt;
        return Scale(root, intervals);
    }

    static constexpr std::optional<Scale> named(PitchClass root, NamedScale name) noexcept {
        if (!is_valid(root))
            return std::nullopt;
        const auto intervals = scale_intervals(name);
        if (!intervals)
            return std::nullopt;
        return Scale(root, *intervals);
    }

    constexpr PitchClass root() const noexcept {
        return root_;
    }
    constexpr PitchClassSet intervals() const noexcept {
        return intervals_;
    }
    constexpr PitchClassSet pitch_classes() const noexcept {
        return intervals_.transposed(static_cast<int>(root_));
    }
    constexpr std::size_t degree_count() const noexcept {
        return intervals_.size();
    }

    constexpr bool contains(PitchClass pitch_class) const noexcept {
        return pitch_classes().contains(pitch_class);
    }

    constexpr std::optional<PitchClass> degree_pitch_class(std::size_t degree) const noexcept {
        const auto interval = intervals_.at(degree);
        if (!interval)
            return std::nullopt;
        return transpose(root_, static_cast<int>(*interval));
    }

    constexpr std::optional<std::size_t> degree_index(PitchClass pitch_class) const noexcept {
        if (!is_valid(pitch_class))
            return std::nullopt;
        return intervals_.index_of(
            wrap_pitch_class(static_cast<int>(pitch_class) - static_cast<int>(root_)));
    }

    constexpr std::optional<int> degree_to_semitones(int degree) const noexcept {
        const int count = static_cast<int>(degree_count());
        if (count == 0)
            return std::nullopt;
        int octave = degree / count;
        int within = degree % count;
        if (within < 0) {
            within += count;
            --octave;
        }
        const auto interval = intervals_.at(static_cast<std::size_t>(within));
        const auto semitones = static_cast<std::int64_t>(octave) * kPitchClassesPerOctave +
                               static_cast<int>(*interval);
        if (semitones < std::numeric_limits<int>::min() ||
            semitones > std::numeric_limits<int>::max())
            return std::nullopt;
        return static_cast<int>(semitones);
    }

    constexpr Scale transposed(int semitones) const noexcept {
        return Scale(transpose(root_, semitones), intervals_);
    }

    constexpr std::optional<Scale> rotated(std::size_t degree) const noexcept {
        const auto pivot = intervals_.at(degree);
        if (!pivot)
            return std::nullopt;
        const int semitones = static_cast<int>(*pivot);
        return Scale(transpose(root_, semitones), intervals_.transposed(-semitones));
    }

    constexpr auto operator<=>(const Scale&) const = default;

  private:
    constexpr Scale(PitchClass root, PitchClassSet intervals) noexcept
        : root_(root), intervals_(intervals) {}

    PitchClass root_ = PitchClass::c;
    PitchClassSet intervals_{};
};

} // namespace pulp::music
