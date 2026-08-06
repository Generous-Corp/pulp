#include <pulp/music/music.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace pulp::music;

static_assert(std::is_trivially_copyable_v<ChordFormula>);
static_assert(std::is_trivially_copyable_v<Voicing>);
static_assert(std::is_trivially_copyable_v<ChordRecognitionList>);
static_assert(sizeof(ChordRecognitionList) < 4096);

namespace {

std::vector<int> pitches_of(const Voicing& voicing) {
    std::vector<int> result;
    for (const auto pitch : voicing.pitches())
        result.push_back(pitch);
    return result;
}

struct OracleVoicing {
    int motion = std::numeric_limits<int>::max();
    std::vector<int> pitches;
};

OracleVoicing exhaustive_voice_leading(std::span<const int> previous, PitchClass root,
                                       const ChordFormula& formula, MidiRange range) {
    OracleVoicing best;
    std::vector<int> current(previous.size());
    std::function<void(std::size_t, int, std::uint8_t)> visit = [&](std::size_t voice, int motion,
                                                                    std::uint8_t used) {
        if (voice == previous.size()) {
            if (motion < best.motion ||
                (motion == best.motion && (best.pitches.empty() || current < best.pitches))) {
                best.motion = motion;
                best.pitches = current;
            }
            return;
        }
        const int first = voice == 0 ? range.lowest : current[voice - 1] + 1;
        for (int pitch = first; pitch <= range.highest; ++pitch) {
            for (std::size_t tone = 0; tone < formula.size(); ++tone) {
                const auto bit = static_cast<std::uint8_t>(1u << tone);
                if ((used & bit) != 0 ||
                    wrap_pitch_class(pitch) != transpose(root, *formula.interval(tone)))
                    continue;
                current[voice] = pitch;
                visit(voice + 1, motion + std::abs(pitch - previous[voice]),
                      static_cast<std::uint8_t>(used | bit));
            }
        }
    };
    visit(0, 0, 0);
    return best;
}

bool unique_named_identity(PitchClass root, ChordQuality quality) {
    const auto formula = ChordFormula::for_quality(quality);
    const auto wanted = formula->pitch_classes().transposed(static_cast<int>(root));
    std::size_t matches = 0;
    for (int candidate_root = 0; candidate_root < 12; ++candidate_root) {
        for (std::size_t candidate_quality = 0; candidate_quality < kChordQualityCount;
             ++candidate_quality) {
            const auto candidate =
                ChordFormula::for_quality(static_cast<ChordQuality>(candidate_quality));
            if (candidate->pitch_classes().transposed(candidate_root) == wanted)
                ++matches;
        }
    }
    return matches == 1;
}

} // namespace

TEST_CASE("chord modifiers preserve the fixed-capacity interval contract", "[music][chord]") {
    const auto major = ChordFormula::for_quality(ChordQuality::major);
    REQUIRE(major);

    const auto ninth = major->with_extension(ChordExtension::ninth);
    REQUIRE(ninth);
    CHECK(ninth->size() == 4);
    CHECK(ninth->interval(3) == 14);
    CHECK_FALSE(ninth->quality());
    CHECK(*ninth->with_extension(ChordExtension::ninth) == *ninth);

    const auto suspended = ninth->with_suspension(ChordSuspension::fourth);
    REQUIRE(suspended);
    CHECK(suspended->interval(1) == 5);
    CHECK(suspended->interval(2) == 7);
    const auto switched_suspension = suspended->with_suspension(ChordSuspension::second);
    REQUIRE(switched_suspension);
    CHECK(switched_suspension->interval(1) == 2);
    CHECK(switched_suspension->interval(2) == 7);

    const auto sharp_fifth = suspended->with_alteration(ChordAlteration::sharp_fifth);
    REQUIRE(sharp_fifth);
    CHECK(sharp_fifth->interval(2) == 8);
    const auto flat_ninth = major->with_alteration(ChordAlteration::flat_ninth);
    REQUIRE(flat_ninth);
    CHECK(flat_ninth->interval(3) == 13);

    CHECK_FALSE(major->with_extension(static_cast<ChordExtension>(99)));
    CHECK_FALSE(major->with_suspension(static_cast<ChordSuspension>(99)));
    CHECK_FALSE(major->with_alteration(static_cast<ChordAlteration>(99)));

    const std::array<int, 8> full{0, 2, 4, 5, 7, 9, 11, 12};
    const auto full_formula = ChordFormula::from_intervals(full);
    REQUIRE(full_formula);
    CHECK_FALSE(full_formula->with_extension(ChordExtension::ninth));
}

