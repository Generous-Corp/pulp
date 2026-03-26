#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/midi/buffer.hpp>
#include <cmath>
#include <numeric>

// Golden-file style tests for PulpGain audio processing
// These verify the complete audio pipeline produces correct output
// for known inputs and parameter settings.

#include "pulp_gain.hpp"
#include "pulp_tone.hpp"

// PulpEffect golden tests are in test_golden_effect.cpp to avoid
// unscoped enum collision (pulp_gain.hpp and pulp_effect.hpp both define kBypass)

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// Helper: generate a sine wave buffer
static pulp::audio::Buffer<float> make_sine(int channels, int samples,
                                             float freq = 440.0f,
                                             double sample_rate = 48000.0) {
    pulp::audio::Buffer<float> buf(channels, samples);
    for (int ch = 0; ch < channels; ++ch) {
        for (int i = 0; i < samples; ++i) {
            buf.channel(ch)[i] = std::sin(2.0f * M_PI * freq * i / sample_rate);
        }
    }
    return buf;
}

// Helper: compute RMS of a buffer channel
static float rms(const pulp::audio::Buffer<float>& buf, int ch) {
    float sum = 0.0f;
    auto span = buf.channel(ch);
    for (std::size_t i = 0; i < buf.num_samples(); ++i) {
        sum += span[i] * span[i];
    }
    return std::sqrt(sum / static_cast<float>(buf.num_samples()));
}

// Helper: process through headless host
static void process_headless(pulp::format::HeadlessHost& host,
                             const pulp::audio::Buffer<float>& in,
                             pulp::audio::Buffer<float>& out) {
    const float* in_ptrs[2] = {in.channel(0).data(),
                                in.num_channels() > 1 ? in.channel(1).data() : in.channel(0).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, in.num_channels(), in.num_samples());
    auto out_view = out.view();
    host.process(out_view, in_view);
}

TEST_CASE("Golden: PulpGain unity gain preserves signal", "[golden][pulpgain]") {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);
    host.prepare(48000.0, 1024);

    auto in = make_sine(2, 1024);
    pulp::audio::Buffer<float> out(2, 1024);
    process_headless(host, in, out);

    // At 0 dB (default), output should exactly match input
    for (std::size_t i = 0; i < 1024; ++i) {
        REQUIRE_THAT(out.channel(0)[i], WithinAbs(in.channel(0)[i], 1e-6));
    }
}

TEST_CASE("Golden: PulpGain +6dB doubles amplitude", "[golden][pulpgain]") {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);
    host.prepare(48000.0, 1024);
    host.state().set_value(1, 6.0f); // Input Gain = +6 dB

    auto in = make_sine(2, 1024);
    pulp::audio::Buffer<float> out(2, 1024);
    process_headless(host, in, out);

    float gain_linear = std::pow(10.0f, 6.0f / 20.0f); // ≈ 1.995
    float in_rms = rms(in, 0);
    float out_rms = rms(out, 0);

    REQUIRE_THAT(static_cast<double>(out_rms / in_rms),
                 WithinRel(static_cast<double>(gain_linear), 0.01));
}

TEST_CASE("Golden: PulpGain -inf dB silences signal", "[golden][pulpgain]") {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);
    host.prepare(48000.0, 1024);
    host.state().set_value(1, -60.0f); // Input Gain = -60 dB
    host.state().set_value(2, -60.0f); // Output Gain = -60 dB

    auto in = make_sine(2, 1024);
    pulp::audio::Buffer<float> out(2, 1024);
    process_headless(host, in, out);

    float out_rms = rms(out, 0);
    // -120 dB combined = essentially silent
    REQUIRE(out_rms < 0.001f);
}

TEST_CASE("Golden: PulpGain bypass passes through unmodified", "[golden][pulpgain]") {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);
    host.prepare(48000.0, 1024);
    host.state().set_value(1, 12.0f);  // Input Gain = +12 dB (should be ignored)
    host.state().set_value(3, 1.0f);   // Bypass = ON

    auto in = make_sine(2, 1024);
    pulp::audio::Buffer<float> out(2, 1024);
    process_headless(host, in, out);

    // Bypass: output == input, regardless of gain settings
    for (std::size_t i = 0; i < 1024; ++i) {
        REQUIRE_THAT(out.channel(0)[i], WithinAbs(in.channel(0)[i], 1e-6));
    }
}

