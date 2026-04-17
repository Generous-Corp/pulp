// PulpSynth test — validates the synth processes audio with MIDI input

#include "pulp_synth.hpp"
#include <pulp/format/headless.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

using namespace pulp::examples;
using namespace pulp::format;
using Catch::Matchers::WithinAbs;

TEST_CASE("PulpSynth produces audio on note-on", "[examples][synth]") {
    HeadlessHost host(create_pulp_synth);
    host.prepare(48000, 512, 0, 2);

    // Create buffers
    std::vector<float> out_l(512, 0), out_r(512, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<float> output(out_ptrs, 2, 512);
    pulp::audio::BufferView<const float> input(nullptr, 0, 512);

    pulp::midi::MidiBuffer midi_in, midi_out;
    midi_in.add(pulp::midi::MidiEvent::note_on(0, 60, 100));

    host.process(output, input, midi_in, midi_out);

    float sum_sq = 0;
    for (int i = 0; i < 512; ++i)
        sum_sq += out_l[i] * out_l[i];
    float rms = std::sqrt(sum_sq / 512.0f);
    REQUIRE(rms > 0.001f);
}

TEST_CASE("PulpSynth is silent without notes", "[examples][synth]") {
    HeadlessHost host(create_pulp_synth);
    host.prepare(48000, 512, 0, 2);

    std::vector<float> out_l(512, 0), out_r(512, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<float> output(out_ptrs, 2, 512);
    pulp::audio::BufferView<const float> input(nullptr, 0, 512);

    host.process(output, input);

    float sum_sq = 0;
    for (int i = 0; i < 512; ++i)
        sum_sq += out_l[i] * out_l[i];
    REQUIRE(sum_sq < 0.0001f);
}

TEST_CASE("PulpSynth descriptor", "[examples][synth]") {
    HeadlessHost host(create_pulp_synth);
    auto& desc = host.descriptor();
    REQUIRE(desc.name == "PulpSynth");
    REQUIRE(desc.category == PluginCategory::Instrument);
    REQUIRE(desc.accepts_midi == true);
}

TEST_CASE("PulpSynth state round-trip", "[examples][synth]") {
    HeadlessHost host(create_pulp_synth);
    host.prepare(48000, 512, 0, 2);

    host.state().set_value(kFilterCutoff, 2000.0f);
    auto data = host.save_state();
    REQUIRE(!data.empty());

    host.state().set_value(kFilterCutoff, 5000.0f);
    host.load_state(data);
    REQUIRE_THAT(host.state().get_value(kFilterCutoff), WithinAbs(2000.0, 0.1));
}

// ── Deeper golden cases (#356 golden breadth) ─────────────────────────

namespace {

float rms_of(const std::vector<float>& v) {
    float sum = 0.0f;
    for (float s : v) sum += s * s;
    return std::sqrt(sum / static_cast<float>(v.size()));
}

std::vector<float> render_with_midi(
    HeadlessHost& host, pulp::midi::MidiBuffer& midi_in, int n_samples)
{
    std::vector<float> out_l(static_cast<std::size_t>(n_samples), 0.0f);
    std::vector<float> out_r(static_cast<std::size_t>(n_samples), 0.0f);
    float* op[] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<float> ov(op, 2, n_samples);
    pulp::audio::BufferView<const float> iv(nullptr, 0, n_samples);
    pulp::midi::MidiBuffer midi_out;
    host.process(ov, iv, midi_in, midi_out);
    return out_l;
}

}  // namespace

// Master gain at -60 dB takes the note-on output well under the
// audible floor. Guards against a future refactor that forgets to
// apply the master gain to one of the audio paths.
//
// The master-gain smoother needs a few thousand samples to settle
// from the default -6 dB baseline to -60 dB, so we render 8192
// samples (~170 ms at 48 kHz) and measure only the tail of the
// output where the smoother has arrived.
TEST_CASE("PulpSynth golden: master gain at -60 dB effectively silences note",
          "[examples][synth][golden][issue-356]") {
    constexpr int n = 8192;

    HeadlessHost loud(create_pulp_synth);
    loud.prepare(48000, n, 0, 2);
    pulp::midi::MidiBuffer m1;
    m1.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
    auto loud_out = render_with_midi(loud, m1, n);

    HeadlessHost quiet(create_pulp_synth);
    quiet.prepare(48000, n, 0, 2);
    quiet.state().set_value(kMasterGain, -60.0f);
    pulp::midi::MidiBuffer m2;
    m2.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
    auto quiet_out = render_with_midi(quiet, m2, n);

    // Measure only the final quarter — the gain smoother has settled
    // and the amp envelope is in the sustain plateau by then.
    std::vector<float> loud_tail(loud_out.end() - n / 4, loud_out.end());
    std::vector<float> quiet_tail(quiet_out.end() - n / 4, quiet_out.end());
    float loud_rms = rms_of(loud_tail);
    float quiet_rms = rms_of(quiet_tail);

    REQUIRE(loud_rms > 0.01f);
    // -60 dB ≈ 0.001× linear; tolerate some smoother residue but fail
    // hard if the gain isn't materially knocking the signal down.
    REQUIRE(quiet_rms < loud_rms * 0.05f);
}

// Three simultaneous notes must produce more output energy than one.
// Polyphony regression guard — catches a future voice-allocator change
// that silently clamps to a single voice.
TEST_CASE("PulpSynth golden: polyphony — 3 notes louder than 1 note",
          "[examples][synth][golden][issue-356]") {
    HeadlessHost mono(create_pulp_synth);
    mono.prepare(48000, 2048, 0, 2);
    pulp::midi::MidiBuffer m1;
    m1.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
    auto mono_out = render_with_midi(mono, m1, 2048);

    HeadlessHost poly(create_pulp_synth);
    poly.prepare(48000, 2048, 0, 2);
    pulp::midi::MidiBuffer m3;
    m3.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
    m3.add(pulp::midi::MidiEvent::note_on(0, 64, 100));
    m3.add(pulp::midi::MidiEvent::note_on(0, 67, 100));
    auto poly_out = render_with_midi(poly, m3, 2048);

    float mono_rms = rms_of(mono_out);
    float poly_rms = rms_of(poly_out);
    REQUIRE(mono_rms > 0.001f);
    REQUIRE(poly_rms > mono_rms * 1.3f);
}

// Identical MIDI + identical state ⇒ bit-identical output. This is
// the determinism contract the entire test matrix depends on.
TEST_CASE("PulpSynth golden: same MIDI + state ⇒ bit-identical output",
          "[examples][synth][golden][issue-356]") {
    HeadlessHost h1(create_pulp_synth);
    h1.prepare(48000, 1024, 0, 2);
    pulp::midi::MidiBuffer m1;
    m1.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
    auto out1 = render_with_midi(h1, m1, 1024);

    HeadlessHost h2(create_pulp_synth);
    h2.prepare(48000, 1024, 0, 2);
    pulp::midi::MidiBuffer m2;
    m2.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
    auto out2 = render_with_midi(h2, m2, 1024);

    for (std::size_t i = 0; i < out1.size(); ++i) {
        REQUIRE(out1[i] == out2[i]);
    }
}