TEST_CASE("chord spelling applies an explicit accidental policy", "[music][spelling]") {
    const auto major = ChordFormula::for_quality(ChordQuality::major);
    REQUIRE(major);
    const auto sharp = spell_chord(PitchClass::c_sharp, *major, AccidentalPolicy::prefer_sharps);
    const auto flat = spell_chord(PitchClass::c_sharp, *major, AccidentalPolicy::prefer_flats);
    REQUIRE(sharp);
    REQUIRE(flat);
    CHECK(spelling_name(*sharp->note(0)) == "C#");
    CHECK(spelling_name(*sharp->note(1)) == "E#");
    CHECK(spelling_name(*sharp->note(2)) == "G#");
    CHECK(spelling_name(*flat->note(0)) == "Db");
    CHECK(spelling_name(*flat->note(1)) == "F");
    CHECK(spelling_name(*flat->note(2)) == "Ab");

    const auto augmented = ChordFormula::for_quality(ChordQuality::augmented);
    REQUIRE(augmented);
    const auto augmented_spelling =
        spell_chord(PitchClass::c, *augmented, AccidentalPolicy::prefer_flats);
    REQUIRE(augmented_spelling);
    CHECK(spelling_name(*augmented_spelling->note(2)) == "G#");
    const auto sharp_ninth = major->with_alteration(ChordAlteration::sharp_ninth);
    REQUIRE(sharp_ninth);
    const auto altered_spelling =
        spell_chord(PitchClass::c, *sharp_ninth, AccidentalPolicy::prefer_flats);
    REQUIRE(altered_spelling);
    CHECK(spelling_name(*altered_spelling->note(3)) == "D#");

    constexpr std::array alterations{
        ChordAlteration::flat_fifth,     ChordAlteration::sharp_fifth,
        ChordAlteration::flat_ninth,     ChordAlteration::sharp_ninth,
        ChordAlteration::sharp_eleventh, ChordAlteration::flat_thirteenth,
    };
    constexpr std::array<std::string_view, alterations.size()> alteration_names{
        "Gb", "G#", "Db", "D#", "F#", "Ab",
    };
    for (std::size_t alteration = 0; alteration < alterations.size(); ++alteration) {
        const auto altered = major->with_alteration(alterations[alteration]);
        REQUIRE(altered);
        const auto spelling =
            spell_chord(PitchClass::c, *altered, AccidentalPolicy::minimize_accidentals);
        REQUIRE(spelling);
        CHECK(spelling_name(*spelling->note(spelling->size() - 1)) == alteration_names[alteration]);
    }
    CHECK(spelling_name(*spell_pitch_class(PitchClass::c_sharp,
                                           AccidentalPolicy::minimize_accidentals)) == "C#");

    for (int root = 0; root < 12; ++root) {
        for (std::size_t quality = 0; quality < kChordQualityCount; ++quality) {
            const auto formula = ChordFormula::for_quality(static_cast<ChordQuality>(quality));
            for (const auto policy :
                 {AccidentalPolicy::prefer_sharps, AccidentalPolicy::prefer_flats,
                  AccidentalPolicy::minimize_accidentals}) {
                const auto spelling = spell_chord(static_cast<PitchClass>(root), *formula, policy);
                REQUIRE(spelling);
                REQUIRE(spelling->size() == formula->size());
                for (std::size_t note = 0; note < spelling->size(); ++note) {
                    REQUIRE(spelling->note(note));
                    CHECK(spelling->note(note)->pitch_class ==
                          transpose(static_cast<PitchClass>(root), *formula->interval(note)));
                    CHECK_FALSE(spelling_name(*spelling->note(note)).empty());
                }
            }
            for (std::size_t inversion = 0; inversion < formula->size(); ++inversion) {
                const auto chord = Chord::construct(48 + root, *formula, inversion);
                REQUIRE(chord);
                CHECK(spell_chord(*chord, AccidentalPolicy::minimize_accidentals) ==
                      spell_chord(static_cast<PitchClass>(root), *formula,
                                  AccidentalPolicy::minimize_accidentals));
            }
        }
    }
    CHECK_FALSE(spell_pitch_class(static_cast<PitchClass>(12), AccidentalPolicy::prefer_sharps));
    CHECK_FALSE(spell_pitch_class(PitchClass::c, static_cast<AccidentalPolicy>(99)));
}

