#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/subsection_reader.hpp>
#include <pulp/audio/window_enumerator.hpp>

using namespace pulp::audio;
using Catch::Matchers::WithinAbs;

namespace {

AudioFileData make_audio(uint32_t sample_rate, uint64_t frames) {
    AudioFileData data;
    data.sample_rate = sample_rate;
    data.channels.resize(1);
    data.channels[0].resize(static_cast<std::size_t>(frames), 0.0f);
    return data;
}

} // namespace

TEST_CASE("enumerate_excerpt_windows rejects invalid queries", "[audio][excerpt]") {
    auto audio = make_audio(48000, 4800);

    SECTION("zero window size") {
        auto result = enumerate_excerpt_windows("loop.wav", audio, {.text = "snare", .window_frames = 0, .hop_frames = 1200});
        REQUIRE(result.status == WindowEnumerationStatus::invalid_query);
        REQUIRE(result.windows.empty());
    }

    SECTION("zero hop size") {
        auto result = enumerate_excerpt_windows("loop.wav", audio, {.text = "snare", .window_frames = 1200, .hop_frames = 0});
        REQUIRE(result.status == WindowEnumerationStatus::invalid_query);
        REQUIRE(result.windows.empty());
    }
}

TEST_CASE("enumerate_excerpt_windows rejects empty or too-short audio", "[audio][excerpt]") {
    SECTION("empty audio") {
        AudioFileData empty;
        auto result = enumerate_excerpt_windows("loop.wav", empty, {.text = "snare", .window_frames = 1200, .hop_frames = 600});
        REQUIRE(result.status == WindowEnumerationStatus::empty_audio);
    }

    SECTION("audio shorter than window") {
        auto audio = make_audio(48000, 1000);
        auto result = enumerate_excerpt_windows("loop.wav", audio, {.text = "snare", .window_frames = 1200, .hop_frames = 600});
        REQUIRE(result.status == WindowEnumerationStatus::file_too_short);
        REQUIRE(result.source_frames == 1000);
        REQUIRE(result.windows.empty());
    }
}

TEST_CASE("enumerate_excerpt_windows uses deterministic overlap and tail coverage", "[audio][excerpt]") {
    auto audio = make_audio(48000, 11);
    auto result = enumerate_excerpt_windows("loop.wav", audio, {.text = "snare", .window_frames = 4, .hop_frames = 3});

    REQUIRE(result.status == WindowEnumerationStatus::ok);
    REQUIRE(result.windows.size() == 4);
    REQUIRE(result.windows[0].start_frame == 0);
    REQUIRE(result.windows[1].start_frame == 3);
    REQUIRE(result.windows[2].start_frame == 6);
    REQUIRE(result.windows[3].start_frame == 7);
    REQUIRE(result.windows.back().end_frame() == 11);
}

TEST_CASE("enumerate_excerpt_windows avoids duplicate tails on exact coverage",
          "[audio][excerpt][issue-640]") {
    SECTION("hop lands exactly on final valid start") {
        auto audio = make_audio(48000, 10);
        auto result = enumerate_excerpt_windows("loop.wav", audio, {.text = "snare", .window_frames = 4, .hop_frames = 3});

        REQUIRE(result.status == WindowEnumerationStatus::ok);
        REQUIRE(result.windows.size() == 3);
        REQUIRE(result.windows[0].start_frame == 0);
        REQUIRE(result.windows[1].start_frame == 3);
        REQUIRE(result.windows[2].start_frame == 6);
        REQUIRE(result.windows.back().end_frame() == 10);
    }

    SECTION("single window covers the whole file") {
        auto audio = make_audio(44100, 4);
        auto result = enumerate_excerpt_windows("hit.wav", audio, {.text = "kick", .window_frames = 4, .hop_frames = 2});

        REQUIRE(result.status == WindowEnumerationStatus::ok);
        REQUIRE(result.source_frames == 4);
        REQUIRE(result.windows.size() == 1);
        REQUIRE(result.windows[0].source_path == "hit.wav");
        REQUIRE(result.windows[0].sample_rate == 44100);
        REQUIRE(result.windows[0].start_frame == 0);
        REQUIRE(result.windows[0].frame_count == 4);
    }
}

TEST_CASE("excerpt windows report seconds from sample rate", "[audio][excerpt]") {
    auto audio = make_audio(48000, 96000);
    auto result = enumerate_excerpt_windows("loop.wav", audio, {.text = "pad", .window_frames = 24000, .hop_frames = 24000});

    REQUIRE(result.status == WindowEnumerationStatus::ok);
    REQUIRE(result.windows.size() == 4);
    REQUIRE_THAT(result.windows[1].start_seconds(), WithinAbs(0.5, 0.0001));
    REQUIRE_THAT(result.windows[1].duration_seconds(), WithinAbs(0.5, 0.0001));
}

