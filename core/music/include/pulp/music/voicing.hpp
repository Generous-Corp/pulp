#pragma once

#include <pulp/music/chord.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>

namespace pulp::music {

enum class VoicingMode : std::uint8_t {
    closed = 0,
    open = 1,
    drop2 = 2,
    drop3 = 3,
    spread = 4,
};

struct MidiRange {
    int lowest = 0;
    int highest = 127;

    constexpr bool valid() const noexcept {
        return lowest >= 0 && highest <= 127 && lowest <= highest;
    }

    constexpr auto operator<=>(const MidiRange&) const = default;
};

struct VoicingConstraints {
    VoicingMode mode = VoicingMode::closed;
    MidiRange range{};
    std::size_t inversion = 0;
    int minimum_spacing = 1;
    int maximum_spacing = 127;

    constexpr bool valid() const noexcept {
        return static_cast<unsigned>(mode) <= static_cast<unsigned>(VoicingMode::spread) &&
               range.valid() && minimum_spacing >= 1 && maximum_spacing >= minimum_spacing &&
               maximum_spacing <= 127;
    }
};

namespace detail {
struct VoicingAccess;
}

class Voicing {
  public:
    constexpr std::size_t size() const noexcept {
        return count_;
    }

    constexpr std::optional<int> pitch(std::size_t index) const noexcept {
        if (index >= count_)
            return std::nullopt;
        return pitches_[index];
    }

    constexpr std::span<const std::uint8_t> pitches() const noexcept {
        return {pitches_.data(), count_};
    }

    constexpr int motion() const noexcept {
        return motion_;
    }
    constexpr auto operator<=>(const Voicing&) const = default;

  private:
    friend std::optional<Voicing> voice_chord(int, const ChordFormula&,
                                              VoicingConstraints) noexcept;
    friend std::optional<Voicing> minimum_motion_voice_leading(std::span<const int>, PitchClass,
                                                               const ChordFormula&,
                                                               MidiRange) noexcept;
    friend struct detail::VoicingAccess;