TEST_CASE("voicing modes cover every root and inversion deterministically", "[music][voicing]") {
    for (int root = 0; root < 12; ++root) {
        for (std::size_t quality = 0; quality < kChordQualityCount; ++quality) {
            const auto formula = ChordFormula::for_quality(static_cast<ChordQuality>(quality));
            for (std::size_t inversion = 0; inversion < formula->size(); ++inversion) {
                for (const auto mode : {VoicingMode::closed, VoicingMode::open, VoicingMode::drop2,
                                        VoicingMode::drop3, VoicingMode::spread}) {
                    if ((mode == VoicingMode::drop2 && formula->size() < 2) ||
                        (mode == VoicingMode::drop3 && formula->size() < 3))
                        continue;
                    VoicingConstraints constraints;
                    constraints.mode = mode;
                    constraints.inversion = inversion;
                    constraints.range = {24, 108};
                    const auto first = voice_chord(48 + root, *formula, constraints);
                    const auto second = voice_chord(48 + root, *formula, constraints);
                    REQUIRE(first);
                    REQUIRE(second);
                    CHECK(*first == *second);
                    CHECK(first->size() == formula->size());
                    for (std::size_t note = 1; note < first->size(); ++note) {
                        CHECK(*first->pitch(note - 1) < *first->pitch(note));
                        if (mode == VoicingMode::spread)
                            CHECK(*first->pitch(note) - *first->pitch(note - 1) >= 5);
                    }
                    CHECK(*first->pitch(0) >= constraints.range.lowest);
                    CHECK(*first->pitch(first->size() - 1) <= constraints.range.highest);
                }
            }
        }
    }

    const auto major = ChordFormula::for_quality(ChordQuality::major);
    REQUIRE(major);
    const std::array<std::array<int, 3>, 4> expected{{
        {{60, 64, 67}},
        {{60, 67, 76}},
        {{52, 60, 67}},
        {{48, 64, 67}},
    }};
    constexpr std::array modes{VoicingMode::closed, VoicingMode::open, VoicingMode::drop2,
                               VoicingMode::drop3};
    for (std::size_t mode = 0; mode < modes.size(); ++mode) {
        VoicingConstraints constraints;
        constraints.mode = modes[mode];
        const auto voiced = voice_chord(60, *major, constraints);
        REQUIRE(voiced);
        for (std::size_t note = 0; note < voiced->size(); ++note)
            CHECK(voiced->pitch(note) == expected[mode][note]);
    }
}

TEST_CASE("range fitting fails closed when constraints are impossible", "[music][voicing]") {
    const auto major = ChordFormula::for_quality(ChordQuality::major);
    REQUIRE(major);
    VoicingConstraints impossible;
    impossible.range = {60, 64};
    CHECK_FALSE(voice_chord(60, *major, impossible));
    impossible.range = {-1, 127};
    CHECK_FALSE(voice_chord(60, *major, impossible));
    impossible.range = {0, 128};
    CHECK_FALSE(voice_chord(60, *major, impossible));
    impossible.range = {0, 127};
    impossible.minimum_spacing = 8;
    impossible.maximum_spacing = 7;
    CHECK_FALSE(voice_chord(60, *major, impossible));
    impossible.minimum_spacing = 1;
    impossible.maximum_spacing = 3;
    CHECK_FALSE(voice_chord(60, *major, impossible));
    CHECK_FALSE(voice_chord(std::numeric_limits<int>::min(), *major));
    CHECK_FALSE(voice_chord(std::numeric_limits<int>::max(), *major));

    const auto fitted_high_root = voice_chord(124, *major);
    REQUIRE(fitted_high_root);
    CHECK(fitted_high_root->size() == major->size());
    CHECK(fitted_high_root->pitch(fitted_high_root->size() - 1) <= 127);
}

