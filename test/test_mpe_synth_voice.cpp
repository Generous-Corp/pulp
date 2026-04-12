#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/mpe_synth_voice.hpp>

using namespace pulp::midi;
using Catch::Approx;
using Kind = MpeExpressionEvent::Kind;

namespace {

struct TestVoice : public MpeSynthVoice {
    int on_count = 0, off_count = 0;
    int render_count = 0;
    void on_note_on(const MpeNoteState& n) override {
        MpeSynthVoice::on_note_on(n); ++on_count;
    }
    void on_note_off() override {
        MpeSynthVoice::on_note_off(); ++off_count;
    }
    void render(float* out, int num_samples) override {
        ++render_count;
        for (int i = 0; i < num_samples; ++i) {
            advance_smoothers();
            out[i] = pressure();
        }
    }
};

MpeExpressionEvent note_on_event(uint8_t ch, uint8_t note, uint8_t vel, uint32_t id) {
    MpeNoteState s; s.active = true; s.channel = ch; s.note = note; s.velocity = vel; s.note_id = id;
    return {0, Kind::NoteOn, s};
}
MpeExpressionEvent note_off_event(uint32_t id) {
    MpeNoteState s; s.active = false; s.note_id = id;
    return {0, Kind::NoteOff, s};
}
MpeExpressionEvent pitch_bend_event(uint32_t id, float semi) {
    MpeNoteState s; s.active = true; s.note_id = id; s.pitch_bend_semitones = semi;
    return {0, Kind::PitchBend, s};
}
MpeExpressionEvent pressure_event(uint32_t id, float p) {
    MpeNoteState s; s.active = true; s.note_id = id; s.pressure = p;
    return {0, Kind::Pressure, s};
}

} // namespace

TEST_CASE("MpeSynthVoice smoothing ramps pressure toward target", "[midi][mpe]") {
    TestVoice v;
    v.set_smoothing(0.9f);
    MpeNoteState s; s.note_id = 1; s.note = 60; s.velocity = 100;
    v.on_note_on(s);

    v.set_pressure(1.0f);
    for (int i = 0; i < 200; ++i) v.advance_smoothers();
    REQUIRE(v.pressure() == Approx(1.0f).margin(0.05f));
}

TEST_CASE("MpeVoiceAllocator routes events to voices by note_id", "[midi][mpe]") {
    MpeVoiceAllocator<TestVoice> alloc{4};

    alloc.dispatch(note_on_event(1, 60, 100, 7));
    REQUIRE(alloc.active_count() == 1);

    alloc.dispatch(pitch_bend_event(7, 2.0f));
    alloc.dispatch(pressure_event(7, 0.75f));

    bool found = false;
    for (std::size_t i = 0; i < alloc.polyphony(); ++i) {
        auto& v = alloc.voice(i);
        if (v.active() && v.note_id() == 7) {
            // after dispatching targets and advancing once, smoothed value
            // approaches target:
            v.advance_smoothers();
            REQUIRE(v.note_number() == 60);
            REQUIRE(v.velocity() == 100);
            found = true;
        }
    }
    REQUIRE(found);

    alloc.dispatch(note_off_event(7));
    // voice is in release, still active until subclass finishes release;
    // but TestVoice never finishes release on its own.
    bool releasing = false;
    for (std::size_t i = 0; i < alloc.polyphony(); ++i) {
        if (alloc.voice(i).releasing()) releasing = true;
    }
    REQUIRE(releasing);
}

TEST_CASE("MpeVoiceAllocator steals oldest when full", "[midi][mpe]") {
    MpeVoiceAllocator<TestVoice> alloc{2};
    alloc.set_steal_mode(MpeVoiceStealMode::Oldest);

    alloc.dispatch(note_on_event(1, 60, 100, 1));
    alloc.dispatch(note_on_event(2, 64, 100, 2));
    REQUIRE(alloc.active_count() == 2);

    // Third note-on must steal one of the two — the allocator should
    // reuse a slot rather than add a third voice.
    alloc.dispatch(note_on_event(3, 67, 100, 3));
    REQUIRE(alloc.polyphony() == 2);
    REQUIRE(alloc.active_count() == 2);

    // Oldest (note_id=1) should have been stolen; note_id=3 should live.
    bool has_3 = false, has_1 = false;
    for (std::size_t i = 0; i < alloc.polyphony(); ++i) {
        if (!alloc.voice(i).active()) continue;
        if (alloc.voice(i).note_id() == 3) has_3 = true;
        if (alloc.voice(i).note_id() == 1) has_1 = true;
    }
    REQUIRE(has_3);
    REQUIRE_FALSE(has_1);
}

TEST_CASE("MpeVoiceAllocator dispatches an entire MpeBuffer in order", "[midi][mpe]") {
    MpeVoiceAllocator<TestVoice> alloc{4};
    MpeBuffer buf;
    buf.add(note_on_event(1, 60, 100, 11));
    buf.add(pressure_event(11, 0.5f));
    buf.add(note_on_event(2, 64, 90, 12));
    alloc.dispatch_all(buf);
    REQUIRE(alloc.active_count() == 2);
}

TEST_CASE("MpeGlideDetector flags overlap on same channel", "[midi][mpe]") {
    MpeGlideDetector glide;
    MpeNoteState a; a.channel = 1; a.note = 60; a.note_id = 1;
    MpeNoteState b; b.channel = 1; b.note = 62; b.note_id = 2;
    MpeNoteState c; c.channel = 2; c.note = 67; c.note_id = 3;

    REQUIRE_FALSE(glide.observe_note_on(a));  // first note on channel — no glide
    REQUIRE(glide.observe_note_on(b));         // overlap on same channel — glide
    REQUIRE_FALSE(glide.observe_note_on(c));   // different channel — not glide

    glide.observe_note_off(b);
    REQUIRE(glide.channel_held(1));            // a still held (we only released b)
    glide.observe_note_off(a);
    REQUIRE_FALSE(glide.channel_held(1));
}

TEST_CASE("MpeVoiceAllocator reports last_was_glide", "[midi][mpe]") {
    MpeVoiceAllocator<TestVoice> alloc{4};
    alloc.dispatch(note_on_event(1, 60, 100, 1));
    REQUIRE_FALSE(alloc.last_was_glide());
    alloc.dispatch(note_on_event(1, 62, 100, 2));
    REQUIRE(alloc.last_was_glide());
}
