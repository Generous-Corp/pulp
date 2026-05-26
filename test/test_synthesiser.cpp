#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/midi/synthesiser.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp::midi;
using Catch::Matchers::WithinAbs;

// ────────────────────────────────────────────────────────────────────────
// macOS plan item 2.5 — Generic Synthesiser polyphony framework
//
// Plain-MIDI (non-MPE) synth with voice-stealing strategies and
// per-block event dispatch into a fixed pool of voices.
// ────────────────────────────────────────────────────────────────────────

namespace {

/// Test voice that records lifecycle + accumulates sample counts so
/// tests can verify dispatch + render scheduling.
class TestVoice : public SynthesiserVoice {
public:
    void on_note_on(const SynthesiserNote& n) override {
        SynthesiserVoice::on_note_on(n);
        ++note_on_calls;
        last_velocity = n.velocity;
    }
    void on_note_off() override {
        SynthesiserVoice::on_note_off();
        ++note_off_calls;
    }
    void on_pitch_bend(float semis) override {
        ++pitch_bend_calls;
        last_pitch_bend = semis;
    }
    void on_aftertouch(float p) override {
        ++aftertouch_calls;
        last_aftertouch = p;
    }
    void on_cc(uint8_t cc, uint8_t value) override {
        ++cc_calls;
        last_cc_number = cc;
        last_cc_value = value;
    }
    void render(float* out, int n) override {
        rendered_samples += n;
        // Add a constant 0.5 per active voice so output sums reveal
        // how many voices ran across the block.
        for (int i = 0; i < n; ++i) out[i] += 0.5f;
    }
    float peak_level() const override { return peak_; }
    void reset() override {
        SynthesiserVoice::reset();
        peak_ = 0.0f;
        // Lifecycle counters are intentionally NOT reset — they accumulate
        // across the voice's life so tests can read steal-path behavior.
    }

    void set_peak(float p) { peak_ = p; }

    int note_on_calls = 0;
    int note_off_calls = 0;
    int pitch_bend_calls = 0;
    int aftertouch_calls = 0;
    int cc_calls = 0;
    int rendered_samples = 0;
    uint8_t last_velocity = 0;
    float last_pitch_bend = 0.0f;
    float last_aftertouch = 0.0f;
    uint8_t last_cc_number = 0;
    uint8_t last_cc_value = 0;

private:
    float peak_ = 0.0f;
};

MidiEvent note_on_ev(uint8_t ch, uint8_t note, uint8_t vel, int sample_offset = 0) {
    return MidiEvent{
        choc::midi::ShortMessage(
            static_cast<uint8_t>(0x90 | (ch & 0x0F)), note, vel),
        sample_offset,
        0.0
    };
}

MidiEvent note_off_ev(uint8_t ch, uint8_t note, int sample_offset = 0) {
    return MidiEvent{
        choc::midi::ShortMessage(
            static_cast<uint8_t>(0x80 | (ch & 0x0F)), note, 0),
        sample_offset,
        0.0
    };
}

MidiEvent cc_ev(uint8_t ch, uint8_t cc, uint8_t value) {
    return MidiEvent{
        choc::midi::ShortMessage(
            static_cast<uint8_t>(0xB0 | (ch & 0x0F)), cc, value),
        0, 0.0
    };
}

MidiEvent pitch_bend_ev(uint8_t ch, uint16_t value14) {
    return MidiEvent{
        choc::midi::ShortMessage(
            static_cast<uint8_t>(0xE0 | (ch & 0x0F)),
            static_cast<uint8_t>(value14 & 0x7F),
            static_cast<uint8_t>((value14 >> 7) & 0x7F)),
        0, 0.0
    };
}

MidiEvent aftertouch_ev(uint8_t ch, uint8_t pressure) {
    return MidiEvent{
        choc::midi::ShortMessage(
            static_cast<uint8_t>(0xD0 | (ch & 0x0F)), pressure, 0),
        0, 0.0
    };
}

} // namespace