TEST_CASE("minimum-motion voice leading matches an exhaustive independent oracle",
          "[music][voicing][oracle]") {
    const MidiRange range{48, 76};
    constexpr std::array roots{PitchClass::c, PitchClass::d_sharp, PitchClass::f_sharp,
                               PitchClass::a};
    constexpr std::array qualities{ChordQuality::major, ChordQuality::minor,
                                   ChordQuality::dominant7};
    constexpr std::array<std::array<int, 4>, 4> previous{{
        {{48, 52, 55, 60}},
        {{51, 55, 58, 63}},
        {{57, 60, 64, 69}},
        {{60, 63, 67, 72}},
    }};

    for (const auto quality : qualities) {
        const auto formula = ChordFormula::for_quality(quality);
        REQUIRE(formula);
        for (const auto root : roots) {
            for (const auto& source : previous) {
                const std::span<const int> voices(source.data(), formula->size());
                const auto expected = exhaustive_voice_leading(voices, root, *formula, range);
                const auto actual = minimum_motion_voice_leading(voices, root, *formula, range);
                REQUIRE(actual);
                CHECK(actual->motion() == expected.motion);
                CHECK(pitches_of(*actual) == expected.pitches);
                CHECK(*minimum_motion_voice_leading(voices, root, *formula, range) == *actual);
            }
        }
    }

    const std::array<int, 3> crossing{60, 59, 67};
    const auto triad = ChordFormula::for_quality(ChordQuality::major);
    REQUIRE(triad);
    CHECK_FALSE(minimum_motion_voice_leading(crossing, PitchClass::c, *triad, range));
    const std::array<int, 2> wrong_count{60, 64};
    CHECK_FALSE(minimum_motion_voice_leading(wrong_count, PitchClass::c, *triad, range));
    const std::array<int, 3> valid_previous{60, 64, 67};
    CHECK_FALSE(
        minimum_motion_voice_leading(valid_previous, static_cast<PitchClass>(12), *triad, range));
    const std::array<int, 3> extremes{0, 64, 127};
    const MidiRange narrow{60, 62};
    CHECK_FALSE(minimum_motion_voice_leading(extremes, PitchClass::c, *triad, narrow));

    // This fixture makes an inversion-blind search observably wrong: forcing
    // root position costs 23 semitones, while the global answer costs 11.
    const std::array<int, 3> clustered{48, 49, 50};
    const auto inversion_sensitive =
        minimum_motion_voice_leading(clustered, PitchClass::f, *triad, range);
    REQUIRE(inversion_sensitive);
    const std::vector<int> inversion_sensitive_expected{48, 53, 57};
    CHECK(inversion_sensitive->motion() == 11);
    CHECK(pitches_of(*inversion_sensitive) == inversion_sensitive_expected);
    CHECK(inversion_sensitive->motion() < 23);

    const std::array<int, 3> transposition_source{48, 52, 55};
    const std::array<int, 3> transposed_source{53, 57, 60};
    const auto base_leading =
        minimum_motion_voice_leading(transposition_source, PitchClass::f, *triad, {48, 76});
    const auto transposed_leading =
        minimum_motion_voice_leading(transposed_source, PitchClass::a_sharp, *triad, {53, 81});
    REQUIRE(base_leading);
    REQUIRE(transposed_leading);
    CHECK(base_leading->motion() == transposed_leading->motion());
    for (std::size_t voice = 0; voice < base_leading->size(); ++voice)
        CHECK(*base_leading->pitch(voice) + 5 == *transposed_leading->pitch(voice));

    const std::array<int, 5> c9_intervals{0, 4, 7, 10, 14};
    const auto c9 = ChordFormula::from_intervals(c9_intervals);
    REQUIRE(c9);
    const std::array<int, 5> c9_previous{60, 62, 64, 67, 70};
    const MidiRange extended_range{48, 84};
    const auto c9_expected =
        exhaustive_voice_leading(c9_previous, PitchClass::c, *c9, extended_range);
    const auto c9_actual =
        minimum_motion_voice_leading(c9_previous, PitchClass::c, *c9, extended_range);
    REQUIRE(c9_actual);
    CHECK(c9_expected.motion == 0);
    CHECK(c9_actual->motion() == 0);
    CHECK(pitches_of(*c9_actual) == std::vector<int>(c9_previous.begin(), c9_previous.end()));
    CHECK(pitches_of(*c9_actual) == c9_expected.pitches);

    const std::array<int, 6> c11_intervals{0, 4, 7, 10, 14, 17};
    const auto c11 = ChordFormula::from_intervals(c11_intervals);
    REQUIRE(c11);
    const std::array<int, 6> c11_previous{55, 58, 60, 62, 64, 65};
    const auto c11_expected =
        exhaustive_voice_leading(c11_previous, PitchClass::d, *c11, extended_range);
    const auto c11_actual =
        minimum_motion_voice_leading(c11_previous, PitchClass::d, *c11, extended_range);
    REQUIRE(c11_actual);
    CHECK(c11_actual->motion() == c11_expected.motion);
    CHECK(pitches_of(*c11_actual) == c11_expected.pitches);
}

