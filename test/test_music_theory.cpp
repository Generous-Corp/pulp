#include <pulp/music/music.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>

using namespace pulp::music;

namespace {

constexpr std::array<std::uint16_t, kNamedScaleCount> kExpectedScaleMasks{{
    0x0AB5u,
    0x05ADu,
    0x06ADu,
    0x05ABu,
    0x0AD5u,
    0x06B5u,
    0x09ADu,
    0x0AADu,
    0x0295u,
    0x04A9u,
    0x056Bu,
    0x0FFFu,
    0x04E9u,
    0x0555u,
}};

constexpr std::array<ChordQuality, kChordQualityCount> kChordQualities{{
    ChordQuality::major,
    ChordQuality::minor,
    ChordQuality::diminished,
    ChordQuality::augmented,
    ChordQuality::dominant7,
    ChordQuality::major7,
    ChordQuality::minor7,
    ChordQuality::half_diminished7,
    ChordQuality::suspended2,
    ChordQuality::suspended4,
    ChordQuality::power,
    ChordQuality::octave,
}};

constexpr std::uint16_t inversion_oracle(std::uint16_t mask, int axis) noexcept {
    std::uint16_t expected = 0;
    for (int source = 0; source < kPitchClassesPerOctave; ++source) {
        if ((mask & (1u << source)) == 0)
            continue;
        int destination = (2 * axis - source) % kPitchClassesPerOctave;
        if (destination < 0)
            destination += kPitchClassesPerOctave;
        expected = static_cast<std::uint16_t>(expected | (1u << destination));
    }
    return expected;
}

} // namespace

TEST_CASE("pitch classes have checked and wrapped construction", "[music]") {
    for (int index = 0; index < kPitchClassesPerOctave; ++index) {
        REQUIRE(pitch_class_from_index(index));
        CHECK(static_cast<int>(*pitch_class_from_index(index)) == index);
        CHECK(wrap_pitch_class(index + 12) == *pitch_class_from_index(index));
        CHECK(wrap_pitch_class(index - 12) == *pitch_class_from_index(index));
    }
    CHECK_FALSE(pitch_class_from_index(-1));
    CHECK_FALSE(pitch_class_from_index(12));
    CHECK_FALSE(is_valid(static_cast<PitchClass>(12)));
    CHECK(is_valid(transpose(PitchClass::c, std::numeric_limits<int>::min())));
    CHECK(is_valid(transpose(PitchClass::b, std::numeric_limits<int>::max())));
}

TEST_CASE("pitch-class sets enforce the twelve-bit domain", "[music]") {
    CHECK(PitchClassSet::from_mask(0));
    CHECK(PitchClassSet::from_mask(kPitchClassMask));
    CHECK_FALSE(PitchClassSet::from_mask(0x1000u));

    constexpr std::array valid{PitchClass::c, PitchClass::e, PitchClass::g, PitchClass::c};
    const auto set = PitchClassSet::from_pitch_classes(valid);
    REQUIRE(set);
    CHECK(set->mask() == 0x0091u);
    CHECK(set->size() == 3);
    CHECK(set->contains(PitchClass::e));
    CHECK_FALSE(set->contains(PitchClass::f));
    CHECK_FALSE(set->contains(static_cast<PitchClass>(99)));

    constexpr std::array invalid{PitchClass::c, static_cast<PitchClass>(12)};
    CHECK_FALSE(PitchClassSet::from_pitch_classes(invalid));
    CHECK_FALSE(set->at(3));
    CHECK_FALSE(set->index_of(PitchClass::f));
}

TEST_CASE("named scale identities and masks are stable", "[music]") {
    STATIC_REQUIRE(static_cast<int>(NamedScale::major) == 0);
    STATIC_REQUIRE(static_cast<int>(NamedScale::minor_pentatonic) == 9);
    STATIC_REQUIRE(static_cast<int>(NamedScale::locrian) == 10);
    STATIC_REQUIRE(static_cast<int>(NamedScale::whole_tone) == 13);

    REQUIRE(kPulpSignalScales.size() == 10);
    CHECK(kPulpSignalScales.front().stored_name == "major");
    CHECK(kPulpSignalScales.back().stored_name == "minor_pentatonic");
    CHECK(kForgeRuntimeScales[1].stored_name == "minor");
    CHECK(kForgeRuntimeScales[7].stored_name == "pentatonic_major");
    CHECK(kForgePrimitiveScales[0].stored_name == "ionian");
    CHECK(kForgePrimitiveScales[5].stored_name == "aeolian");
    CHECK(kForgePrimitiveScales[13].stored_name == "chromatic");

    for (std::size_t index = 0; index < kNamedScaleCount; ++index) {
        const auto intervals = scale_intervals(static_cast<NamedScale>(index));
        REQUIRE(intervals);
        CHECK(intervals->mask() == kExpectedScaleMasks[index]);
        CHECK(intervals->contains(PitchClass::c));
    }
    CHECK_FALSE(scale_intervals(static_cast<NamedScale>(kNamedScaleCount)));
}