TEST_CASE("Synthesiser allocates a free voice on note_on", "[midi][synth]") {
    Synthesiser<TestVoice> synth(4);
    REQUIRE(synth.active_count() == 0);
    synth.note_on(0, 60, 100);
    REQUIRE(synth.active_count() == 1);
    REQUIRE(synth.voice(0).note().note == 60);
    REQUIRE(synth.voice(0).note().velocity == 100);
}

TEST_CASE("Synthesiser note_off marks the matching voice as releasing",
          "[midi][synth]") {
    Synthesiser<TestVoice> synth(4);
    synth.note_on(0, 60, 100);
    synth.note_off(0, 60);
    REQUIRE(synth.voice(0).releasing());
    // Still active until the subclass calls mark_inactive().
    REQUIRE(synth.voice(0).active());
    // Codex P2 on #2870 — `note().releasing` MUST stay in sync with
    // the voice's `releasing_` flag so subclasses reading either
    // path see the same state.
    REQUIRE(synth.voice(0).note().releasing);
}

TEST_CASE("Synthesiser note_on velocity 0 is a note-off", "[midi][synth]") {
    Synthesiser<TestVoice> synth(4);
    synth.note_on(0, 60, 100);
    synth.note_on(0, 60, 0); // velocity 0 → note off
    REQUIRE(synth.voice(0).releasing());
}

TEST_CASE("Synthesiser allocates separate voices for distinct notes",
          "[midi][synth]") {
    Synthesiser<TestVoice> synth(4);
    synth.note_on(0, 60, 100);
    synth.note_on(0, 64, 90);
    synth.note_on(0, 67, 80);
    REQUIRE(synth.active_count() == 3);
}

TEST_CASE("Synthesiser Oldest steal evicts the smallest note_id",
          "[midi][synth][steal]") {
    Synthesiser<TestVoice> synth(2);
    synth.set_steal_strategy(VoiceStealStrategy::Oldest);
    synth.note_on(0, 60, 100); // note_id = 1 (oldest)
    synth.note_on(0, 64, 90);  // note_id = 2
    synth.note_on(0, 67, 80);  // steals → oldest (note 60)
    REQUIRE(synth.active_count() == 2);
    // No voice holds note 60 anymore.
    for (std::size_t i = 0; i < synth.polyphony(); ++i) {
        REQUIRE(synth.voice(i).note().note != 60);
    }
}

TEST_CASE("Synthesiser Lowest steal evicts the lowest pitch",
          "[midi][synth][steal]") {
    Synthesiser<TestVoice> synth(2);
    synth.set_steal_strategy(VoiceStealStrategy::Lowest);
    synth.note_on(0, 60, 100);
    synth.note_on(0, 72, 90);
    synth.note_on(0, 80, 80); // steals lowest (60)
    REQUIRE(synth.active_count() == 2);
    for (std::size_t i = 0; i < synth.polyphony(); ++i) {
        REQUIRE(synth.voice(i).note().note != 60);
    }
}

TEST_CASE("Synthesiser Highest steal evicts the highest pitch",
          "[midi][synth][steal]") {
    Synthesiser<TestVoice> synth(2);
    synth.set_steal_strategy(VoiceStealStrategy::Highest);
    synth.note_on(0, 60, 100);
    synth.note_on(0, 80, 90);
    synth.note_on(0, 64, 80); // steals highest (80)
    REQUIRE(synth.active_count() == 2);
    for (std::size_t i = 0; i < synth.polyphony(); ++i) {
        REQUIRE(synth.voice(i).note().note != 80);
    }
}

TEST_CASE("Synthesiser Priority steal evicts the lowest priority",
          "[midi][synth][steal]") {
    Synthesiser<TestVoice> synth(2);
    synth.set_steal_strategy(VoiceStealStrategy::Priority);
    synth.note_on(0, 60, 100, /*priority=*/5);
    synth.note_on(0, 64, 90, /*priority=*/1); // lowest priority
    synth.note_on(0, 67, 80, /*priority=*/3); // steals priority-1 voice
    REQUIRE(synth.active_count() == 2);
    for (std::size_t i = 0; i < synth.polyphony(); ++i) {
        REQUIRE(synth.voice(i).note().note != 64);
    }
}

