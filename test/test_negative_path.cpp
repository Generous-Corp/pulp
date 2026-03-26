#include <catch2/catch_test_macros.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/midi/buffer.hpp>
#include <cmath>
#include <limits>

#include "pulp_gain.hpp"
#include "pulp_tone.hpp"

using namespace pulp;

// Helper: create const buffer view from Buffer
static void process_buf(format::HeadlessHost& host,
                        audio::Buffer<float>& in,
                        audio::Buffer<float>& out) {
    const float* in_ptrs[2] = {in.channel(0).data(),
                                in.num_channels() > 1 ? in.channel(1).data() : in.channel(0).data()};
    audio::BufferView<const float> in_view(in_ptrs, in.num_channels(), in.num_samples());
    auto out_view = out.view();
    host.process(out_view, in_view);
}

// ── NaN/Inf input handling ──────────────────────────────────────────────

TEST_CASE("Negative: PulpGain handles NaN input without crashing", "[negative][gain]") {
    format::HeadlessHost host(examples::create_pulp_gain);
    host.prepare(48000.0, 256);

    audio::Buffer<float> in(2, 256);
    audio::Buffer<float> out(2, 256);
    for (std::size_t i = 0; i < 256; ++i) {
        in.channel(0)[i] = std::numeric_limits<float>::quiet_NaN();
        in.channel(1)[i] = std::numeric_limits<float>::quiet_NaN();
    }

    process_buf(host, in, out);
    SUCCEED("NaN input processed without crash");
}

TEST_CASE("Negative: PulpGain handles Inf input without crashing", "[negative][gain]") {
    format::HeadlessHost host(examples::create_pulp_gain);
    host.prepare(48000.0, 256);

    audio::Buffer<float> in(2, 256);
    audio::Buffer<float> out(2, 256);
    for (std::size_t i = 0; i < 256; ++i) {
        in.channel(0)[i] = std::numeric_limits<float>::infinity();
        in.channel(1)[i] = -std::numeric_limits<float>::infinity();
    }

    process_buf(host, in, out);
    SUCCEED("Inf input processed without crash");
}

// ── Extreme sample rates ────────────────────────────────────────────────

TEST_CASE("Negative: PulpGain survives very low sample rate", "[negative][gain]") {
    format::HeadlessHost host(examples::create_pulp_gain);
    host.prepare(100.0, 32);

    audio::Buffer<float> in(2, 32);
    audio::Buffer<float> out(2, 32);
    for (std::size_t i = 0; i < 32; ++i) {
        in.channel(0)[i] = std::sin(2.0f * static_cast<float>(M_PI) * 10.0f * static_cast<float>(i) / 100.0f);
        in.channel(1)[i] = in.channel(0)[i];
    }

    process_buf(host, in, out);
    for (std::size_t i = 0; i < 32; ++i) {
        REQUIRE(std::isfinite(out.channel(0)[i]));
    }
}

TEST_CASE("Negative: PulpGain survives very high sample rate", "[negative][gain]") {
    format::HeadlessHost host(examples::create_pulp_gain);
    host.prepare(384000.0, 1024);

    audio::Buffer<float> in(2, 1024);
    audio::Buffer<float> out(2, 1024);
    for (std::size_t i = 0; i < 1024; ++i) {
        in.channel(0)[i] = std::sin(2.0f * static_cast<float>(M_PI) * 1000.0f * static_cast<float>(i) / 384000.0f);
        in.channel(1)[i] = in.channel(0)[i];
    }

    process_buf(host, in, out);
    for (std::size_t i = 0; i < 1024; ++i) {
        REQUIRE(std::isfinite(out.channel(0)[i]));
    }
}

// ── Extreme buffer sizes ────────────────────────────────────────────────

TEST_CASE("Negative: PulpGain handles single-sample buffer", "[negative][gain]") {
    format::HeadlessHost host(examples::create_pulp_gain);
    host.prepare(48000.0, 1);

    audio::Buffer<float> in(2, 1);
    audio::Buffer<float> out(2, 1);
    in.channel(0)[0] = 0.5f;
    in.channel(1)[0] = -0.5f;

    process_buf(host, in, out);
    REQUIRE(std::isfinite(out.channel(0)[0]));
    REQUIRE(std::isfinite(out.channel(1)[0]));
}

TEST_CASE("Negative: PulpGain handles large buffer", "[negative][gain]") {
    format::HeadlessHost host(examples::create_pulp_gain);
    host.prepare(48000.0, 16384);

    audio::Buffer<float> in(2, 16384);
    audio::Buffer<float> out(2, 16384);
    for (std::size_t i = 0; i < 16384; ++i) {
        in.channel(0)[i] = std::sin(2.0f * static_cast<float>(M_PI) * 440.0f * static_cast<float>(i) / 48000.0f);
        in.channel(1)[i] = in.channel(0)[i];
    }

    process_buf(host, in, out);
    for (std::size_t i = 0; i < 16384; ++i) {
        REQUIRE(std::isfinite(out.channel(0)[i]));
    }
}

// ── Instrument at extreme sample rate ───────────────────────────────────

TEST_CASE("Negative: PulpTone handles note at extreme sample rate", "[negative][tone]") {
    format::HeadlessHost host(examples::create_pulp_tone);
    host.prepare(384000.0, 256, 0, 2);

    midi::MidiBuffer midi_in;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 100));
    midi::MidiBuffer midi_out;

    const float* in_ptrs[] = {nullptr};
    audio::BufferView<const float> in_view(in_ptrs, 0, 256);
    audio::Buffer<float> out(2, 256);
    auto out_view = out.view();

    host.process(out_view, in_view, midi_in, midi_out);
    for (std::size_t i = 0; i < 256; ++i) {
        REQUIRE(std::isfinite(out.channel(0)[i]));
    }
}

// ── Rapid prepare/release cycling ───────────────────────────────────────

TEST_CASE("Negative: PulpGain survives rapid prepare/release cycling", "[negative][gain]") {
    for (int cycle = 0; cycle < 100; ++cycle) {
        format::HeadlessHost host(examples::create_pulp_gain);
        host.prepare(44100.0 + cycle * 100, 64 + cycle);

        audio::Buffer<float> in(2, 64);
        audio::Buffer<float> out(2, 64);
        for (std::size_t i = 0; i < 64; ++i) {
            in.channel(0)[i] = 0.1f;
            in.channel(1)[i] = 0.1f;
        }

        process_buf(host, in, out);
        host.release();
    }
    SUCCEED("100 prepare/release cycles completed");
}