TEST_CASE("every named scale is transposition invariant at every root", "[music]") {
    for (std::size_t scale_index = 0; scale_index < kNamedScaleCount; ++scale_index) {
        const auto name = static_cast<NamedScale>(scale_index);
        const auto base = Scale::named(PitchClass::c, name);
        REQUIRE(base);
        for (int root = 0; root < kPitchClassesPerOctave; ++root) {
            const auto rooted = Scale::named(static_cast<PitchClass>(root), name);
            REQUIRE(rooted);
            CHECK(rooted->pitch_classes() == base->pitch_classes().transposed(root));
            CHECK(rooted->degree_count() == base->degree_count());
            for (std::size_t degree = 0; degree < rooted->degree_count(); ++degree) {
                const auto pitch_class = rooted->degree_pitch_class(degree);
                REQUIRE(pitch_class);
                CHECK(rooted->contains(*pitch_class));
                CHECK(rooted->degree_index(*pitch_class) == degree);
            }
        }
    }
}

TEST_CASE("scale rotation preserves absolute pitch classes", "[music]") {
    for (std::size_t scale_index = 0; scale_index < kNamedScaleCount; ++scale_index) {
        const auto scale = Scale::named(PitchClass::f_sharp, static_cast<NamedScale>(scale_index));
        REQUIRE(scale);
        for (std::size_t degree = 0; degree < scale->degree_count(); ++degree) {
            const auto rotated = scale->rotated(degree);
            REQUIRE(rotated);
            CHECK(rotated->pitch_classes() == scale->pitch_classes());
            CHECK(rotated->root() == *scale->degree_pitch_class(degree));
        }
        CHECK_FALSE(scale->rotated(scale->degree_count()));
    }
}

TEST_CASE("signed scale degrees cross octaves exactly", "[music]") {
    const auto major = Scale::named(PitchClass::c, NamedScale::major);
    REQUIRE(major);
    constexpr std::array expected{-13, -12, -1, 0, 2, 11, 12, 14, 23, 24};
    constexpr std::array degrees{-8, -7, -1, 0, 1, 6, 7, 8, 13, 14};
    for (std::size_t index = 0; index < degrees.size(); ++index)
        CHECK(major->degree_to_semitones(degrees[index]) == expected[index]);

    const auto empty = PitchClassSet::from_mask(0);
    REQUIRE(empty);
    const auto empty_scale = Scale::from_intervals(PitchClass::c, *empty);
    REQUIRE(empty_scale);
    CHECK_FALSE(empty_scale->degree_to_semitones(0));
    CHECK_FALSE(Scale::from_intervals(static_cast<PitchClass>(12), *empty));
    CHECK_FALSE(Scale::named(static_cast<PitchClass>(12), NamedScale::major));
    CHECK_FALSE(major->degree_to_semitones(std::numeric_limits<int>::min()));
    CHECK_FALSE(major->degree_to_semitones(std::numeric_limits<int>::max()));
}

TEST_CASE("pitch-class inversion is an involution around every axis", "[music]") {
    for (std::uint16_t mask = 0; mask <= kPitchClassMask; ++mask) {
        const auto set = PitchClassSet::from_mask(mask);
        REQUIRE(set);
        for (int axis = 0; axis < kPitchClassesPerOctave; ++axis) {
            const auto inverted = set->inverted(static_cast<PitchClass>(axis));
            CHECK(inverted.mask() == inversion_oracle(mask, axis));
            CHECK(inverted.inverted(static_cast<PitchClass>(axis)) == *set);
            CHECK(inverted.size() == set->size());
        }
    }

    const auto major_triad = PitchClassSet::from_mask(0x0091u);
    REQUIRE(major_triad);
    CHECK(major_triad->inverted(PitchClass::c).mask() == 0x0121u);
    CHECK(major_triad->inverted(PitchClass::c) != *major_triad);
}

TEST_CASE("named chord quality identities and formulas are stable", "[music]") {
    STATIC_REQUIRE(static_cast<int>(ChordQuality::major) == 0);
    STATIC_REQUIRE(static_cast<int>(ChordQuality::suspended4) == 9);
    STATIC_REQUIRE(static_cast<int>(ChordQuality::power) == 10);
    STATIC_REQUIRE(static_cast<int>(ChordQuality::octave) == 11);
    REQUIRE(kPulpTimelineChordQualities.size() == 10);
    CHECK(kPulpTimelineChordQualities[4].stored_name == "dominant7");
    CHECK(kPulpTimelineChordQualities[7].quality == ChordQuality::half_diminished7);
    REQUIRE(kForgeChordQualities.size() == 11);
    CHECK(kForgeChordQualities[4].stored_name == "sus2");
    CHECK(kForgeChordQualities[8].quality == ChordQuality::dominant7);
    CHECK(kForgeChordQualities[10].stored_name == "octave");

    constexpr std::array<std::array<int, 4>, kChordQualityCount> expected{{
        {{0, 4, 7, -1}},
        {{0, 3, 7, -1}},
        {{0, 3, 6, -1}},
        {{0, 4, 8, -1}},
        {{0, 4, 7, 10}},
        {{0, 4, 7, 11}},
        {{0, 3, 7, 10}},
        {{0, 3, 6, 10}},
        {{0, 2, 7, -1}},
        {{0, 5, 7, -1}},
        {{0, 7, -1, -1}},
        {{0, 12, -1, -1}},
    }};
    constexpr std::array<std::size_t, kChordQualityCount> counts{
        {3, 3, 3, 3, 4, 4, 4, 4, 3, 3, 2, 2}};
    for (std::size_t quality = 0; quality < kChordQualityCount; ++quality) {
        const auto formula = ChordFormula::for_quality(kChordQualities[quality]);
        REQUIRE(formula);
        CHECK(formula->quality() == kChordQualities[quality]);
        CHECK(formula->size() == counts[quality]);
        for (std::size_t note = 0; note < formula->size(); ++note)
            CHECK(formula->interval(note) == expected[quality][note]);
        CHECK_FALSE(formula->interval(formula->size()));
    }
    CHECK_FALSE(ChordFormula::for_quality(static_cast<ChordQuality>(kChordQualityCount)));
}