TEST_CASE("Golden: PulpGain stereo channels are independent", "[golden][pulpgain]") {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);
    host.prepare(48000.0, 512);

    // Different content per channel
    pulp::audio::Buffer<float> in(2, 512);
    for (std::size_t i = 0; i < 512; ++i) {
        in.channel(0)[i] = std::sin(2.0f * M_PI * 440.0f * i / 48000.0f);
        in.channel(1)[i] = std::sin(2.0f * M_PI * 880.0f * i / 48000.0f);
    }

    pulp::audio::Buffer<float> out(2, 512);
    process_headless(host, in, out);

    // At unity gain, channels should match input independently
    float rms_in_L = rms(in, 0), rms_in_R = rms(in, 1);
    float rms_out_L = rms(out, 0), rms_out_R = rms(out, 1);

    REQUIRE_THAT(static_cast<double>(rms_out_L), WithinRel(static_cast<double>(rms_in_L), 0.001));
    REQUIRE_THAT(static_cast<double>(rms_out_R), WithinRel(static_cast<double>(rms_in_R), 0.001));

    // Channels should not be identical (different frequencies)
    REQUIRE(std::abs(rms_in_L - rms_in_R) < 0.1f); // Both are ~0.707 RMS
    // But samples differ
    REQUIRE(std::abs(out.channel(0)[10] - out.channel(1)[10]) > 0.01f);
}

TEST_CASE("Golden: PulpGain state round-trip preserves audio", "[golden][pulpgain]") {
    // Set specific gains, save state, load into fresh instance, verify same output
    pulp::format::HeadlessHost host1(pulp::examples::create_pulp_gain);
    host1.prepare(48000.0, 512);
    host1.state().set_value(1, -6.0f);  // Input Gain = -6 dB
    host1.state().set_value(2, 3.0f);   // Output Gain = +3 dB

    auto in = make_sine(2, 512);
    pulp::audio::Buffer<float> out1(2, 512);
    process_headless(host1, in, out1);

    auto saved_state = host1.save_state();

    // Fresh instance with restored state
    pulp::format::HeadlessHost host2(pulp::examples::create_pulp_gain);
    host2.prepare(48000.0, 512);
    REQUIRE(host2.load_state(saved_state));

    pulp::audio::Buffer<float> out2(2, 512);
    process_headless(host2, in, out2);

    // Output should be bit-identical
    for (std::size_t i = 0; i < 512; ++i) {
        REQUIRE(out1.channel(0)[i] == out2.channel(0)[i]);
    }
}

// ── PulpTone (instrument) golden tests ──────────────────────────────────

TEST_CASE("Golden: PulpTone produces sound with MIDI note", "[golden][pulptone]") {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_tone);
    host.prepare(48000.0, 1024, 0, 2); // instrument: 0 inputs, 2 outputs

    pulp::midi::MidiBuffer midi_in;
    midi_in.add(pulp::midi::MidiEvent::note_on(0, 60, 100)); // Middle C
    pulp::midi::MidiBuffer midi_out;

    pulp::audio::Buffer<float> in(0, 1024);  // no audio input for instrument
    pulp::audio::Buffer<float> out(2, 1024);

    const float* in_ptrs[] = {nullptr};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 0, 1024);
    auto out_view = out.view();

    host.process(out_view, in_view, midi_in, midi_out);

    // Should produce non-zero output when a note is playing
    float max_sample = 0.0f;
    for (std::size_t i = 0; i < 1024; ++i) {
        max_sample = std::max(max_sample, std::abs(out.channel(0)[i]));
    }
    REQUIRE(max_sample > 0.01f); // audible output
}

TEST_CASE("Golden: PulpTone is silent without MIDI", "[golden][pulptone]") {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_tone);
    host.prepare(48000.0, 1024, 0, 2);

    pulp::midi::MidiBuffer midi_in; // empty
    pulp::midi::MidiBuffer midi_out;

    const float* in_ptrs[] = {nullptr};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 0, 1024);
    pulp::audio::Buffer<float> out(2, 1024);
    auto out_view = out.view();

    host.process(out_view, in_view, midi_in, midi_out);

    // No notes playing: output should be silence
    float max_sample = 0.0f;
    for (std::size_t i = 0; i < 1024; ++i) {
        max_sample = std::max(max_sample, std::abs(out.channel(0)[i]));
    }
    REQUIRE(max_sample < 0.001f);
}

