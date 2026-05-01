#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/bias.hpp>
#include <pulp/midi/midi_message_sequence.hpp>
#include <pulp/audio/subsection_reader.hpp>
#include <pulp/format/host_type.hpp>
#include <pulp/runtime/primes.hpp>
#include <pulp/runtime/expression.hpp>

using Catch::Matchers::WithinAbs;

// ── Bias ────────────────────────────────────────────────────────────────

TEST_CASE("Bias adds constant", "[signal][bias]") {
    pulp::signal::Bias bias;
    bias.set_bias(0.5f);
    REQUIRE_THAT(bias.process(0.0f), WithinAbs(0.5f, 1e-5));
    REQUIRE_THAT(bias.process(-0.5f), WithinAbs(0.0f, 1e-5));
}

TEST_CASE("Bias processes buffer", "[signal][bias]") {
    pulp::signal::Bias bias;
    bias.set_bias(1.0f);
    float buf[] = {0, 0.5f, -0.5f};
    bias.process(buf, 3);
    REQUIRE_THAT(buf[0], WithinAbs(1.0f, 1e-5));
    REQUIRE_THAT(buf[2], WithinAbs(0.5f, 1e-5));
}

TEST_CASE("Bias reports state and processes separate buffers",
          "[signal][bias][issue-645]") {
    pulp::signal::Bias bias;
    bias.set_sample_rate(48000.0f);
    bias.set_bias(-0.25f);

    REQUIRE_THAT(bias.bias(), WithinAbs(-0.25f, 1e-6f));

    const float input[] = {0.25f, -0.25f, 1.0f};
    float output[] = {9.0f, 9.0f, 9.0f};
    bias.process(input, output, 3);

    REQUIRE_THAT(output[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(output[1], WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(output[2], WithinAbs(0.75f, 1e-6f));

    bias.reset();
    REQUIRE_THAT(bias.process(0.5f), WithinAbs(0.25f, 1e-6f));
}

// ── MidiMessageSequence ─────────────────────────────────────────────────

TEST_CASE("MidiMessageSequence maintains order", "[midi][sequence]") {
    pulp::midi::MidiMessageSequence seq;
    seq.add_note_on(1.0, 0, 60, 100);
    seq.add_note_on(0.5, 0, 64, 80);
    seq.add_note_off(1.5, 0, 60);

    REQUIRE(seq.size() == 3);
    REQUIRE(seq[0].timestamp < seq[1].timestamp);
    REQUIRE(seq[1].timestamp < seq[2].timestamp);
}

TEST_CASE("MidiMessageSequence note pairing", "[midi][sequence]") {
    pulp::midi::MidiMessageSequence seq;
    seq.add_note_on(0.0, 0, 60, 100);
    seq.add_note_on(0.5, 0, 64, 80);
    seq.add_note_off(1.0, 0, 60);
    seq.add_note_off(1.5, 0, 64);

    auto off = seq.find_note_off(0);
    REQUIRE(off.has_value());
    REQUIRE(*off == 2);  // Third event is note-off for first note-on
}

TEST_CASE("MidiMessageSequence range query", "[midi][sequence]") {
    pulp::midi::MidiMessageSequence seq;
    seq.add_note_on(0.5, 0, 60, 100);
    seq.add_note_on(1.5, 0, 64, 80);
    seq.add_note_on(2.5, 0, 67, 90);

    auto events = seq.events_in_range(0.0, 2.0);
    REQUIRE(events.size() == 2);
}

TEST_CASE("MidiMessageSequence CC events", "[midi][sequence]") {
    pulp::midi::MidiMessageSequence seq;
    seq.add_cc(0.0, 0, 1, 64);  // Modulation
    REQUIRE(seq[0].is_cc());
    REQUIRE(seq[0].data1 == 1);
    REQUIRE(seq[0].data2 == 64);
}

TEST_CASE("MidiMessageSequence classifies aliases and masks helper fields", "[midi][sequence][issue-645]") {
    pulp::midi::MidiMessageSequence seq;
    seq.add_note_on(1.0, 31, 130, 255);
    seq.add_note_off(2.0, 17, 130);
    seq.add_cc(0.5, 31, 200, 255);

    REQUIRE(seq.size() == 3);
    REQUIRE(seq[0].is_cc());
    REQUIRE(seq[0].status == 0xBF);
    REQUIRE(seq[0].data1 == 72);
    REQUIRE(seq[0].data2 == 127);

    REQUIRE(seq[1].is_note_on());
    REQUIRE(seq[1].channel() == 15);
    REQUIRE(seq[1].note() == 2);
    REQUIRE(seq[1].velocity() == 127);

    REQUIRE(seq[2].is_note_off());
    REQUIRE(seq[2].channel() == 1);
    REQUIRE(seq[2].note() == 2);

    pulp::midi::TimestampedMidiEvent zero_velocity_note_on{0.0, 0x92, 60, 0, {}};
    REQUIRE_FALSE(zero_velocity_note_on.is_note_on());
    REQUIRE(zero_velocity_note_on.is_note_off());
    REQUIRE(zero_velocity_note_on.channel() == 2);

    pulp::midi::TimestampedMidiEvent sysex;
    sysex.status = 0xF0;
    sysex.sysex = {0xF0, 0x7D, 0x01, 0xF7};
    REQUIRE(sysex.is_sysex());
}

TEST_CASE("MidiMessageSequence note-off lookup handles invalid and channel-specific cases", "[midi][sequence][issue-645]") {
    pulp::midi::MidiMessageSequence seq;
    REQUIRE_FALSE(seq.find_note_off(-1).has_value());
    REQUIRE_FALSE(seq.find_note_off(0).has_value());

    seq.add_note_on(0.0, 0, 60, 100);
    seq.add_note_off(0.25, 1, 60);
    seq.add_note_on(0.5, 0, 64, 0);
    seq.add_note_off(1.0, 0, 60);
    seq.add_note_on(2.0, 0, 70, 100);

    auto off = seq.find_note_off(0);
    REQUIRE(off.has_value());
    REQUIRE(*off == 3);
    REQUIRE_FALSE(seq.find_note_off(1).has_value());
    REQUIRE_FALSE(seq.find_note_off(2).has_value());
    REQUIRE_FALSE(seq.find_note_off(4).has_value());
    REQUIRE_FALSE(seq.find_note_off(99).has_value());
}

TEST_CASE("MidiMessageSequence ranges offsets duration and clear", "[midi][sequence][issue-645]") {
    pulp::midi::MidiMessageSequence seq;
    REQUIRE(seq.duration() == 0.0);
    REQUIRE(seq.events().empty());
    REQUIRE(seq.begin() == seq.end());

    seq.add_note_on(0.25, 0, 60, 100);
    seq.add_note_on(0.75, 0, 64, 100);
    seq.add_note_off(1.25, 0, 60);
    seq.offset_timestamps(-0.25);

    REQUIRE(seq.duration() == 1.0);
    auto events = seq.events_in_range(0.0, 0.5);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0]->note() == 60);
    REQUIRE(seq.events_in_range(0.5, 1.0).size() == 1);
    REQUIRE(seq.events_in_range(1.0, 2.0).size() == 1);

    int iterated = 0;
    for (const auto& event : seq) {
        REQUIRE(event.timestamp >= 0.0);
        ++iterated;
    }
    REQUIRE(iterated == seq.size());

    seq.clear();
    REQUIRE(seq.size() == 0);
    REQUIRE(seq.duration() == 0.0);
    REQUIRE(seq.events().empty());
}

// ── AudioSubsectionReader ───────────────────────────────────────────────

TEST_CASE("AudioSubsectionReader basic", "[audio][subsection]") {
    pulp::audio::AudioFileData data;
    data.sample_rate = 44100;
    data.channels.resize(1);
    data.channels[0] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

    pulp::audio::AudioSubsectionReader reader(data, 3, 4);
    REQUIRE(reader.num_frames() == 4);
    REQUIRE(reader.sample(0, 0) == 3.0f);
    REQUIRE(reader.sample(0, 3) == 6.0f);
}

TEST_CASE("AudioSubsectionReader extract", "[audio][subsection]") {
    pulp::audio::AudioFileData data;
    data.sample_rate = 48000;
    data.channels.resize(2);
    data.channels[0] = {0, 1, 2, 3, 4};
    data.channels[1] = {10, 11, 12, 13, 14};

    pulp::audio::AudioSubsectionReader reader(data, 1, 3);
    auto extracted = reader.extract();
    REQUIRE(extracted.num_frames() == 3);
    REQUIRE(extracted.channels[0][0] == 1.0f);
    REQUIRE(extracted.channels[1][2] == 13.0f);
}

TEST_CASE("AudioSubsectionReader handles invalid and clamped reads",
          "[audio][subsection][issue-640]") {
    pulp::audio::AudioSubsectionReader invalid;
    REQUIRE_FALSE(invalid.is_valid());
    REQUIRE(invalid.num_frames() == 0);
    REQUIRE(invalid.num_channels() == 0);
    REQUIRE(invalid.sample_rate() == 0);
    REQUIRE(invalid.sample(0, 0) == 0.0f);
    REQUIRE(invalid.duration_seconds() == 0.0);

    float untouched[2] = {42.0f, 43.0f};
    invalid.read_frames(untouched, 0, 0, 2);
    REQUIRE(untouched[0] == 42.0f);
    REQUIRE(untouched[1] == 43.0f);
    REQUIRE(invalid.extract().channels.empty());

    pulp::audio::AudioFileData data;
    data.sample_rate = 48000;
    data.channels.resize(2);
    data.channels[0] = {0, 1, 2, 3, 4, 5};
    data.channels[1] = {10, 11, 12, 13, 14, 15};

    pulp::audio::AudioSubsectionReader reader(data, 4, 99);
    REQUIRE(reader.is_valid());
    REQUIRE(reader.num_frames() == 2);
    REQUIRE(reader.num_channels() == 2);
    REQUIRE(reader.sample_rate() == 48000);
    REQUIRE_THAT(reader.duration_seconds(), WithinAbs(2.0 / 48000.0, 1e-12));
    REQUIRE(reader.sample(0, 0) == 4.0f);
    REQUIRE(reader.sample(1, 1) == 15.0f);
    REQUIRE(reader.sample(2, 0) == 0.0f);
    REQUIRE(reader.sample(0, 2) == 0.0f);

    float copied[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    reader.read_frames(copied, 1, 1, 3);
    REQUIRE(copied[0] == 15.0f);
    REQUIRE(copied[1] == -1.0f);
    REQUIRE(copied[2] == -1.0f);
    REQUIRE(copied[3] == -1.0f);

    reader.read_frames(copied, 9, 0, 2);
    REQUIRE(copied[0] == 15.0f);
    REQUIRE(copied[1] == -1.0f);
}

TEST_CASE("AudioSubsectionReader start past end is an empty source view",
          "[audio][subsection][issue-640]") {
    pulp::audio::AudioFileData data;
    data.sample_rate = 44100;
    data.channels.resize(1);
    data.channels[0] = {1, 2, 3};

    pulp::audio::AudioSubsectionReader reader(data, 99, 4);
    REQUIRE_FALSE(reader.is_valid());
    REQUIRE(reader.num_frames() == 0);
    REQUIRE(reader.num_channels() == 1);
    REQUIRE(reader.sample_rate() == 44100);
    REQUIRE(reader.sample(0, 0) == 0.0f);

    auto extracted = reader.extract();
    REQUIRE(extracted.sample_rate == 44100);
    REQUIRE(extracted.channels.size() == 1);
    REQUIRE(extracted.channels[0].empty());
}

// ── PluginHostType ──────────────────────────────────────────────────────

TEST_CASE("HostType detect returns something", "[format][host]") {
    auto type = pulp::format::detect_host_type();
    // In test context, should be Unknown or Standalone
    auto name = pulp::format::host_type_name(type);
    REQUIRE_FALSE(name.empty());
}

TEST_CASE("HostType names", "[format][host]") {
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::LogicPro) == "Logic Pro");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::GarageBand) == "GarageBand");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::AbletonLive) == "Ableton Live");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::Reaper) == "REAPER");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::ProTools) == "Pro Tools");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::Cubase) == "Cubase");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::Nuendo) == "Nuendo");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::StudioOne) == "Studio One");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::FLStudio) == "FL Studio");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::Bitwig) == "Bitwig Studio");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::Maschine) == "Maschine");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::AudacityTenacity) == "Audacity/Tenacity");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::Ardour) == "Ardour");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::Standalone) == "Pulp Standalone");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::Other) == "Other");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::Unknown) == "Unknown");
    REQUIRE(pulp::format::host_type_name(static_cast<pulp::format::HostType>(999)) == "Unknown");
}