TEST_CASE("arbitrary chord formulas reject malformed interval domains", "[music]") {
    const std::array<int, 0> empty{};
    const std::array missing_root{3, 7};
    const std::array duplicate{0, 4, 4};
    const std::array descending{0, 7, 4};
    const std::array too_wide{0, ChordFormula::kMaxInterval + 1};
    const std::array too_many{0, 1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_FALSE(ChordFormula::from_intervals(empty));
    CHECK_FALSE(ChordFormula::from_intervals(missing_root));
    CHECK_FALSE(ChordFormula::from_intervals(duplicate));
    CHECK_FALSE(ChordFormula::from_intervals(descending));
    CHECK_FALSE(ChordFormula::from_intervals(too_wide));
    CHECK_FALSE(ChordFormula::from_intervals(too_many));

    const std::array altered_extension{0, 4, 7, 10, 13, 18, 21};
    const auto formula = ChordFormula::from_intervals(altered_extension);
    REQUIRE(formula);
    CHECK_FALSE(formula->quality());
    CHECK(formula->size() == altered_extension.size());
}

TEST_CASE("chord construction covers every root and legal inversion", "[music]") {
    for (const auto quality : kChordQualities) {
        const auto formula = ChordFormula::for_quality(quality);
        REQUIRE(formula);
        for (int root_pitch_class = 0; root_pitch_class < kPitchClassesPerOctave;
             ++root_pitch_class) {
            const int root_midi = 36 + root_pitch_class;
            const auto root_position = Chord::construct(root_midi, *formula);
            REQUIRE(root_position);
            for (std::size_t inversion = 0; inversion < formula->size(); ++inversion) {
                const auto chord = Chord::construct(root_midi, *formula, inversion);
                REQUIRE(chord);
                CHECK(chord->root_midi() == root_midi);
                CHECK(chord->root_pitch_class() == static_cast<PitchClass>(root_pitch_class));
                CHECK(chord->inversion() == inversion);
                CHECK(chord->size() == formula->size());
                CHECK(chord->pitch_classes() == root_position->pitch_classes());
                for (std::size_t note = 1; note < chord->size(); ++note)
                    CHECK(*chord->pitch(note - 1) < *chord->pitch(note));

                const auto repeated = Chord::construct(root_midi, *formula, inversion);
                REQUIRE(repeated);
                CHECK(*repeated == *chord);
            }
            CHECK_FALSE(Chord::construct(root_midi, *formula, formula->size()));
        }
    }
}

TEST_CASE("chord transposition round trips when the MIDI domain permits it", "[music]") {
    for (const auto quality : kChordQualities) {
        const auto formula = ChordFormula::for_quality(quality);
        REQUIRE(formula);
        for (std::size_t inversion = 0; inversion < formula->size(); ++inversion) {
            const auto chord = Chord::construct(48, *formula, inversion);
            REQUIRE(chord);
            const auto up = chord->transposed(11);
            REQUIRE(up);
            CHECK(up->pitch_classes() == chord->pitch_classes().transposed(11));
            const auto back = up->transposed(-11);
            REQUIRE(back);
            CHECK(*back == *chord);
        }
    }
}

TEST_CASE("chord construction fails closed at MIDI boundaries", "[music]") {
    const auto major = ChordFormula::for_quality(ChordQuality::major);
    REQUIRE(major);
    CHECK_FALSE(Chord::construct(-1, *major));
    CHECK_FALSE(Chord::construct(128, *major));
    CHECK_FALSE(Chord::construct(124, *major));

    const auto safe = Chord::construct(0, *major);
    REQUIRE(safe);
    CHECK_FALSE(safe->pitch(safe->size()));
    CHECK_FALSE(safe->transposed(-1));
    CHECK_FALSE(safe->transposed(std::numeric_limits<int>::min()));
    CHECK_FALSE(safe->transposed(std::numeric_limits<int>::max()));
}
