#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "pulp_tone.hpp"
#include <cmath>

using namespace pulp;
using namespace pulp::examples;
using Catch::Matchers::WithinAbs;

struct ToneFixture {
    state::StateStore store;
    std::unique_ptr<format::Processor> processor;

    ToneFixture() {
        processor = create_pulp_tone();
        processor->set_state_store(&store);
        processor->define_parameters(store);
        processor->prepare({48000.0, 512, 0, 2});
    }

    audio::Buffer<float> render(midi::MidiBuffer& midi_in, int samples = 512) {
        audio::Buffer<float> out(2, samples);
        auto out_view = out.view();
        audio::BufferView<const float> empty_in;
        midi::MidiBuffer midi_out;
        format::ProcessContext ctx;
        ctx.sample_rate = 48000.0;
        ctx.num_samples = samples;
        processor->process(out_view, empty_in, midi_in, midi_out, ctx);
        return out;
    }
};

TEST_CASE("PulpTone descriptor", "[pulptone]") {
    PulpToneProcessor proc;
    auto desc = proc.descriptor();
    REQUIRE(desc.name == "PulpTone");
    REQUIRE(desc.category == format::PluginCategory::Instrument);
    REQUIRE(desc.accepts_midi);
    REQUIRE(desc.default_output_channels() == 2);
    REQUIRE(desc.default_input_channels() == 0);
}

TEST_CASE("PulpTone parameters", "[pulptone]") {
    ToneFixture fx;
    REQUIRE(fx.store.param_count() == 4);
    REQUIRE(fx.store.info(kWaveform) != nullptr);
    REQUIRE(fx.store.info(kVolume) != nullptr);
    REQUIRE(fx.store.info(kAttack) != nullptr);
    REQUIRE(fx.store.info(kRelease) != nullptr);
}

TEST_CASE("PulpTone silence without notes", "[pulptone]") {
    ToneFixture fx;
    midi::MidiBuffer empty_midi;
    auto out = fx.render(empty_midi);

    // Should be silent — no notes playing
    float max_val = 0.0f;
    for (std::size_t i = 0; i < out.num_samples(); ++i) {
        max_val = std::max(max_val, std::abs(out.channel(0)[i]));
    }
    REQUIRE(max_val < 0.001f);
}

TEST_CASE("PulpTone produces sound on note-on", "[pulptone]") {
    ToneFixture fx;

    midi::MidiBuffer midi_in;
    auto note = midi::MidiEvent::note_on(0, 69, 100); // A4 = 440 Hz
    note.sample_offset = 0;
    midi_in.add(note);

    auto out = fx.render(midi_in);

    // Should have non-zero output (attack starts immediately)
    float max_val = 0.0f;
    for (std::size_t i = 0; i < out.num_samples(); ++i) {
        max_val = std::max(max_val, std::abs(out.channel(0)[i]));
    }
    REQUIRE(max_val > 0.01f);
}

TEST_CASE("PulpTone prepare resets active voices before retrigger",
          "[pulptone][midi][lifecycle]") {
    ToneFixture fx;
    fx.store.set_value(kWaveform, 1.0f); // Saw
    fx.store.set_value(kVolume, 0.0f);   // 0 dB
    fx.store.set_value(kAttack, 1.0f);   // 1ms attack (fast)

    auto peak = [](const audio::Buffer<float>& buffer) {
        float result = 0.0f;
        for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
            for (std::size_t i = 0; i < buffer.num_samples(); ++i) {
                result = std::max(result, std::abs(buffer.channel(ch)[i]));
            }
        }
        return result;
    };

    midi::MidiBuffer initial_note;
    initial_note.add(midi::MidiEvent::note_on(0, 69, 127));
    const auto sounding = fx.render(initial_note);
    REQUIRE(peak(sounding) > 0.01f);

    fx.processor->prepare({48000.0, 512, 0, 2});

    midi::MidiBuffer empty_midi;
    const auto after_prepare = fx.render(empty_midi);
    for (std::size_t i = 0; i < after_prepare.num_samples(); ++i) {
        REQUIRE(after_prepare.channel(0)[i] == 0.0f);
        REQUIRE(after_prepare.channel(1)[i] == 0.0f);
    }

    midi::MidiBuffer retrigger;
    retrigger.add(midi::MidiEvent::note_on(0, 69, 127));
    const auto after_retrigger = fx.render(retrigger);
    REQUIRE(peak(after_retrigger) > 0.01f);
}

TEST_CASE("PulpTone A4 produces ~440 Hz sine", "[pulptone]") {
    ToneFixture fx;
    fx.store.set_value(kWaveform, 0.0f); // Sine
    fx.store.set_value(kVolume, 0.0f);   // 0 dB
    fx.store.set_value(kAttack, 1.0f);   // 1ms attack (fast)

    midi::MidiBuffer midi_in;
    auto note = midi::MidiEvent::note_on(0, 69, 127); // A4
    note.sample_offset = 0;
    midi_in.add(note);

    // Render enough samples for several cycles of 440 Hz
    // At 48000 Hz, one cycle of 440 Hz = ~109 samples
    auto out = fx.render(midi_in, 4800); // 100ms

    // Count zero crossings in the last half (after attack settles)
    int crossings = 0;
    for (std::size_t i = 2401; i < 4799; ++i) {
        if ((out.channel(0)[i] >= 0) != (out.channel(0)[i + 1] >= 0)) {
            ++crossings;
        }
    }

    // 440 Hz in 50ms = 22 cycles = ~44 zero crossings (±5 tolerance)
    REQUIRE(crossings >= 38);
    REQUIRE(crossings <= 50);
}

TEST_CASE("PulpTone note-off triggers release", "[pulptone]") {
    ToneFixture fx;
    fx.store.set_value(kAttack, 1.0f);    // 1ms
    fx.store.set_value(kRelease, 100.0f); // 100ms

    // First buffer: note on
    midi::MidiBuffer midi_on;
    midi_on.add(midi::MidiEvent::note_on(0, 60, 100));
    fx.render(midi_on, 4800); // Let it play for 100ms

    // Second buffer: note off
    midi::MidiBuffer midi_off;
    midi_off.add(midi::MidiEvent::note_off(0, 60));
    auto after_off = fx.render(midi_off, 9600); // 200ms after note-off

    // The output should decay to near-silence by the end
    float end_val = std::abs(after_off.channel(0)[9500]);
    REQUIRE(end_val < 0.05f); // Should be very quiet after 200ms release
}

TEST_CASE("PulpTone polyphony", "[pulptone]") {
    ToneFixture fx;
    fx.store.set_value(kAttack, 1.0f);

    midi::MidiBuffer midi_in;
    // Play a chord: C4 + E4 + G4
    auto c = midi::MidiEvent::note_on(0, 60, 100);
    auto e = midi::MidiEvent::note_on(0, 64, 100);
    auto g = midi::MidiEvent::note_on(0, 67, 100);
    midi_in.add(c);
    midi_in.add(e);
    midi_in.add(g);

    auto out = fx.render(midi_in, 4800);

    // Output should be louder than a single note (three voices summed)
    float max_val = 0.0f;
    for (std::size_t i = 2400; i < 4800; ++i) {
        max_val = std::max(max_val, std::abs(out.channel(0)[i]));
    }
    REQUIRE(max_val > 0.1f); // Chord should be audible
}