TEST_CASE("excerpt value types expose zero-rate and summary defaults",
          "[audio][excerpt][issue-640]") {
    ExcerptWindow window;
    window.source_path = "loop.wav";
    window.start_frame = 480;
    window.frame_count = 120;

    REQUIRE(window.end_frame() == 600);
    REQUIRE_THAT(window.start_seconds(), WithinAbs(0.0, 0.0001));
    REQUIRE_THAT(window.duration_seconds(), WithinAbs(0.0, 0.0001));

    window.sample_rate = 24000;
    REQUIRE_THAT(window.start_seconds(), WithinAbs(0.02, 0.0001));
    REQUIRE_THAT(window.duration_seconds(), WithinAbs(0.005, 0.0001));

    ExcerptCandidate candidate;
    candidate.window = window;
    candidate.score = 0.75;
    candidate.backend = "deterministic";
    candidate.model_id = "hash-v1";
    REQUIRE(candidate.window.source_path == "loop.wav");
    REQUIRE(candidate.score == 0.75);
    REQUIRE(candidate.backend == "deterministic");
    REQUIRE(candidate.model_id == "hash-v1");

    ExcerptSearchSummary summary;
    REQUIRE(summary.query_text.empty());
    REQUIRE(summary.input_file_count == 0);
    REQUIRE(summary.total_frames_scanned == 0);
    REQUIRE(summary.enumerated_window_count == 0);
    REQUIRE(summary.ranked_candidate_count == 0);
}

TEST_CASE("AudioSubsectionReader clamps ranges and exposes source metadata",
          "[audio][subsection][issue-640]") {
    AudioFileData audio;
    audio.sample_rate = 48000;
    audio.channels = {
        {0.0f, 0.1f, 0.2f, 0.3f, 0.4f},
        {1.0f, 1.1f, 1.2f, 1.3f, 1.4f},
    };

    AudioSubsectionReader reader(audio, 2, 10);
    REQUIRE(reader.is_valid());
    REQUIRE(reader.num_channels() == 2);
    REQUIRE(reader.num_frames() == 3);
    REQUIRE(reader.sample_rate() == 48000);
    REQUIRE_THAT(reader.duration_seconds(), WithinAbs(3.0 / 48000.0, 0.000001));

    REQUIRE_THAT(reader.sample(0, 0), WithinAbs(0.2f, 0.000001f));
    REQUIRE_THAT(reader.sample(1, 2), WithinAbs(1.4f, 0.000001f));
    REQUIRE(reader.sample(2, 0) == 0.0f);
    REQUIRE(reader.sample(0, 3) == 0.0f);

    float dest[] = {-1.0f, -1.0f, -1.0f};
    reader.read_frames(dest, 1, 1, 8);
    REQUIRE_THAT(dest[0], WithinAbs(1.3f, 0.000001f));
    REQUIRE_THAT(dest[1], WithinAbs(1.4f, 0.000001f));
    REQUIRE(dest[2] == -1.0f);

    auto extracted = reader.extract();
    REQUIRE(extracted.sample_rate == 48000);
    REQUIRE(extracted.num_channels() == 2);
    REQUIRE(extracted.num_frames() == 3);
    REQUIRE_THAT(extracted.channels[0][0], WithinAbs(0.2f, 0.000001f));
    REQUIRE_THAT(extracted.channels[1][2], WithinAbs(1.4f, 0.000001f));
}

TEST_CASE("AudioSubsectionReader handles empty and past-end ranges",
          "[audio][subsection][issue-640]") {
    AudioSubsectionReader empty;
    REQUIRE_FALSE(empty.is_valid());
    REQUIRE(empty.num_channels() == 0);
    REQUIRE(empty.num_frames() == 0);
    REQUIRE(empty.sample_rate() == 0);
    REQUIRE(empty.duration_seconds() == 0.0);
    REQUIRE(empty.sample(0, 0) == 0.0f);
    REQUIRE(empty.extract().empty());

    AudioFileData audio = make_audio(44100, 4);
    AudioSubsectionReader past_end(audio, 99, 2);
    REQUIRE_FALSE(past_end.is_valid());
    REQUIRE(past_end.num_channels() == 1);
    REQUIRE(past_end.num_frames() == 0);
    REQUIRE(past_end.sample_rate() == 44100);

    float dest[] = {7.0f, 8.0f};
    past_end.read_frames(dest, 0, 0, 2);
    REQUIRE(dest[0] == 7.0f);
    REQUIRE(dest[1] == 8.0f);
}