    std::array<std::uint8_t, ChordFormula::kMaxNotes> pitches_{};
    std::uint8_t count_ = 0;
    int motion_ = 0;
};

namespace detail {

struct VoicingAccess {
    static Voicing make(std::array<std::uint8_t, ChordFormula::kMaxNotes> pitches,
                        std::size_t count, int motion) noexcept {
        Voicing result;
        result.pitches_ = pitches;
        result.count_ = static_cast<std::uint8_t>(count);
        result.motion_ = motion;
        return result;
    }
};

inline std::optional<Voicing> solve_ordered_voicing(std::span<const PitchClass> pitch_classes,
                                                    std::span<const int> targets, MidiRange range,
                                                    int minimum_spacing,
                                                    int maximum_spacing) noexcept {
    if (pitch_classes.empty() || pitch_classes.size() > ChordFormula::kMaxNotes ||
        pitch_classes.size() != targets.size() || !range.valid() || minimum_spacing < 1 ||
        maximum_spacing < minimum_spacing || maximum_spacing > 127)
        return std::nullopt;

    constexpr int unreachable = std::numeric_limits<int>::max() / 4;
    std::array<std::array<int, 129>, ChordFormula::kMaxNotes + 1> cost{};
    for (auto& row : cost)
        row.fill(unreachable);
    for (int previous = -1; previous <= 127; ++previous)
        cost[pitch_classes.size()][static_cast<std::size_t>(previous + 1)] = 0;

    for (std::size_t reverse = pitch_classes.size(); reverse-- > 0;) {
        for (int previous = -1; previous <= 127; ++previous) {
            int best = unreachable;
            const int first = previous < 0 ? range.lowest : previous + minimum_spacing;
            const int last =
                previous < 0 ? range.highest : std::min(range.highest, previous + maximum_spacing);
            for (int pitch = first; pitch <= last; ++pitch) {
                if (wrap_pitch_class(pitch) != pitch_classes[reverse])
                    continue;
                const int suffix = cost[reverse + 1][static_cast<std::size_t>(pitch + 1)];
                if (suffix == unreachable)
                    continue;
                const int local = std::abs(pitch - targets[reverse]);
                best = std::min(best, local + suffix);
            }
            cost[reverse][static_cast<std::size_t>(previous + 1)] = best;
        }
    }

    const int total = cost[0][0];
    if (total == unreachable)
        return std::nullopt;
    std::array<std::uint8_t, ChordFormula::kMaxNotes> result{};
    int previous = -1;
    for (std::size_t note = 0; note < pitch_classes.size(); ++note) {
        const int wanted = cost[note][static_cast<std::size_t>(previous + 1)];
        const int first = previous < 0 ? range.lowest : previous + minimum_spacing;
        const int last =
            previous < 0 ? range.highest : std::min(range.highest, previous + maximum_spacing);
        for (int pitch = first; pitch <= last; ++pitch) {
            if (wrap_pitch_class(pitch) != pitch_classes[note])
                continue;
            const int suffix = cost[note + 1][static_cast<std::size_t>(pitch + 1)];
            if (suffix != unreachable && std::abs(pitch - targets[note]) + suffix == wanted) {
                result[note] = static_cast<std::uint8_t>(pitch);
                previous = pitch;
                break;
            }
        }
    }
    return VoicingAccess::make(result, pitch_classes.size(), total);
}

inline std::optional<Voicing>
solve_unordered_voice_leading(std::span<const PitchClass> pitch_classes,
                              std::span<const int> previous, MidiRange range) noexcept {
    if (pitch_classes.empty() || pitch_classes.size() > ChordFormula::kMaxNotes ||
        pitch_classes.size() != previous.size() || !range.valid())
        return std::nullopt;

    constexpr std::uint16_t unreachable = std::numeric_limits<std::uint16_t>::max();
    constexpr std::size_t state_count = 1u << ChordFormula::kMaxNotes;
    std::array<std::array<std::uint16_t, 129>, state_count> cost{};
    for (auto& row : cost)
        row.fill(unreachable);

    const auto full_mask = static_cast<std::size_t>((1u << pitch_classes.size()) - 1u);
    cost[full_mask].fill(0);
    for (std::size_t mask = full_mask; mask-- > 0;) {
        const auto voice = static_cast<std::size_t>(std::popcount(mask));
        for (int last_pitch = -1; last_pitch <= 127; ++last_pitch) {
            std::uint16_t best = unreachable;
            const int first = last_pitch < 0 ? range.lowest : last_pitch + 1;
            for (std::size_t tone = 0; tone < pitch_classes.size(); ++tone) {
                const auto tone_bit = static_cast<std::size_t>(1u << tone);
                if ((mask & tone_bit) != 0)
                    continue;
                for (int pitch = first; pitch <= range.highest; ++pitch) {
                    if (wrap_pitch_class(pitch) != pitch_classes[tone])
                        continue;
                    const auto suffix = cost[mask | tone_bit][static_cast<std::size_t>(pitch + 1)];
                    if (suffix == unreachable)
                        continue;
                    const int candidate = std::abs(pitch - previous[voice]) + suffix;
                    if (candidate < best)
                        best = static_cast<std::uint16_t>(candidate);
                }
            }
            cost[mask][static_cast<std::size_t>(last_pitch + 1)] = best;
        }
    }

    if (cost[0][0] == unreachable)
        return std::nullopt;
    std::array<std::uint8_t, ChordFormula::kMaxNotes> result{};
    std::size_t mask = 0;
    int last_pitch = -1;
    for (std::size_t voice = 0; voice < pitch_classes.size(); ++voice) {
        const auto wanted = cost[mask][static_cast<std::size_t>(last_pitch + 1)];
        const int first = last_pitch < 0 ? range.lowest : last_pitch + 1;
        bool selected = false;
        for (int pitch = first; pitch <= range.highest && !selected; ++pitch) {
            for (std::size_t tone = 0; tone < pitch_classes.size(); ++tone) {
                const auto tone_bit = static_cast<std::size_t>(1u << tone);
                if ((mask & tone_bit) != 0 || wrap_pitch_class(pitch) != pitch_classes[tone])
                    continue;
                const auto suffix = cost[mask | tone_bit][static_cast<std::size_t>(pitch + 1)];
                if (suffix != unreachable && std::abs(pitch - previous[voice]) + suffix == wanted) {
                    result[voice] = static_cast<std::uint8_t>(pitch);
                    mask |= tone_bit;
                    last_pitch = pitch;
                    selected = true;
                    break;
                }
            }
        }
    }
    return VoicingAccess::make(result, pitch_classes.size(), cost[0][0]);
}

inline void sort_parallel(std::array<int, ChordFormula::kMaxNotes>& pitches,
                          std::array<PitchClass, ChordFormula::kMaxNotes>& pitch_classes,
                          std::size_t count) noexcept {
    for (std::size_t right = 1; right < count; ++right) {
        const int pitch = pitches[right];
        const PitchClass pitch_class = pitch_classes[right];
        std::size_t left = right;
        while (left > 0 && pitches[left - 1] > pitch) {
            pitches[left] = pitches[left - 1];
            pitch_classes[left] = pitch_classes[left - 1];
            --left;
        }
        pitches[left] = pitch;
        pitch_classes[left] = pitch_class;
    }
}

} // namespace detail

inline std::optional<Voicing> voice_chord(int root_midi, const ChordFormula& formula,
                                          VoicingConstraints constraints = {}) noexcept {
    if (!constraints.valid() || formula.size() == 0 || constraints.inversion >= formula.size())
        return std::nullopt;
    if (root_midi < 0 || root_midi > 127)
        return std::nullopt;

    std::array<int, ChordFormula::kMaxNotes> targets{};
    std::array<PitchClass, ChordFormula::kMaxNotes> pitch_classes{};
    int previous = -1;
    for (std::size_t note = 0; note < formula.size(); ++note) {
        const std::size_t source = (note + constraints.inversion) % formula.size();
        targets[note] = root_midi + *formula.interval(source);
        if (source < constraints.inversion)
            while (targets[note] <= previous)
                targets[note] += kPitchClassesPerOctave;
        pitch_classes[note] = wrap_pitch_class(targets[note]);
        previous = targets[note];
    }

    switch (constraints.mode) {
    case VoicingMode::closed:
        break;
    case VoicingMode::open:
        for (std::size_t note = 1; note + 1 < formula.size(); note += 2)
            targets[note] += 12;
        break;
    case VoicingMode::drop2:
        if (formula.size() < 2)
            return std::nullopt;
        targets[formula.size() - 2] -= 12;
        break;
    case VoicingMode::drop3:
        if (formula.size() < 3)
            return std::nullopt;
        targets[formula.size() - 3] -= 12;
        break;
    case VoicingMode::spread:
        constraints.minimum_spacing = std::max(constraints.minimum_spacing, 5);
        break;
    }
    detail::sort_parallel(targets, pitch_classes, formula.size());
    return detail::solve_ordered_voicing(
        std::span<const PitchClass>(pitch_classes.data(), formula.size()),
        std::span<const int>(targets.data(), formula.size()), constraints.range,
        constraints.minimum_spacing, constraints.maximum_spacing);
}

inline std::optional<Voicing> minimum_motion_voice_leading(std::span<const int> previous,
                                                           PitchClass target_root,
                                                           const ChordFormula& formula,
                                                           MidiRange range = {}) noexcept {
    if (!is_valid(target_root) || !range.valid() || previous.empty() ||
        previous.size() != formula.size() || previous.size() > ChordFormula::kMaxNotes)
        return std::nullopt;
    for (std::size_t voice = 0; voice < previous.size(); ++voice) {
        if (previous[voice] < 0 || previous[voice] > 127 || !range.valid() ||
            (voice != 0 && previous[voice - 1] >= previous[voice]))
            return std::nullopt;
    }

    std::array<PitchClass, ChordFormula::kMaxNotes> pitch_classes{};
    for (std::size_t tone = 0; tone < formula.size(); ++tone)
        pitch_classes[tone] = transpose(target_root, *formula.interval(tone));
    return detail::solve_unordered_voice_leading(
        std::span<const PitchClass>(pitch_classes.data(), formula.size()), previous, range);
}

} // namespace pulp::music
