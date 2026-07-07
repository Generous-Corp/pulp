#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/tuning.hpp>

#if PULP_HAS_MTS_ESP
#include <pulp/midi/mts_esp_tuning.hpp>
#endif

#if PULP_HAS_SCALA_TUNING
#include <pulp/midi/scala_tuning.hpp>
#endif

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

using Catch::Approx;

namespace {

#if PULP_HAS_SCALA_TUNING

constexpr const char* kTwentyFourEdoScl = R"SCL(! 24-EDO
24 equal divisions of the octave
24
50.0
100.0
150.0
200.0
250.0
300.0
350.0
400.0
450.0
500.0
550.0
600.0
650.0
700.0
750.0
800.0
850.0
900.0
950.0
1000.0
1050.0
1100.0
1150.0
2/1
)SCL";

constexpr const char* kUnmappedMiddleKbm = R"KBM(! three-key test mapping
3
60
62
60
60
261.6255653006
3
0
x
2
)KBM";

#endif

}  // namespace

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
    REQUIRE(tuning.has_session_tuning() ==
            (status.has_external_master || status.has_local_mts_sysex));
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
    REQUIRE(tuning.has_session_tuning());
    REQUIRE(status.map_size == 12);
    REQUIRE(status.map_start_key == 60);
}

#endif

#if PULP_HAS_SCALA_TUNING

TEST_CASE("Scala tuning provider loads SCL data and maps notes") {
    pulp::midi::ScalaTuningProvider tuning;
    std::string error;
    REQUIRE(tuning.load_scl_data(kTwentyFourEdoScl, &error));
    REQUIRE(error.empty());

    const auto status = tuning.status();
    REQUIRE(status.has_local_file_tuning);
    REQUIRE_FALSE(status.has_keyboard_mapping);
    REQUIRE(status.scale_name == "24 equal divisions of the octave");
    REQUIRE(status.period_ratio == Approx(2.0));
    REQUIRE(status.period_semitones == Approx(12.0));

    const auto c4 = tuning.note_to_frequency(60, 0);
    REQUIRE(c4.valid);
    REQUIRE(c4.frequency_hz == Approx(261.6255653006));
    REQUIRE_FALSE(c4.should_filter_note);

    const auto c_sharp_quarter = tuning.note_to_frequency(61, 0);
    REQUIRE(c_sharp_quarter.valid);
    REQUIRE(c_sharp_quarter.frequency_hz ==
            Approx(261.6255653006 * std::pow(2.0, 50.0 / 1200.0)));
    REQUIRE(c_sharp_quarter.retuning_semitones == Approx(-0.5));
    REQUIRE(c_sharp_quarter.retuning_ratio == Approx(std::pow(2.0, -0.5 / 12.0)));

    const auto nearest = tuning.frequency_to_note(c_sharp_quarter.frequency_hz, 4);
    REQUIRE(nearest.valid);
    REQUIRE(nearest.midi_note == 61);
    REQUIRE(nearest.midi_channel == 4);
}

TEST_CASE("Scala tuning provider loads KBM data and reports unmapped notes") {
    pulp::midi::ScalaTuningProvider tuning;
    std::string error;
    REQUIRE(tuning.load_kbm_data(kUnmappedMiddleKbm, &error));
    REQUIRE(error.empty());

    const auto status = tuning.status();
    REQUIRE(status.has_local_file_tuning);
    REQUIRE(status.has_keyboard_mapping);
    REQUIRE(status.map_size == 3);
    REQUIRE(status.map_start_key == 60);
    REQUIRE(status.reference_key == 60);

    const auto mapped = tuning.note_to_frequency(60, 0);
    REQUIRE(mapped.valid);
    REQUIRE_FALSE(mapped.should_filter_note);

    const auto filtered = tuning.note_to_frequency(61, 0);
    REQUIRE(filtered.valid);
    REQUIRE(filtered.should_filter_note);

    const auto nearest = tuning.frequency_to_note(filtered.frequency_hz, 2);
    REQUIRE(nearest.valid);
    REQUIRE(nearest.midi_note != 61);
    REQUIRE(nearest.midi_channel == 2);
}

TEST_CASE("Scala tuning provider failed load leaves prior tuning intact") {
    pulp::midi::ScalaTuningProvider tuning;
    std::string error;
    REQUIRE(tuning.load_scl_data(kTwentyFourEdoScl, &error));
    const auto before = tuning.note_to_frequency(61, 0);
    REQUIRE(before.valid);

    REQUIRE_FALSE(tuning.load_scl_data("not a valid scl", &error));
    REQUIRE_FALSE(error.empty());
    const auto after = tuning.note_to_frequency(61, 0);
    REQUIRE(after.valid);
    REQUIRE(after.frequency_hz == Approx(before.frequency_hz));
}

TEST_CASE("Scala tuning provider loads SCL and KBM files together") {
    const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path() /
        ("pulp-scala-tuning-provider-test-" + std::to_string(unique_suffix));
    std::filesystem::create_directories(dir);
    const auto scl = dir / "24edo.scl";
    const auto kbm = dir / "unmapped.kbm";
    {
        std::ofstream out(scl);
        out << kTwentyFourEdoScl;
    }
    {
        std::ofstream out(kbm);
        out << kUnmappedMiddleKbm;
    }

    pulp::midi::ScalaTuningProvider tuning;
    std::string error;
    REQUIRE(tuning.load_scl_kbm_files(scl, kbm, &error));
    REQUIRE(error.empty());
    REQUIRE(tuning.status().has_local_file_tuning);
    REQUIRE(tuning.status().has_keyboard_mapping);
    REQUIRE(tuning.note_to_frequency(61, 0).should_filter_note);

    std::filesystem::remove_all(dir);
}

#endif  // PULP_HAS_SCALA_TUNING

#if PULP_HAS_MTS_ESP && PULP_HAS_SCALA_TUNING

TEST_CASE("MTS-ESP fallback provider uses Scala tuning until an MTS session is active") {
    auto local = std::make_unique<pulp::midi::ScalaTuningProvider>();
    std::string error;
    REQUIRE(local->load_scl_data(kTwentyFourEdoScl, &error));

    pulp::midi::MtsEspFallbackTuningProvider tuning(std::move(local));
    if (tuning.using_mts_session())
        SKIP("MTS-ESP master or local MTS SysEx is active; local-fallback assertions are not deterministic");

    const auto local_note = tuning.note_to_frequency(61, 0);
    REQUIRE(local_note.valid);
    REQUIRE(local_note.frequency_hz ==
            Approx(261.6255653006 * std::pow(2.0, 50.0 / 1200.0)));
    REQUIRE(tuning.status().has_local_file_tuning);

    const std::array<std::uint8_t, 21> scale_octave_one_byte = {
        0xf0, 0x7f, 0x7f, 0x08, 0x08, 0x00, 0x00, 0x7f,
        0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
        0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
        0xf7,
    };
    tuning.parse_midi_data(scale_octave_one_byte);

    REQUIRE(tuning.using_mts_session());
    REQUIRE(tuning.status().has_local_mts_sysex);
    const auto mts_note = tuning.note_to_frequency(61, 0);
    REQUIRE(mts_note.valid);
    REQUIRE(mts_note.frequency_hz == Approx(pulp::midi::equal_temperament_frequency(61)));
}

#endif  // PULP_HAS_MTS_ESP && PULP_HAS_SCALA_TUNING
