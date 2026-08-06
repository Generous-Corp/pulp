#include <pulp/music/music.hpp>
#include <pulp/signal/harmony_engine.hpp>
#include <pulp/timeline/model.hpp>

#include "chord_scale_names.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

TEST_CASE("signal scale identity delegates to the shared music table", "[music]") {
    using pulp::signal::ScaleType;

    STATIC_REQUIRE(static_cast<int>(ScaleType::major) == 0);
    STATIC_REQUIRE(static_cast<int>(ScaleType::natural_minor) == 1);
    STATIC_REQUIRE(static_cast<int>(ScaleType::dorian) == 2);
    STATIC_REQUIRE(static_cast<int>(ScaleType::phrygian) == 3);
    STATIC_REQUIRE(static_cast<int>(ScaleType::lydian) == 4);
    STATIC_REQUIRE(static_cast<int>(ScaleType::mixolydian) == 5);
    STATIC_REQUIRE(static_cast<int>(ScaleType::harmonic_minor) == 6);
    STATIC_REQUIRE(static_cast<int>(ScaleType::melodic_minor) == 7);
    STATIC_REQUIRE(static_cast<int>(ScaleType::major_pentatonic) == 8);
    STATIC_REQUIRE(static_cast<int>(ScaleType::minor_pentatonic) == 9);
    STATIC_REQUIRE(pulp::signal::kScaleCount == 10);

    for (std::size_t index = 0; index < pulp::music::kPulpSignalScales.size(); ++index) {
        const auto shared =
            pulp::music::scale_intervals(pulp::music::kPulpSignalScales[index].scale);
        REQUIRE(shared);
        CHECK(pulp::signal::kScaleTable[index] == shared->mask());
    }
}

TEST_CASE("timeline chord identity delegates to the shared music table", "[music]") {
    using pulp::timeline::ChordQuality;
    namespace detail = pulp::timeline::detail;

    constexpr std::array values{
        ChordQuality::Major,      ChordQuality::Minor,           ChordQuality::Diminished,
        ChordQuality::Augmented,  ChordQuality::Dominant7,       ChordQuality::Major7,
        ChordQuality::Minor7,     ChordQuality::HalfDiminished7, ChordQuality::Suspended2,
        ChordQuality::Suspended4,
    };
    STATIC_REQUIRE(static_cast<int>(ChordQuality::Major) == 0);
    STATIC_REQUIRE(static_cast<int>(ChordQuality::Suspended4) == 9);

    for (std::size_t index = 0; index < values.size(); ++index) {
        CHECK(static_cast<std::size_t>(values[index]) == index);
        CHECK(detail::chord_quality_name(values[index]) ==
              pulp::music::kPulpTimelineChordQualities[index].stored_name);
        CHECK(detail::chord_quality_from_name(detail::chord_quality_name(values[index])) ==
              values[index]);
        CHECK(detail::music_chord_quality(values[index]) ==
              pulp::music::kPulpTimelineChordQualities[index].quality);
    }

    CHECK(detail::chord_quality_name(static_cast<ChordQuality>(10)).empty());
    CHECK_FALSE(detail::chord_quality_from_name("unknown"));
    CHECK_FALSE(detail::music_chord_quality(static_cast<ChordQuality>(10)));
}

TEST_CASE("timeline scale identity delegates to the shared music table", "[music]") {
    using pulp::timeline::ScaleMode;
    namespace detail = pulp::timeline::detail;

    constexpr std::array values{
        ScaleMode::Major,        ScaleMode::NaturalMinor, ScaleMode::HarmonicMinor,
        ScaleMode::MelodicMinor, ScaleMode::Dorian,       ScaleMode::Phrygian,
        ScaleMode::Lydian,       ScaleMode::Mixolydian,   ScaleMode::Locrian,
        ScaleMode::Chromatic,
    };
    STATIC_REQUIRE(static_cast<int>(ScaleMode::Major) == 0);
    STATIC_REQUIRE(static_cast<int>(ScaleMode::Chromatic) == 9);

    for (std::size_t index = 0; index < values.size(); ++index) {
        CHECK(static_cast<std::size_t>(values[index]) == index);
        CHECK(detail::scale_mode_name(values[index]) ==
              pulp::music::kPulpTimelineScales[index].stored_name);
        CHECK(detail::scale_mode_from_name(detail::scale_mode_name(values[index])) ==
              values[index]);
        CHECK(detail::music_scale_mode(values[index]) ==
              pulp::music::kPulpTimelineScales[index].scale);

        const auto intervals =
            pulp::music::scale_intervals(*detail::music_scale_mode(values[index]));
        REQUIRE(intervals);
        CHECK(intervals->contains(pulp::music::PitchClass::c));
    }

    CHECK(detail::scale_mode_name(static_cast<ScaleMode>(10)).empty());
    CHECK_FALSE(detail::scale_mode_from_name("unknown"));
    CHECK_FALSE(detail::music_scale_mode(static_cast<ScaleMode>(10)));
}
