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
    void finish_now() { finish_release(); }
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
MpeExpressionEvent note_off_event(uint32_t id, uint8_t ch = 0) {
    MpeNoteState s; s.active = false; s.channel = ch; s.note_id = id;
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
MpeExpressionEvent timbre_event(uint32_t id, float t) {
    MpeNoteState s; s.active = true; s.note_id = id; s.timbre = t;
    return {0, Kind::Timbre, s};
}

template<typename Alloc>
bool has_note_id(const Alloc& alloc, uint32_t note_id) {
    for (std::size_t i = 0; i < alloc.polyphony(); ++i) {
        if (alloc.voice(i).active() && alloc.voice(i).note_id() == note_id) return true;
    }
    return false;
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

TEST_CASE("MpeSynthVoice clamps smoothing and resets expression state",
          "[midi][mpe][issue-645]") {
    TestVoice v;
    MpeNoteState s;
    s.note_id = 42;
    s.channel = 4;
    s.note = 62;
    s.velocity = 111;
    s.pitch_bend_semitones = 3.0f;
    s.pressure = 0.25f;
    s.timbre = 0.5f;
    v.on_note_on(s);

    REQUIRE(v.active());
    REQUIRE_FALSE(v.releasing());
    REQUIRE(v.channel() == 4);
    REQUIRE(v.note_number() == 62);
    REQUIRE(v.velocity() == 111);

    v.set_smoothing(-1.0f);
    v.set_pitch_bend(-2.0f);
    v.set_pressure(0.75f);
    v.set_timbre(0.25f);
    v.advance_smoothers();
    REQUIRE(v.pitch_bend() == Approx(-2.0f));
    REQUIRE(v.pressure() == Approx(0.75f));
    REQUIRE(v.timbre() == Approx(0.25f));

    v.set_smoothing(2.0f);
    v.set_pressure(1.0f);
    v.advance_smoothers();
    REQUIRE(v.pressure() > 0.75f);
    REQUIRE(v.pressure() < 0.751f);

    v.on_note_off();
    REQUIRE(v.releasing());
    v.finish_now();
    REQUIRE_FALSE(v.active());
    REQUIRE_FALSE(v.releasing());

    v.reset();
    REQUIRE_FALSE(v.active());
    REQUIRE_FALSE(v.releasing());
    REQUIRE(v.note_id() == 0);
    REQUIRE(v.channel() == 0);
    REQUIRE(v.note_number() == 0);
    REQUIRE(v.velocity() == 0);
    REQUIRE(v.pitch_bend() == Approx(0.0f));
    REQUIRE(v.pressure() == Approx(0.0f));
    REQUIRE(v.timbre() == Approx(0.0f));
}

TEST_CASE("MpeVoiceAllocator routes events to voices by note_id", "[midi][mpe]") {
    MpeVoiceAllocator<TestVoice> alloc{4};

    alloc.dispatch(note_on_event(1, 60, 100, 7));
    REQUIRE(alloc.active_count() == 1);

    alloc.dispatch(pitch_bend_event(7, 2.0f));
    alloc.dispatch(pressure_event(7, 0.75f));
    alloc.dispatch(timbre_event(7, 0.5f));

    bool found = false;
    for (std::size_t i = 0; i < alloc.polyphony(); ++i) {
        auto& v = alloc.voice(i);
        if (v.active() && v.note_id() == 7) {
            // after dispatching targets and advancing once, smoothed value
            // approaches target:
            v.advance_smoothers();
            REQUIRE(v.note_number() == 60);
            REQUIRE(v.velocity() == 100);
            REQUIRE(v.timbre() > 0.0f);
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

TEST_CASE("MpeVoiceAllocator honors velocity and pitch steal policies",
          "[midi][mpe][issue-645]") {
    MpeVoiceAllocator<TestVoice> by_velocity{2};
    by_velocity.set_steal_mode(MpeVoiceStealMode::LowestVelocity);
    REQUIRE(by_velocity.steal_mode() == MpeVoiceStealMode::LowestVelocity);
    by_velocity.dispatch(note_on_event(1, 60, 20, 1));
    by_velocity.dispatch(note_on_event(2, 64, 100, 2));
    by_velocity.dispatch(note_on_event(3, 67, 80, 3));
    REQUIRE_FALSE(has_note_id(by_velocity, 1));
    REQUIRE(has_note_id(by_velocity, 2));
    REQUIRE(has_note_id(by_velocity, 3));

    MpeVoiceAllocator<TestVoice> by_low_pitch{2};
    by_low_pitch.set_steal_mode(MpeVoiceStealMode::LowestPitch);
    REQUIRE(by_low_pitch.steal_mode() == MpeVoiceStealMode::LowestPitch);
    by_low_pitch.dispatch(note_on_event(1, 48, 100, 11));
    by_low_pitch.dispatch(note_on_event(2, 72, 100, 12));
    by_low_pitch.dispatch(note_on_event(3, 60, 100, 13));
    REQUIRE_FALSE(has_note_id(by_low_pitch, 11));
    REQUIRE(has_note_id(by_low_pitch, 12));
    REQUIRE(has_note_id(by_low_pitch, 13));

    MpeVoiceAllocator<TestVoice> by_high_pitch{2};
    by_high_pitch.set_steal_mode(MpeVoiceStealMode::HighestPitch);
    REQUIRE(by_high_pitch.steal_mode() == MpeVoiceStealMode::HighestPitch);
    by_high_pitch.dispatch(note_on_event(1, 48, 100, 21));
    by_high_pitch.dispatch(note_on_event(2, 72, 100, 22));
    by_high_pitch.dispatch(note_on_event(3, 60, 100, 23));
    REQUIRE(has_note_id(by_high_pitch, 21));
    REQUIRE_FALSE(has_note_id(by_high_pitch, 22));
    REQUIRE(has_note_id(by_high_pitch, 23));
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

TEST_CASE("MpeVoiceAllocator ignores unknown expressions and reset_all clears glide",
          "[midi][mpe][issue-645]") {
    MpeVoiceAllocator<TestVoice> alloc{2};
    alloc.dispatch(pitch_bend_event(999, 12.0f));
    alloc.dispatch(pressure_event(999, 1.0f));
    alloc.dispatch(timbre_event(999, 1.0f));
    alloc.dispatch(note_off_event(999, 1));
    REQUIRE(alloc.active_count() == 0);

    alloc.dispatch(note_on_event(1, 60, 100, 1));
    alloc.dispatch(note_on_event(1, 62, 100, 2));
    REQUIRE(alloc.last_was_glide());
    REQUIRE(alloc.active_count() == 2);

    alloc.reset_all();
    REQUIRE(alloc.active_count() == 0);
    REQUIRE_FALSE(alloc.last_was_glide());
    for (std::size_t i = 0; i < alloc.polyphony(); ++i) {
        REQUIRE_FALSE(alloc.voice(i).active());
        REQUIRE_FALSE(alloc.voice(i).releasing());
    }

    alloc.dispatch(note_on_event(1, 64, 100, 3));
    REQUIRE_FALSE(alloc.last_was_glide());
    REQUIRE(alloc.active_count() == 1);
}

TEST_CASE("MpeVoiceAllocator tolerates zero polyphony",
          "[midi][mpe][issue-645]") {
    MpeVoiceAllocator<TestVoice> alloc{0};
    REQUIRE(alloc.polyphony() == 0);
    REQUIRE(alloc.active_count() == 0);

    alloc.dispatch(note_on_event(1, 60, 100, 1));
    alloc.dispatch(pressure_event(1, 0.5f));
    alloc.dispatch(note_off_event(1, 1));
    REQUIRE(alloc.active_count() == 0);
    REQUIRE_FALSE(alloc.last_was_glide());

    alloc.reset_all();
    REQUIRE(alloc.active_count() == 0);
    REQUIRE_FALSE(alloc.last_was_glide());
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

TEST_CASE("MpeGlideDetector ignores unmatched releases and reset clears channels",
          "[midi][mpe][codecov]") {
    MpeGlideDetector glide;
    MpeNoteState note;
    note.channel = 9;
    note.note = 72;
    note.note_id = 90;

    glide.observe_note_off(note);
    REQUIRE_FALSE(glide.channel_held(9));

    REQUIRE_FALSE(glide.observe_note_on(note));
    REQUIRE(glide.channel_held(9));

    glide.reset();
    REQUIRE_FALSE(glide.channel_held(9));
    REQUIRE_FALSE(glide.observe_note_on(note));
}

TEST_CASE("MpeVoiceAllocator reports last_was_glide", "[midi][mpe]") {
    MpeVoiceAllocator<TestVoice> alloc{4};
    alloc.dispatch(note_on_event(1, 60, 100, 1));
    REQUIRE_FALSE(alloc.last_was_glide());
    alloc.dispatch(note_on_event(1, 62, 100, 2));
    REQUIRE(alloc.last_was_glide());
}

TEST_CASE("MpeVoiceAllocator steal does not double-decrement glide on later note-off", "[midi][mpe]") {
    // Regression for PR #138 Codex P2: if a voice is stolen mid-life, the
    // deferred NoteOff for that note must not decrement the glide
    // refcount again (its channel is already released by the steal path).
    MpeVoiceAllocator<TestVoice> alloc{1};
    alloc.set_steal_mode(MpeVoiceStealMode::Oldest);

    // note_id=1 on channel 1; gets stolen by note_id=2 on the same channel.
    alloc.dispatch(note_on_event(1, 60, 100, 1));
    alloc.dispatch(note_on_event(1, 62, 100, 2));   // steals id=1, legato
    REQUIRE(alloc.last_was_glide());

    // Host now delivers the note-off for the stolen note (id=1). It must
    // not drive the channel refcount below the live note's contribution.
    alloc.dispatch(note_off_event(1));

    // A further note-on on the same channel while id=2 is still held
    // must still be flagged as glide.
    alloc.dispatch(note_on_event(1, 64, 100, 3));
    REQUIRE(alloc.last_was_glide());
}

TEST_CASE("MpeVoiceAllocator steal path decrements glide refcount", "[midi][mpe]") {
    // Regression for Codex P2: when a steal retires a held note on some
    // channel, subsequent note-ons on that channel must not be flagged as
    // glide because the stolen note's refcount stayed elevated.
    MpeVoiceAllocator<TestVoice> alloc{1};
    alloc.set_steal_mode(MpeVoiceStealMode::Oldest);

    alloc.dispatch(note_on_event(3, 60, 100, 1));   // held on channel 3
    alloc.dispatch(note_on_event(4, 64, 100, 2));   // steals id=1, channel 3 no longer held
    REQUIRE(alloc.active_count() == 1);

    // Now a note on channel 3 should be fresh, not flagged as glide.
    alloc.dispatch(note_on_event(3, 67, 100, 3));
    REQUIRE_FALSE(alloc.last_was_glide());
}