TEST_CASE("diatonic construction stacks scale thirds without hidden lookup tables",
          "[music][harmony]") {
    const auto major = Scale::named(PitchClass::c, NamedScale::major);
    REQUIRE(major);
    constexpr std::array<std::uint16_t, 7> expected_masks{0x0091u, 0x0224u, 0x0890u, 0x0221u,
                                                          0x0884u, 0x0211u, 0x0824u};
    for (std::size_t degree = 0; degree < 7; ++degree) {
        const auto chord = diatonic_chord(*major, degree);
        REQUIRE(chord);
        CHECK(chord->scale_degree == degree);
        CHECK(chord->root == *major->degree_pitch_class(degree));
        CHECK(chord->formula.size() == 3);
        CHECK(chord->formula.pitch_classes().transposed(static_cast<int>(chord->root)).mask() ==
              expected_masks[degree]);
    }
    CHECK_FALSE(diatonic_chord(*major, 7));
    CHECK_FALSE(diatonic_chord(*major, 0, 1));
    CHECK_FALSE(diatonic_chord(*major, 0, ChordFormula::kMaxNotes + 1));
}

TEST_CASE("ranked recognition round trips unique named chords at every inversion",
          "[music][recognition]") {
    for (int root = 0; root < 12; ++root) {
        for (std::size_t quality_index = 0; quality_index < kChordQualityCount; ++quality_index) {
            const auto quality = static_cast<ChordQuality>(quality_index);
            const auto formula = ChordFormula::for_quality(quality);
            if (!unique_named_identity(static_cast<PitchClass>(root), quality) ||
                formula->pitch_classes().size() != formula->size())
                continue;
            for (std::size_t inversion = 0; inversion < formula->size(); ++inversion) {
                const auto chord = Chord::construct(48 + root, *formula, inversion);
                REQUIRE(chord);
                std::array<int, ChordFormula::kMaxNotes> pitches{};
                for (std::size_t note = 0; note < chord->size(); ++note)
                    pitches[note] = *chord->pitch(note);
                const auto recognized =
                    recognize_chord(std::span<const int>(pitches.data(), chord->size()));
                REQUIRE(recognized);
                REQUIRE(recognized->candidate(0));
                CHECK(recognized->candidate(0)->root == static_cast<PitchClass>(root));
                CHECK(recognized->candidate(0)->quality == quality);
                CHECK(recognized->candidate(0)->score.exact());
                CHECK(recognized->candidate(0)->inversion == inversion);
                CHECK(recognized->candidate(0)->inversion_match_count == 1);
                CHECK(recognized->candidate(0)->inversion_match_mask == (1u << inversion));
                CHECK_FALSE(recognized->ambiguous());
            }
        }
    }
}