TEST_CASE("Synthesiser Quietest steal evicts the lowest peak_level",
          "[midi][synth][steal]") {
    Synthesiser<TestVoice> synth(2);
    synth.set_steal_strategy(VoiceStealStrategy::Quietest);
    synth.note_on(0, 60, 100);
    synth.note_on(0, 64, 90);
    synth.voice(0).set_peak(0.8f);
    synth.voice(1).set_peak(0.2f);
    synth.note_on(0, 67, 80); // steals voice(1) (quieter)
    REQUIRE(synth.voice(1).note().note == 67);
    REQUIRE(synth.voice(0).note().note == 60);
}

TEST_CASE("Synthesiser channel-level pitch bend reaches every voice on that channel",
          "[midi][synth][controllers]") {
    Synthesiser<TestVoice> synth(4);
    synth.note_on(0, 60, 100);
    synth.note_on(0, 64, 100);
    synth.note_on(1, 72, 100); // different channel — must NOT receive

    synth.pitch_bend(0, 1.5f);
    REQUIRE(synth.voice(0).pitch_bend_calls == 1);
    REQUIRE(synth.voice(1).pitch_bend_calls == 1);
    REQUIRE(synth.voice(2).pitch_bend_calls == 0);
    REQUIRE_THAT(synth.voice(0).last_pitch_bend, WithinAbs(1.5f, 1e-6f));
}

TEST_CASE("Synthesiser channel-level aftertouch routes to channel voices",
          "[midi][synth][controllers]") {
    Synthesiser<TestVoice> synth(4);
    synth.note_on(0, 60, 100);
    synth.note_on(1, 64, 100);
    synth.aftertouch(0, 0.75f);
    REQUIRE(synth.voice(0).aftertouch_calls == 1);
    REQUIRE_THAT(synth.voice(0).last_aftertouch, WithinAbs(0.75f, 1e-6f));
    REQUIRE(synth.voice(1).aftertouch_calls == 0);
}

TEST_CASE("Synthesiser channel-level CC routes to channel voices",
          "[midi][synth][controllers]") {
    Synthesiser<TestVoice> synth(2);
    synth.note_on(0, 60, 100);
    synth.cc(0, 1, 42); // mod wheel
    REQUIRE(synth.voice(0).cc_calls == 1);
    REQUIRE(synth.voice(0).last_cc_number == 1);
    REQUIRE(synth.voice(0).last_cc_value == 42);
}

TEST_CASE("Synthesiser MidiBuffer process dispatches events at sample offsets",
          "[midi][synth][process]") {
    Synthesiser<TestVoice> synth(2);
    MidiBuffer buf;
    buf.add(note_on_ev(0, 60, 100, /*offset=*/0));
    buf.add(note_on_ev(0, 64, 90, /*offset=*/100));
    buf.add(note_off_ev(0, 60, /*offset=*/200));

    std::vector<float> out(256, 0.0f);
    synth.process(buf, out.data(), static_cast<int>(out.size()));

    // Voice 0 (note 60) was active samples 0..200 → 200 samples rendered
    // before note_off (which moves it to releasing but still rendering).
    // After note_off it keeps rendering through the rest of the block
    // (releasing = true, active = true) → another 56 samples.
    REQUIRE(synth.voice(0).note().note == 60);
    REQUIRE(synth.voice(0).releasing());
    REQUIRE(synth.voice(0).rendered_samples == 256);

    // Voice 1 (note 64) activated at sample 100 → renders samples
    // 100..256 → 156 samples.
    REQUIRE(synth.voice(1).note().note == 64);
    REQUIRE_FALSE(synth.voice(1).releasing());
    REQUIRE(synth.voice(1).rendered_samples == 156);
}

TEST_CASE("Synthesiser MidiBuffer process handles pitch-bend events",
          "[midi][synth][process]") {
    Synthesiser<TestVoice> synth(2);
    synth.set_pitch_bend_range_semitones(12.0f);
    MidiBuffer buf;
    buf.add(note_on_ev(0, 60, 100, 0));
    buf.add(pitch_bend_ev(0, 16383)); // full positive
    std::vector<float> out(128, 0.0f);
    synth.process(buf, out.data(), 128);
    REQUIRE(synth.voice(0).pitch_bend_calls == 1);
    // (16383 - 8192) / 8192 ≈ 1.0 → 1.0 * 12 = 12 semitones (within 1 LSB)
    REQUIRE_THAT(synth.voice(0).last_pitch_bend, WithinAbs(12.0f, 0.01f));
}

