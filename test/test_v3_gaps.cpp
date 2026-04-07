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

// ── PluginHostType ──────────────────────────────────────────────────────

TEST_CASE("HostType detect returns something", "[format][host]") {
    auto type = pulp::format::detect_host_type();
    // In test context, should be Unknown or Standalone
    auto name = pulp::format::host_type_name(type);
    REQUIRE_FALSE(name.empty());
}

TEST_CASE("HostType names", "[format][host]") {
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::LogicPro) == "Logic Pro");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::Reaper) == "REAPER");
    REQUIRE(pulp::format::host_type_name(pulp::format::HostType::Unknown) == "Unknown");
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
