#include "harness/rt_allocation_probe.hpp"

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
    // #2870 — `note().releasing` MUST stay in sync with
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

TEST_CASE("Synthesiser sustain pedal defers note-off until CC64 is lifted",
          "[midi][synth][sustain][phase3]") {
    Synthesiser<TestVoice> synth(2);
    synth.note_on(0, 60, 100);

    synth.cc(0, 64, 127);
    REQUIRE(synth.voice(0).cc_calls == 1);
    REQUIRE(synth.voice(0).last_cc_number == 64);
    REQUIRE(synth.voice(0).last_cc_value == 127);

    synth.note_off(0, 60);
    REQUIRE(synth.voice(0).active());
    REQUIRE_FALSE(synth.voice(0).releasing());
    REQUIRE(synth.voice(0).note().sustained);
    REQUIRE(synth.voice(0).note_off_calls == 0);

    synth.cc(0, 64, 0);
    REQUIRE(synth.voice(0).releasing());
    REQUIRE_FALSE(synth.voice(0).note().sustained);
    REQUIRE(synth.voice(0).note_off_calls == 1);
}

TEST_CASE("Synthesiser sustain pedal release is channel-scoped",
          "[midi][synth][sustain][phase3]") {
    Synthesiser<TestVoice> synth(2);
    synth.note_on(0, 60, 100);
    synth.note_on(1, 64, 100);

    synth.cc(0, 64, 127);
    synth.cc(1, 64, 127);
    synth.note_off(0, 60);
    synth.note_off(1, 64);

    REQUIRE(synth.voice(0).note().sustained);
    REQUIRE(synth.voice(1).note().sustained);

    synth.cc(0, 64, 0);
    REQUIRE(synth.voice(0).releasing());
    REQUIRE_FALSE(synth.voice(0).note().sustained);
    REQUIRE_FALSE(synth.voice(1).releasing());
    REQUIRE(synth.voice(1).note().sustained);

    synth.cc(1, 64, 0);
    REQUIRE(synth.voice(1).releasing());
    REQUIRE_FALSE(synth.voice(1).note().sustained);
}

TEST_CASE("Synthesiser sostenuto pedal captures only currently held notes",
          "[midi][synth][sostenuto][phase3]") {
    Synthesiser<TestVoice> synth(3);
    synth.note_on(0, 60, 100);

    synth.cc(0, 66, 127);
    REQUIRE(synth.voice(0).cc_calls == 1);
    REQUIRE(synth.voice(0).last_cc_number == 66);
    REQUIRE(synth.voice(0).note().sostenuto);

    synth.note_on(0, 64, 100);
    REQUIRE_FALSE(synth.voice(1).note().sostenuto);

    synth.note_off(0, 60);
    synth.note_off(0, 64);

    REQUIRE(synth.voice(0).active());
    REQUIRE_FALSE(synth.voice(0).releasing());
    REQUIRE(synth.voice(0).note().sostenuto);
    REQUIRE(synth.voice(0).note_off_calls == 0);

    REQUIRE(synth.voice(1).releasing());
    REQUIRE(synth.voice(1).note_off_calls == 1);

    synth.cc(0, 66, 0);
    REQUIRE(synth.voice(0).releasing());
    REQUIRE_FALSE(synth.voice(0).note().sostenuto);
    REQUIRE(synth.voice(0).note_off_calls == 1);
}

TEST_CASE("Synthesiser sustain and sostenuto release independently",
          "[midi][synth][sustain][sostenuto][phase3]") {
    Synthesiser<TestVoice> synth(2);
    synth.note_on(0, 60, 100);

    synth.cc(0, 66, 127);
    synth.cc(0, 64, 127);
    synth.note_off(0, 60);

    REQUIRE(synth.voice(0).note().sostenuto);
    REQUIRE(synth.voice(0).note().sustained);
    REQUIRE_FALSE(synth.voice(0).releasing());

    synth.cc(0, 66, 0);
    REQUIRE_FALSE(synth.voice(0).note().sostenuto);
    REQUIRE(synth.voice(0).note().sustained);
    REQUIRE_FALSE(synth.voice(0).releasing());

    synth.cc(0, 64, 0);
    REQUIRE_FALSE(synth.voice(0).note().sustained);
    REQUIRE(synth.voice(0).releasing());
    REQUIRE(synth.voice(0).note_off_calls == 1);
}