TEST_CASE("HostType capability policy exceptions", "[format][host]") {
    using pulp::format::HostType;

    REQUIRE_FALSE(pulp::format::host_supports_resize(HostType::GarageBand));
    REQUIRE_FALSE(pulp::format::host_supports_resize(HostType::ProTools));
    REQUIRE(pulp::format::host_supports_resize(HostType::LogicPro));
    REQUIRE(pulp::format::host_supports_resize(HostType::Unknown));

    REQUIRE_FALSE(pulp::format::host_supports_sidechain(HostType::GarageBand));
    REQUIRE_FALSE(pulp::format::host_supports_sidechain(HostType::AudacityTenacity));
    REQUIRE(pulp::format::host_supports_sidechain(HostType::ProTools));
    REQUIRE(pulp::format::host_supports_sidechain(HostType::Unknown));
}

// ── Primes ──────────────────────────────────────────────────────────────

TEST_CASE("is_prime known values", "[runtime][primes]") {
    REQUIRE(pulp::runtime::is_prime(2));
    REQUIRE(pulp::runtime::is_prime(3));
    REQUIRE_FALSE(pulp::runtime::is_prime(4));
    REQUIRE(pulp::runtime::is_prime(7));
    REQUIRE(pulp::runtime::is_prime(97));
    REQUIRE_FALSE(pulp::runtime::is_prime(100));
}