TEST_CASE("Synthesiser MidiBuffer process handles aftertouch + CC events",
          "[midi][synth][process]") {
    Synthesiser<TestVoice> synth(2);
    MidiBuffer buf;
    buf.add(note_on_ev(0, 60, 100, 0));
    buf.add(aftertouch_ev(0, 127));
    buf.add(cc_ev(0, 74, 64));
    std::vector<float> out(64, 0.0f);
    synth.process(buf, out.data(), 64);
    REQUIRE(synth.voice(0).aftertouch_calls == 1);
    REQUIRE_THAT(synth.voice(0).last_aftertouch, WithinAbs(1.0f, 1e-6f));
    REQUIRE(synth.voice(0).cc_calls == 1);
    REQUIRE(synth.voice(0).last_cc_number == 74);
    REQUIRE(synth.voice(0).last_cc_value == 64);
}

TEST_CASE("Synthesiser 32-voice polyphony stress without dropouts",
          "[midi][synth][stress]") {
    Synthesiser<TestVoice> synth(32);
    for (uint8_t n = 36; n < 68; ++n) {
        synth.note_on(0, n, 100);
    }
    REQUIRE(synth.active_count() == 32);
    // No voice was dropped — every requested note ended up in a slot.
    std::vector<uint8_t> notes_observed;
    notes_observed.reserve(32);
    for (std::size_t i = 0; i < synth.polyphony(); ++i) {
        if (synth.voice(i).active()) notes_observed.push_back(synth.voice(i).note().note);
    }
    std::sort(notes_observed.begin(), notes_observed.end());
    for (uint8_t n = 36; n < 68; ++n) {
        REQUIRE(std::binary_search(notes_observed.begin(), notes_observed.end(), n));
    }

    // Drive a single 512-sample block — every voice renders its full
    // share, output sum is 32 voices × 512 samples × 0.5 per voice.
    std::vector<float> out(512, 0.0f);
    MidiBuffer empty;
    synth.process(empty, out.data(), 512);
    for (float s : out) REQUIRE_THAT(s, WithinAbs(16.0f, 1e-3f));
}

TEST_CASE("Synthesiser reset clears every voice", "[midi][synth]") {
    Synthesiser<TestVoice> synth(4);
    synth.note_on(0, 60, 100);
    synth.note_on(0, 64, 100);
    synth.reset();
    REQUIRE(synth.active_count() == 0);
}

TEST_CASE("Synthesiser process with empty MidiBuffer renders active voices full block",
          "[midi][synth][process]") {
    Synthesiser<TestVoice> synth(2);
    synth.note_on(0, 60, 100);
    MidiBuffer empty;
    std::vector<float> out(256, 0.0f);
    synth.process(empty, out.data(), 256);
    REQUIRE(synth.voice(0).rendered_samples == 256);
}

TEST_CASE("Synthesiser process with num_samples <= 0 is a no-op",
          "[midi][synth][process]") {
    Synthesiser<TestVoice> synth(2);
    synth.note_on(0, 60, 100);
    MidiBuffer empty;
    std::vector<float> out(16, 0.0f);
    synth.process(empty, out.data(), 0);
    synth.process(empty, out.data(), -5);
    REQUIRE(synth.voice(0).rendered_samples == 0);
}

TEST_CASE("Synthesiser process clamps event sample_offset above block size",
          "[midi][synth][process]") {
    Synthesiser<TestVoice> synth(2);
    MidiBuffer buf;
    buf.add(note_on_ev(0, 60, 100, /*offset=*/0));
    // Out-of-range offset (host emitted late) — clamped to block end,
    // so render fills the whole block first then dispatches.
    buf.add(note_off_ev(0, 60, /*offset=*/9999));
    std::vector<float> out(128, 0.0f);
    synth.process(buf, out.data(), 128);
    REQUIRE(synth.voice(0).rendered_samples == 128);
    REQUIRE(synth.voice(0).releasing());
}