TEST_CASE("Synthesiser soft pedal metadata is channel-scoped",
          "[midi][synth][soft-pedal][phase3]") {
    Synthesiser<TestVoice> synth(3);
    synth.cc(0, 67, 127);

    synth.note_on(0, 60, 100);
    synth.note_on(1, 64, 100);

    REQUIRE(synth.voice(0).note().soft_pedal);
    REQUIRE_FALSE(synth.voice(1).note().soft_pedal);

    synth.cc(1, 67, 127);
    REQUIRE(synth.voice(1).note().soft_pedal);
    REQUIRE(synth.voice(0).note().soft_pedal);

    synth.cc(0, 67, 0);
    REQUIRE_FALSE(synth.voice(0).note().soft_pedal);
    REQUIRE(synth.voice(1).note().soft_pedal);
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

TEST_CASE("Synthesiser voice group choke releases matching held voices",
          "[midi][synth][choke][phase3]") {
    Synthesiser<TestVoice> synth(4);
    synth.note_on(0, 42, 100, /*priority=*/0, /*voice_group=*/1);
    synth.note_on(0, 46, 100, /*priority=*/0, /*voice_group=*/1,
                  /*choke_group=*/true);

    REQUIRE(synth.voice(0).releasing());
    REQUIRE(synth.voice(0).note().voice_group == 1);
    REQUIRE(synth.voice(0).note_off_calls == 1);

    REQUIRE(synth.voice(1).active());
    REQUIRE_FALSE(synth.voice(1).releasing());
    REQUIRE(synth.voice(1).note().note == 46);
    REQUIRE(synth.voice(1).note().voice_group == 1);
}

TEST_CASE("Synthesiser voice group choke is channel-scoped",
          "[midi][synth][choke][phase3]") {
    Synthesiser<TestVoice> synth(4);
    synth.note_on(0, 42, 100, /*priority=*/0, /*voice_group=*/2);
    synth.note_on(1, 42, 100, /*priority=*/0, /*voice_group=*/2);
    synth.note_on(0, 46, 100, /*priority=*/0, /*voice_group=*/2,
                  /*choke_group=*/true);

    REQUIRE(synth.voice(0).releasing());
    REQUIRE_FALSE(synth.voice(1).releasing());
    REQUIRE(synth.voice(1).note().channel == 1);
    REQUIRE(synth.voice(2).note().note == 46);
}

TEST_CASE("Synthesiser voice group without choke allows overlap",
          "[midi][synth][choke][phase3]") {
    Synthesiser<TestVoice> synth(4);
    synth.note_on(0, 42, 100, /*priority=*/0, /*voice_group=*/3);
    synth.note_on(0, 46, 100, /*priority=*/0, /*voice_group=*/3);

    REQUIRE_FALSE(synth.voice(0).releasing());
    REQUIRE_FALSE(synth.voice(1).releasing());
    REQUIRE(synth.active_count() == 2);
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

TEST_CASE("Synthesiser renders hundreds of voices without audio-thread allocation",
          "[midi][synth][stress][rt-safety][phase3]") {
    constexpr std::size_t kVoices = 256;
    constexpr int kSamples = 128;
    Synthesiser<TestVoice> synth(kVoices);

    for (std::size_t i = 0; i < kVoices; ++i) {
        synth.note_on(static_cast<uint8_t>(i % 16),
                      static_cast<uint8_t>(i % 128),
                      100);
    }
    REQUIRE(synth.active_count() == kVoices);

    MidiBuffer empty;
    std::vector<float> out(kSamples, 0.0f);

    {
        pulp::test::RtAllocationProbe probe;
        synth.process(empty, out.data(), kSamples);
        REQUIRE_FALSE(probe.saw_allocation());
    }

    for (float s : out) {
        REQUIRE_THAT(s, WithinAbs(static_cast<float>(kVoices) * 0.5f, 1e-3f));
    }
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

TEST_CASE("Synthesiser exposes voice-count telemetry",
          "[midi][synth][telemetry][phase2]") {
    Synthesiser<TestVoice> synth(2);
    synth.set_steal_strategy(VoiceStealStrategy::Oldest);

    const auto initial = synth.telemetry();
    REQUIRE(initial.polyphony == 2);
    REQUIRE(initial.active_voice_count == 0);
    REQUIRE(initial.releasing_voice_count == 0);
    REQUIRE(initial.steal_count == 0);
    REQUIRE(initial.steal_strategy == VoiceStealStrategy::Oldest);

    synth.note_on(0, 60, 100);
    synth.note_on(0, 64, 100);
    const auto active = synth.telemetry();
    REQUIRE(active.active_voice_count == 2);
    REQUIRE(active.releasing_voice_count == 0);
    REQUIRE(active.steal_count == 0);

    synth.note_off(0, 60);
    const auto releasing = synth.telemetry();
    REQUIRE(releasing.active_voice_count == 2);
    REQUIRE(releasing.releasing_voice_count == 1);

    synth.note_on(0, 67, 100);
    const auto stolen = synth.telemetry();
    REQUIRE(stolen.active_voice_count == 2);
    REQUIRE(stolen.releasing_voice_count == 0);
    REQUIRE(stolen.steal_count == 1);
    REQUIRE(synth.steal_count() == 1);

    synth.reset_steal_count();
    REQUIRE(synth.telemetry().steal_count == 0);
}

TEST_CASE("Synthesiser evaluates optional runtime budget from voice telemetry",
          "[midi][synth][budget-policy][phase4]") {
    Synthesiser<TestVoice> synth(2);
    synth.note_on(0, 60, 100);
    synth.note_on(0, 64, 100);
    synth.note_off(0, 60);

    REQUIRE(synth.estimate_optional_runtime_cost() == 168);

    pulp::runtime::RuntimeBudgetFrame exact(168);
    auto report = synth.evaluate_optional_runtime_budget(
        exact, pulp::runtime::RuntimeWorkLane::Background);
    REQUIRE(report.telemetry.polyphony == 2);
    REQUIRE(report.telemetry.active_voice_count == 2);
    REQUIRE(report.telemetry.releasing_voice_count == 1);
    REQUIRE(report.estimated_cost == 168);
    REQUIRE(report.decision.action == pulp::runtime::RuntimeBudgetAction::Run);
    REQUIRE(report.should_run_optional_work());
    REQUIRE(report.frame_stats.run_count == 1);
    REQUIRE(report.frame_stats.remaining_budget == 0);

    pulp::runtime::RuntimeBudgetFrame tight(167);
    report = synth.evaluate_optional_runtime_budget(
        tight, pulp::runtime::RuntimeWorkLane::Background);
    REQUIRE(report.decision.action == pulp::runtime::RuntimeBudgetAction::Bypass);
    REQUIRE_FALSE(report.should_run_optional_work());
    REQUIRE(report.frame_stats.bypass_count == 1);

    pulp::runtime::RuntimeBudgetPolicy policy;
    policy.shed_background_on_overload = true;
    pulp::runtime::RuntimeBudgetFrame overloaded(1024, policy, true);
    report = synth.evaluate_optional_runtime_budget(
        overloaded, pulp::runtime::RuntimeWorkLane::Background);
    REQUIRE(report.decision.action == pulp::runtime::RuntimeBudgetAction::Shed);
    REQUIRE(report.frame_stats.shed_count == 1);
}

TEST_CASE("Synthesiser optional runtime budget has deterministic large-voice cost",
          "[midi][synth][budget-policy][scale][phase4]") {
    constexpr std::size_t kVoices = 128;
    Synthesiser<TestVoice> synth(kVoices);
    for (std::size_t i = 0; i < kVoices; ++i) {
        synth.note_on(0, static_cast<uint8_t>(i % 128), 100,
                      static_cast<int8_t>(i % 16));
    }

    const auto expected_cost =
        static_cast<std::uint64_t>(kVoices) * 4u
        + static_cast<std::uint64_t>(kVoices) * 64u;
    REQUIRE(synth.estimate_optional_runtime_cost() == expected_cost);

    pulp::runtime::RuntimeBudgetFrame exact(expected_cost);
    auto report = synth.evaluate_optional_runtime_budget(
        exact, pulp::runtime::RuntimeWorkLane::Background);
    REQUIRE(report.telemetry.polyphony == kVoices);
    REQUIRE(report.telemetry.active_voice_count == kVoices);
    REQUIRE(report.estimated_cost == expected_cost);
    REQUIRE(report.decision.action == pulp::runtime::RuntimeBudgetAction::Run);

    pulp::runtime::RuntimeBudgetFrame tight(expected_cost - 1);
    report = synth.evaluate_optional_runtime_budget(
        tight, pulp::runtime::RuntimeWorkLane::Background);
    REQUIRE(report.decision.action == pulp::runtime::RuntimeBudgetAction::Bypass);
}

TEST_CASE("Synthesiser voice telemetry path allocates zero times",
          "[midi][synth][telemetry][rt-safety][phase2]") {
    Synthesiser<TestVoice> synth(2);
    synth.note_on(0, 60, 100);
    synth.note_on(0, 64, 100);
    synth.note_off(0, 60);
    pulp::runtime::RuntimeBudgetFrame frame(1024);

    pulp::test::RtAllocationProbe probe;

    (void)synth.telemetry();
    (void)synth.estimate_optional_runtime_cost();
    (void)synth.evaluate_optional_runtime_budget(
        frame, pulp::runtime::RuntimeWorkLane::Opportunistic);
    (void)synth.active_count();
    (void)synth.releasing_count();
    (void)synth.steal_count();
    synth.reset_steal_count();

    REQUIRE_FALSE(probe.saw_allocation());
}