TEST_CASE("generate_prime", "[runtime][primes]") {
    auto p = pulp::runtime::generate_prime(16);
    REQUIRE(p > 0);
    REQUIRE(pulp::runtime::is_prime(p));
}

TEST_CASE("sieve_primes", "[runtime][primes]") {
    auto primes = pulp::runtime::sieve_primes(100);
    REQUIRE(primes.size() == 25);  // 25 primes below 100
    REQUIRE(primes[0] == 2);
    REQUIRE(primes.back() == 97);
}

// ── Expression evaluator ────────────────────────────────────────────────

TEST_CASE("Expression basic arithmetic", "[runtime][expression]") {
    REQUIRE_THAT(*pulp::runtime::evaluate("2 + 3"), WithinAbs(5.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("10 - 4"), WithinAbs(6.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("3 * 4"), WithinAbs(12.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("10 / 4"), WithinAbs(2.5, 1e-10));
}

TEST_CASE("Expression precedence", "[runtime][expression]") {
    REQUIRE_THAT(*pulp::runtime::evaluate("2 + 3 * 4"), WithinAbs(14.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("(2 + 3) * 4"), WithinAbs(20.0, 1e-10));
}

TEST_CASE("Expression power", "[runtime][expression]") {
    REQUIRE_THAT(*pulp::runtime::evaluate("2 ^ 10"), WithinAbs(1024.0, 1e-10));
}

TEST_CASE("Expression functions", "[runtime][expression]") {
    REQUIRE_THAT(*pulp::runtime::evaluate("sin(0)"), WithinAbs(0.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("cos(0)"), WithinAbs(1.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("sqrt(16)"), WithinAbs(4.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("abs(-5)"), WithinAbs(5.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("min(3, 7)"), WithinAbs(3.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("max(3, 7)"), WithinAbs(7.0, 1e-10));
}

TEST_CASE("Expression variables", "[runtime][expression]") {
    auto result = pulp::runtime::evaluate("x * 2 + 1", {{"x", 3.0}});
    REQUIRE(result.has_value());
    REQUIRE_THAT(*result, WithinAbs(7.0, 1e-10));
}

TEST_CASE("Expression constants", "[runtime][expression]") {
    REQUIRE_THAT(*pulp::runtime::evaluate("pi"), WithinAbs(3.14159265, 1e-5));
    REQUIRE_THAT(*pulp::runtime::evaluate("e"), WithinAbs(2.71828182, 1e-5));
}

TEST_CASE("Expression invalid returns nullopt", "[runtime][expression]") {
    REQUIRE_FALSE(pulp::runtime::evaluate("2 +").has_value());
    REQUIRE_FALSE(pulp::runtime::evaluate("unknown_var").has_value());
}

TEST_CASE("ExpressionEvaluator stateful", "[runtime][expression]") {
    pulp::runtime::ExpressionEvaluator eval;
    eval.set("freq", 440.0);
    eval.set("gain", -6.0);

    auto result = eval.evaluate("freq * 2");
    REQUIRE(result.has_value());
    REQUIRE_THAT(*result, WithinAbs(880.0, 1e-10));
}

TEST_CASE("Expression handles numeric edge syntax and safe division", "[runtime][expression][issue-641]") {
    REQUIRE_THAT(*pulp::runtime::evaluate("+1.5e2"), WithinAbs(150.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("6 / 0"), WithinAbs(0.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("2 +\n 3\t* 4"), WithinAbs(14.0, 1e-10));
}

TEST_CASE("Expression covers remaining built-in function branches", "[runtime][expression][issue-641]") {
    REQUIRE_THAT(*pulp::runtime::evaluate("tan(0)"), WithinAbs(0.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("log(e)"), WithinAbs(1.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("log10(1000)"), WithinAbs(3.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("exp(0)"), WithinAbs(1.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("floor(3.9)"), WithinAbs(3.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("ceil(3.1)"), WithinAbs(4.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("round(3.5)"), WithinAbs(4.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("pow(2, 8)"), WithinAbs(256.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("clamp(12, 10)"), WithinAbs(10.0, 1e-10));
    REQUIRE_THAT(*pulp::runtime::evaluate("clamp(-3, 10)"), WithinAbs(0.0, 1e-10));
}

TEST_CASE("Expression rejects malformed calls and trailing tokens", "[runtime][expression][issue-641]") {
    REQUIRE_FALSE(pulp::runtime::evaluate("sqrt(16").has_value());
    REQUIRE_FALSE(pulp::runtime::evaluate("unknown(1)").has_value());
    REQUIRE_FALSE(pulp::runtime::evaluate("min(1)").has_value());
    REQUIRE_FALSE(pulp::runtime::evaluate("1 2").has_value());
}

TEST_CASE("ExpressionEvaluator custom functions and clearing", "[runtime][expression][issue-641]") {
    pulp::runtime::ExpressionEvaluator eval;
    eval.set("x", 4.0);
    eval.register_function("double_it", [](double value) { return value * 2.0; });

    REQUIRE(eval.get("x").has_value());
    REQUIRE_THAT(*eval.evaluate("double_it(x) + 1"), WithinAbs(9.0, 1e-10));

    eval.clear_variables();
    REQUIRE_FALSE(eval.get("x").has_value());
    REQUIRE_FALSE(eval.evaluate("double_it(x)").has_value());
    REQUIRE_THAT(*eval.evaluate("double_it(5)"), WithinAbs(10.0, 1e-10));
}