TEST_CASE("recognition exposes ambiguity and keeps ranked ties stable", "[music][recognition]") {
    const auto augmented = ChordFormula::for_quality(ChordQuality::augmented);
    REQUIRE(augmented);
    const auto first = recognize_chord(augmented->pitch_classes());
    const auto second = recognize_chord(augmented->pitch_classes());
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->size() == ChordRecognitionList::kMaxCandidates);
    CHECK(first->ambiguous());
    CHECK(first->best_equivalent_count() == 3);
    constexpr std::array augmented_root_order{PitchClass::c, PitchClass::e, PitchClass::g_sharp};
    for (std::size_t rank = 0; rank < augmented_root_order.size(); ++rank) {
        REQUIRE(first->candidate(rank));
        CHECK(first->candidate(rank)->root == augmented_root_order[rank]);
        CHECK(first->candidate(rank)->quality == ChordQuality::augmented);
        CHECK(first->candidate(rank)->score.exact());
    }
    REQUIRE(first->candidate(3));
    CHECK_FALSE(first->candidate(3)->score.exact());
    for (std::size_t index = 0; index < first->size(); ++index)
        CHECK(first->candidate(index) == second->candidate(index));

    const auto near = PitchClassSet::from_mask(0x00D1u); // C major plus F-sharp.
    REQUIRE(near);
    const auto ranked = recognize_chord(*near);
    REQUIRE(ranked);
    REQUIRE(ranked->candidate(0));
    CHECK(ranked->candidate(0)->root == PitchClass::c);
    CHECK(ranked->candidate(0)->quality == ChordQuality::major);
    CHECK(ranked->candidate(0)->score.extra == 1);
    CHECK_FALSE(ranked->candidate(0)->score.exact());

    // C, E, and B-flat are independently known to be C7 with its fifth omitted.
    // Keep this fixture literal so it does not derive the expected missing tone
    // from ChordFormula or from the recognition implementation under test.
    constexpr std::array<PitchClass, 3> omitted_fifth{PitchClass::c, PitchClass::e,
                                                      PitchClass::a_sharp};
    const auto observed = PitchClassSet::from_pitch_classes(omitted_fifth);
    REQUIRE(observed);
    const auto incomplete = recognize_chord(*observed);
    REQUIRE(incomplete);
    REQUIRE(incomplete->candidate(0));
    CHECK(incomplete->candidate(0)->root == PitchClass::c);
    CHECK(incomplete->candidate(0)->quality == ChordQuality::dominant7);
    CHECK(incomplete->candidate(0)->score.matched == 3);
    CHECK(incomplete->candidate(0)->score.missing == 1);
    CHECK(incomplete->candidate(0)->score.extra == 0);
    CHECK(incomplete->candidate(0)->score.root_present);
    CHECK_FALSE(incomplete->candidate(0)->score.exact());

    const auto octave = ChordFormula::for_quality(ChordQuality::octave);
    REQUIRE(octave);
    const auto octave_chord = Chord::construct(60, *octave);
    REQUIRE(octave_chord);
    const std::array<int, 2> octave_pitches{*octave_chord->pitch(0), *octave_chord->pitch(1)};
    const auto octave_recognition = recognize_chord(octave_pitches);
    REQUIRE(octave_recognition);
    REQUIRE(octave_recognition->candidate(0));
    CHECK(octave_recognition->candidate(0)->quality == ChordQuality::octave);
    CHECK(octave_recognition->candidate(0)->score.exact());
    CHECK(octave_recognition->candidate(0)->inversion_match_count == 2);
    CHECK(octave_recognition->candidate(0)->inversion_match_mask == 0x03u);
    CHECK_FALSE(octave_recognition->candidate(0)->inversion);

    CHECK_FALSE(recognize_chord(PitchClassSet{}));
    const std::array<int, 1> negative{-1};
    const std::array<int, 1> too_high{128};
    const std::array<int, 9> too_many{};
    CHECK_FALSE(recognize_chord(negative));
    CHECK_FALSE(recognize_chord(too_high));
    CHECK_FALSE(recognize_chord(too_many));
}
