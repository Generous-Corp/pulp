#include "midi_utility_test_support.hpp"

TEST_CASE("Controller mapping applies caller-owned curves, smoothing, and legal CC domains",
          "[midi][utility][controller]") {
    midi::ControllerMapper<2> mapper;
    CHECK_FALSE(mapper.set_rule(0, {.input_channel = 16, .enabled = true}));
    CHECK_FALSE(mapper.set_rule(0, {.output_cc = 128, .enabled = true}));
    REQUIRE(mapper.set_rule(0, {.input_channel = 2,
                                .input_cc = 1,
                                .output_channel = 15,
                                .output_cc = 74,
                                .minimum = 0.0f,
                                .maximum = 1.0f,
                                .smoothing_seconds = 0.01f,
                                .curve = square_curve,
                                .enabled = true}));
    auto input = prepared_buffer();
    auto output = prepared_buffer();
    input.add(midi::MidiEvent::cc(2, 1, 64));
    REQUIRE(mapper.process(input, output, 48000.0).complete);
    REQUIRE(output.size() == 1);
    CHECK(output[0].channel() == 15);
    CHECK(output[0].cc_number() == 74);
    CHECK(output[0].cc_value() <= 127);
    CHECK(mapper.value(0) == Catch::Approx((64.0f / 127.0f) * (64.0f / 127.0f)));

    SECTION("non-default ranges scale the emitted CC value") {
        midi::ControllerMapper<1> ranged;
        REQUIRE(ranged.set_rule(0, {.input_channel = 2,
                                    .input_cc = 1,
                                    .output_channel = 15,
                                    .output_cc = 74,
                                    .minimum = 0.5f,
                                    .maximum = 1.0f,
                                    .enabled = true}));
        auto low = prepared_buffer();
        low.add(midi::MidiEvent::cc(2, 1, 0));
        REQUIRE(ranged.process(low, output, 48000.0).complete);
        REQUIRE(output.size() == 1);
        CHECK(output[0].cc_value() == 64);
    }

    SECTION("non-finite clock and curve values fail closed") {
        CHECK_FALSE(
            mapper.process(input, output, std::numeric_limits<double>::quiet_NaN()).complete);
        REQUIRE(mapper.set_rule(0, {.input_channel = 2,
                                    .input_cc = 1,
                                    .output_channel = 15,
                                    .output_cc = 74,
                                    .minimum = 0.0f,
                                    .maximum = 1.0f,
                                    .curve = nan_curve,
                                    .enabled = true}));
        CHECK_FALSE(
            mapper.process(input, output, 48000.0, {std::numeric_limits<std::int64_t>::max()})
                .complete);
        CHECK(output.empty());
    }

    SECTION("extreme finite domains and sample positions remain defined") {
        midi::ControllerMapper<1> extreme;
        REQUIRE(extreme.set_rule(0, {.input_channel = 2,
                                     .input_cc = 1,
                                     .output_channel = 3,
                                     .output_cc = 7,
                                     .minimum = -std::numeric_limits<float>::max(),
                                     .maximum = std::numeric_limits<float>::max(),
                                     .smoothing_seconds = 0.01f,
                                     .enabled = true}));
        auto low = prepared_buffer();
        low.add(midi::MidiEvent::cc(2, 1, 0));
        REQUIRE(extreme.process(low, output, 48000.0, {std::numeric_limits<std::int64_t>::min()})
                    .complete);
        auto high = prepared_buffer();
        high.add(midi::MidiEvent::cc(2, 1, 127));
        REQUIRE(extreme.process(high, output, 48000.0, {std::numeric_limits<std::int64_t>::max()})
                    .complete);
        REQUIRE(output.size() == 1);
        CHECK(output[0].cc_value() == 127);
        CHECK(std::isfinite(extreme.value(0)));
    }
}

TEST_CASE("Scale-aware MPE bend uses Pulp scale arithmetic with Forge tie parity",
          "[midi][utility][mpe][parity]") {
    const auto scale = music::Scale::named(music::PitchClass::c, music::NamedScale::major);
    REQUIRE(scale.has_value());
    midi::MpeNoteState note;
    note.active = true;
    note.note = 61;
    note.pitch_bend_semitones = 0.0f;
    midi::ScaleAwareMpePitch pitch;
    CHECK(pitch.target_cents(note, *scale) == Catch::Approx(-100.0f));

    note.note = 60;
    note.pitch_bend_semitones = 24.0f;
    CHECK(pitch.target_cents(note, *scale) == Catch::Approx(200.0f));

    note.pitch_bend_semitones = std::numeric_limits<float>::quiet_NaN();
    CHECK(std::isfinite(pitch.target_cents(note, *scale)));
    pitch.replace_spec({.input_bend_range_semitones = 48.0f,
                        .bend_range_degrees = std::numeric_limits<float>::max(),
                        .glide_seconds = 0.0f});
    note.pitch_bend_semitones = std::numeric_limits<float>::max();
    CHECK(std::isfinite(pitch.target_cents(note, *scale)));
}
