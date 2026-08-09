#pragma once

#include <pulp/music/chord.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace pulp::music {

enum class NoteLetter : std::uint8_t {
    c = 0,
    d = 1,
    e = 2,
    f = 3,
    g = 4,
    a = 5,
    b = 6,
};

enum class AccidentalPolicy : std::uint8_t {
    prefer_sharps = 0,
    prefer_flats = 1,
    // Natural spellings have zero cost. A chromatic pitch has equal-cost
    // one-accidental spellings, so this policy resolves that tie toward sharps.
    minimize_accidentals = 2,
};

struct SpelledPitchClass {
    NoteLetter letter = NoteLetter::c;
    std::int8_t accidental = 0;
    PitchClass pitch_class = PitchClass::c;

    constexpr auto operator<=>(const SpelledPitchClass&) const = default;
};

constexpr std::string_view spelling_name(SpelledPitchClass spelling) noexcept {
    constexpr std::array<std::array<std::string_view, 5>, 7> names{{
        {{"Cbb", "Cb", "C", "C#", "Cx"}},
        {{"Dbb", "Db", "D", "D#", "Dx"}},
        {{"Ebb", "Eb", "E", "E#", "Ex"}},
        {{"Fbb", "Fb", "F", "F#", "Fx"}},
        {{"Gbb", "Gb", "G", "G#", "Gx"}},
        {{"Abb", "Ab", "A", "A#", "Ax"}},
        {{"Bbb", "Bb", "B", "B#", "Bx"}},
    }};
    const auto letter = static_cast<std::size_t>(spelling.letter);
    const int accidental = spelling.accidental;
    if (letter >= names.size() || accidental < -2 || accidental > 2)
        return {};
    return names[letter][static_cast<std::size_t>(accidental + 2)];
}

constexpr std::optional<SpelledPitchClass> spell_pitch_class(PitchClass pitch_class,
                                                             AccidentalPolicy policy) noexcept {
    if (!is_valid(pitch_class) || static_cast<unsigned>(policy) > 2)
        return std::nullopt;
    constexpr std::array<NoteLetter, 12> sharp_letters{
        NoteLetter::c, NoteLetter::c, NoteLetter::d, NoteLetter::d, NoteLetter::e, NoteLetter::f,
        NoteLetter::f, NoteLetter::g, NoteLetter::g, NoteLetter::a, NoteLetter::a, NoteLetter::b};
    constexpr std::array<std::int8_t, 12> sharp_accidentals{0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    constexpr std::array<NoteLetter, 12> flat_letters{
        NoteLetter::c, NoteLetter::d, NoteLetter::d, NoteLetter::e, NoteLetter::e, NoteLetter::f,
        NoteLetter::g, NoteLetter::g, NoteLetter::a, NoteLetter::a, NoteLetter::b, NoteLetter::b};
    constexpr std::array<std::int8_t, 12> flat_accidentals{0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0};
    const auto index = static_cast<std::size_t>(pitch_class);
    const bool flats = policy == AccidentalPolicy::prefer_flats;
    return SpelledPitchClass{flats ? flat_letters[index] : sharp_letters[index],
                             flats ? flat_accidentals[index] : sharp_accidentals[index],
                             pitch_class};
}

class SpelledChord {
  public:
    constexpr std::size_t size() const noexcept {
        return count_;
    }

    constexpr std::optional<SpelledPitchClass> note(std::size_t index) const noexcept {
        if (index >= count_)
            return std::nullopt;
        return notes_[index];
    }

    constexpr auto operator<=>(const SpelledChord&) const = default;

  private:
    friend constexpr std::optional<SpelledChord> spell_chord(PitchClass, const ChordFormula&,
                                                             AccidentalPolicy) noexcept;

    std::array<SpelledPitchClass, ChordFormula::kMaxNotes> notes_{};
    std::uint8_t count_ = 0;
};

constexpr std::optional<SpelledChord> spell_chord(PitchClass root, const ChordFormula& formula,
                                                  AccidentalPolicy policy) noexcept {
    const auto spelled_root = spell_pitch_class(root, policy);
    if (!spelled_root || formula.size() == 0)
        return std::nullopt;

    constexpr std::array<int, 7> natural_pitch_classes{0, 2, 4, 5, 7, 9, 11};
    constexpr std::array<int, 12> diatonic_steps{0, 1, 1, 2, 2, 3, 4, 4, 4, 5, 6, 6};
    SpelledChord result;
    result.count_ = static_cast<std::uint8_t>(formula.size());
    const int root_letter = static_cast<int>(spelled_root->letter);

    for (std::size_t note = 0; note < formula.size(); ++note) {
        const int interval = *formula.interval(note);
        const int octaves = interval / kPitchClassesPerOctave;
        const int within_octave = interval % kPitchClassesPerOctave;
        int letter_steps = octaves * 7 + diatonic_steps[within_octave];
        if (interval == 15)
            letter_steps = 8; // sharp ninth
        else if (interval == 18)
            letter_steps = 10; // sharp eleventh
        else if (interval == 20)
            letter_steps = 12; // flat thirteenth
        const auto letter = static_cast<NoteLetter>((root_letter + letter_steps) % 7);
        const auto pitch_class = transpose(root, interval);
        int accidental =
            static_cast<int>(pitch_class) - natural_pitch_classes[static_cast<std::size_t>(letter)];
        while (accidental > 6)
            accidental -= 12;
        while (accidental < -6)
            accidental += 12;
        if (accidental < -2 || accidental > 2)
            return std::nullopt;
        result.notes_[note] =
            SpelledPitchClass{letter, static_cast<std::int8_t>(accidental), pitch_class};
    }
    return result;
}

constexpr std::optional<SpelledChord> spell_chord(const Chord& chord,
                                                  AccidentalPolicy policy) noexcept {
    return spell_chord(chord.root_pitch_class(), chord.formula(), policy);
}

} // namespace pulp::music
