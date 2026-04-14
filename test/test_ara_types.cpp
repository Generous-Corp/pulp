// Unit tests for the ARA core type layer (workstream 06 slice 6.2).
// SDK-independent: these compile and run regardless of PULP_HAS_ARA.

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/ara/types.hpp>

using namespace pulp::format::ara;

TEST_CASE("AudioSourceContentChange is a bitset", "[ara][types]") {
    auto flags = AudioSourceContentChange::Notes
               | AudioSourceContentChange::Tempo;
    REQUIRE(any(flags));
    REQUIRE(has(flags, AudioSourceContentChange::Notes));
    REQUIRE(has(flags, AudioSourceContentChange::Tempo));
    REQUIRE_FALSE(has(flags, AudioSourceContentChange::Samples));

    REQUIRE_FALSE(any(AudioSourceContentChange::None));
    REQUIRE_FALSE(has(AudioSourceContentChange::None,
                      AudioSourceContentChange::Notes));
}

TEST_CASE("AraAudioSource has expected default-constructed shape", "[ara][types]") {
    AraAudioSource src;
    REQUIRE(src.id == 0);
    REQUIRE(src.sample_count == 0);
    REQUIRE(src.sample_rate == 0.0);
    REQUIRE(src.channel_count == 0);
    REQUIRE(src.merits_content_based_processing == false);
    REQUIRE(src.name.empty());
}

TEST_CASE("AraMusicalContext defaults to 4/4 with empty tempo map", "[ara][types]") {
    AraMusicalContext ctx;
    REQUIRE(ctx.id == 0);
    REQUIRE(ctx.time_signature_numerator == 4);
    REQUIRE(ctx.time_signature_denominator == 4);
    REQUIRE(ctx.tempo_map.empty());
}

TEST_CASE("AraPlaybackRegion is a value type with zero-init defaults",
          "[ara][types]") {
    AraPlaybackRegion r;
    r.id = 100;
    r.modification_id = 200;
    r.region_sequence_id = 300;
    r.start_in_playback_time = 1.5;
    r.duration_in_playback_time = 2.5;
    r.start_in_modification_time = 1.0;      // seconds
    r.duration_in_modification_time = 2.0;   // seconds

    AraPlaybackRegion copy = r;
    REQUIRE(copy.id == 100);
    REQUIRE(copy.duration_in_playback_time == 2.5);
    REQUIRE(copy.start_in_modification_time == 1.0);
    REQUIRE(copy.duration_in_modification_time == 2.0);
}
