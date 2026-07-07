#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/tuning.hpp>

#if PULP_HAS_MTS_ESP
#include <pulp/midi/mts_esp_tuning.hpp>
#endif

#include <array>
#include <cstdint>
#include <limits>

using Catch::Approx;

TEST_CASE("equal temperament tuning returns canonical MIDI frequencies") {
    REQUIRE(pulp::midi::equal_temperament_frequency(69) == Approx(440.0));
    REQUIRE(pulp::midi::equal_temperament_frequency(60) == Approx(261.6255653006));
    REQUIRE(pulp::midi::equal_temperament_frequency(-1) == Approx(0.0));
    REQUIRE(pulp::midi::equal_temperament_frequency(
                69,
                std::numeric_limits<double>::infinity()) == Approx(0.0));
    REQUIRE(pulp::midi::equal_temperament_note_for_frequency(440.0) == 69);
    REQUIRE(pulp::midi::equal_temperament_note_for_frequency(261.6255653006) == 60);
    REQUIRE(pulp::midi::equal_temperament_note_for_frequency(
                440.0,
                std::numeric_limits<double>::quiet_NaN()) == -1);
}

TEST_CASE("equal temperament provider exposes query and status surface") {
    pulp::midi::EqualTemperamentTuningProvider tuning;

    const auto middle_c = tuning.note_to_frequency(60, 0);
    REQUIRE(middle_c.valid);
    REQUIRE(middle_c.frequency_hz == Approx(261.6255653006));
    REQUIRE(middle_c.retuning_semitones == Approx(0.0));
    REQUIRE(middle_c.retuning_ratio == Approx(1.0));
    REQUIRE_FALSE(middle_c.should_filter_note);

    const auto invalid = tuning.note_to_frequency(128, 0);
    REQUIRE_FALSE(invalid.valid);
    const auto invalid_frequency = tuning.frequency_to_note(-1.0, 0);
    REQUIRE_FALSE(invalid_frequency.valid);
    REQUIRE(invalid_frequency.midi_channel == pulp::midi::kUnknownMidiChannel);

    const auto infinity_frequency = tuning.frequency_to_note(
        std::numeric_limits<double>::infinity(),
        0);
    REQUIRE_FALSE(infinity_frequency.valid);
    REQUIRE(infinity_frequency.midi_channel == pulp::midi::kUnknownMidiChannel);

    const auto nan_frequency = tuning.frequency_to_note_and_channel(
        std::numeric_limits<double>::quiet_NaN());
    REQUIRE_FALSE(nan_frequency.valid);
    REQUIRE(nan_frequency.midi_channel == pulp::midi::kUnknownMidiChannel);

    const auto note = tuning.frequency_to_note(440.0, 3);
    REQUIRE(note.valid);
    REQUIRE(note.midi_note == 69);
    REQUIRE(note.midi_channel == 3);

    const auto generated = tuning.frequency_to_note_and_channel(440.0);
    REQUIRE(generated.valid);
    REQUIRE(generated.midi_note == 69);
    REQUIRE(generated.midi_channel == 0);

    const auto status = tuning.status();
    REQUIRE(status.scale_name == "12-TET");
    REQUIRE(status.period_ratio == Approx(2.0));
    REQUIRE(status.period_semitones == Approx(12.0));
}

#if PULP_HAS_MTS_ESP

TEST_CASE("MTS-ESP provider falls back to local 12-TET without an installed master") {
    pulp::midi::MtsEspTuningProvider tuning;

    const auto status = tuning.status();
    if (status.has_external_master)
        SKIP("MTS-ESP master is active; fallback 12-TET assertions are not deterministic");

    const auto a4 = tuning.note_to_frequency(69, 0);
    REQUIRE(a4.valid);
    REQUIRE(a4.frequency_hz == Approx(440.0));
    REQUIRE(a4.retuning_semitones == Approx(0.0));
    REQUIRE(a4.retuning_ratio == Approx(1.0));
    REQUIRE_FALSE(a4.should_filter_note);

    REQUIRE_FALSE(status.library_update_recommended);
    REQUIRE(status.period_ratio == Approx(2.0));
    REQUIRE(status.period_semitones == Approx(12.0));
    REQUIRE_FALSE(status.scale_name.empty());
    REQUIRE_FALSE(tuning.frequency_to_note(std::numeric_limits<double>::infinity(), 0).valid);
    REQUIRE_FALSE(tuning.frequency_to_note_and_channel(
                      std::numeric_limits<double>::quiet_NaN())
                      .valid);

    const auto note = tuning.frequency_to_note_and_channel(440.0);
    REQUIRE(note.valid);
    REQUIRE(note.midi_note == 69);
    REQUIRE(pulp::midi::is_valid_midi_channel(note.midi_channel));
}

TEST_CASE("MTS-ESP provider accepts MTS SysEx data") {
    pulp::midi::MtsEspTuningProvider tuning;

    const std::array<std::uint8_t, 21> scale_octave_one_byte = {
        0xf0, 0x7f, 0x7f, 0x08, 0x08, 0x00, 0x00, 0x7f,
        0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
        0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
        0xf7,
    };

    tuning.parse_midi_data(scale_octave_one_byte);

    const auto status = tuning.status();
    REQUIRE(status.has_local_mts_sysex);
    REQUIRE(status.map_size == 12);
    REQUIRE(status.map_start_key == 60);
}

#endif
