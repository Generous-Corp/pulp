// Tests for the Source (impulse-response) display model: file-name → clean
// name, and AudioFileInfo → human facts. Pure/hermetic — no file I/O.

#include <catch2/catch_test_macros.hpp>

#include "source_display.hpp"

using namespace pulp;
using namespace pulp::superconvolver;

TEST_CASE("clean_source_name derives a display name from the file name",
          "[superconvolver][source]") {
    // Path, extension, separators, and trailing engineering tokens are removed.
    REQUIRE(clean_source_name("/Users/x/IRs/Chapel_St-Vitus_48k_24bit.wav") ==
            "Chapel St Vitus");
    // All-lowercase words get title-cased; spaces preserved.
    REQUIRE(clean_source_name("bright plate.aiff") == "Bright Plate");
    // Acronyms / mixed case are preserved; trailing "IR" marker dropped.
    REQUIRE(clean_source_name("EMT140_IR.wav") == "EMT140");
    // Bare sample-rate / bit-depth tokens are dropped from the tail only.
    REQUIRE(clean_source_name("Hall-Large-96khz.flac") == "Hall Large");
    REQUIRE(clean_source_name("room_44100.wav") == "Room");
    // No extension, no markers → just tidied.
    REQUIRE(clean_source_name("cathedral") == "Cathedral");
    // A name that is ONLY markers falls back to the raw stem (never empty).
    REQUIRE(clean_source_name("48k_24bit.wav") == "48k_24bit");
    // Windows-style path separators.
    REQUIRE(clean_source_name("C:\\irs\\Plate 140.wav") == "Plate 140");
}

TEST_CASE("format_source_facts reports duration, channels, sample rate",
          "[superconvolver][source]") {
    audio::AudioFileInfo i;
    i.sample_rate = 48000; i.num_channels = 2; i.duration_seconds = 3.24;
    REQUIRE(format_source_facts(i) == "3.2 s · stereo · 48 kHz");

    audio::AudioFileInfo mono;
    mono.sample_rate = 44100; mono.num_channels = 1; mono.duration_seconds = 12.6;
    REQUIRE(format_source_facts(mono) == "13 s · mono · 44.1 kHz");

    // Duration derived from frames when duration_seconds is absent.
    audio::AudioFileInfo frames;
    frames.sample_rate = 48000; frames.num_channels = 4; frames.num_frames = 96000;
    REQUIRE(format_source_facts(frames) == "2.0 s · 4 ch · 48 kHz");

    // Empty info → empty facts (no crash, no garbage).
    REQUIRE(format_source_facts(audio::AudioFileInfo{}).empty());
}

TEST_CASE("derive_source_display combines name and facts",
          "[superconvolver][source]") {
    audio::AudioFileInfo i;
    i.sample_rate = 48000; i.num_channels = 2; i.duration_seconds = 2.0;
    const auto d = derive_source_display("/x/Concert_Hall_48k.wav", i);
    REQUIRE(d.name == "Concert Hall");
    REQUIRE(d.facts == "2.0 s · stereo · 48 kHz");

    // No info (e.g. an unreadable header) → name still resolves, facts empty.
    const auto d2 = derive_source_display("/x/Concert_Hall_48k.wav", std::nullopt);
    REQUIRE(d2.name == "Concert Hall");
    REQUIRE(d2.facts.empty());
}
