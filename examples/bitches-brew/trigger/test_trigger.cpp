// Trigger — the four signals a note becomes, and the envelope generator behind one
// of them.
//
// The envelope is the one thing in this suite that is genuinely stateful, so it is
// held to a stricter standard than "it looks right": the running state machine is
// measured, sample for sample, against the pure `envelope_at` the editor draws.
//
// Scope note: `Processor::process()` and the pure envelope functions. Nothing here
// proves a voltage reaches a jack.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "trigger_processor.hpp"
#include <pulp/format/headless.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

using namespace pulp;
using namespace pulp::examples::brew;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kSampleRate = 48000.0;

/// A value no correct output ever emits, pre-loaded into the output buffer so a
/// channel the processor forgets to write cannot read as silence.
constexpr float kSentinel = -7.0f;

[[nodiscard]] constexpr float as_param(TriggerSignal m) noexcept {
    return static_cast<float>(static_cast<int>(m));
}
[[nodiscard]] constexpr float as_param(Multiplier m) noexcept {
    return static_cast<float>(static_cast<int>(m));
}

/// A prepared host. `HeadlessHost` is neither copyable nor movable — it hands the
/// processor a pointer to its own StateStore — so the rig holds one in place.
struct Rig {
    Rig() { host.prepare(kSampleRate, 4096, 2, 2); }

    state::StateStore& state() { return host.state(); }
    const TriggerProcessor& proc() const {
        return *static_cast<const TriggerProcessor*>(host.processor());
    }

    /// Set a control on both channels, so a test never accidentally reads the
    /// other one's default.
    void set(state::ParamID id, float v) {
        for (std::size_t ch = 0; ch < kChannelCount; ++ch)
            host.state().set_value(static_cast<state::ParamID>(param_for(id, ch)), v);
    }
    void set_ch(state::ParamID id, std::size_t ch, float v) {
        host.state().set_value(static_cast<state::ParamID>(param_for(id, ch)), v);
    }

    /// The simplest envelope worth measuring: no delay, a linear rise, a linear
    /// fall to the sustain, a linear release.
    void simple_envelope() {
        set(TriggerProcessor::kMode, as_param(TriggerSignal::envelope));
        set(TriggerProcessor::kAnyNote, 1.0f);
        set(TriggerProcessor::kTimeA1, 0.0f);
        set(TriggerProcessor::kLevelA1, 0.0f);
        set(TriggerProcessor::kTimeA2, 0.01f);
        set(TriggerProcessor::kLevelA2, 1.0f);
        set(TriggerProcessor::kTimeA3, 0.02f);
        set(TriggerProcessor::kSustain, 0.5f);
        set(TriggerProcessor::kTimeR1, 0.02f);
        set(TriggerProcessor::kLevelR1, 0.0f);
        set(TriggerProcessor::kTimeR2, 0.0f);
    }

    /// Render one block. `midi` is consumed; `input` fills both input channels.
    std::vector<float> render(int frames, midi::MidiBuffer* midi = nullptr,
                              bool bypassed = false,
                              const std::function<void(int, float*, float*)>& in_fill = {}) {
        audio::Buffer<float> in(2, static_cast<std::size_t>(frames));
        audio::Buffer<float> out(2, static_cast<std::size_t>(frames));
        in.clear();
        for (std::size_t c = 0; c < 2; ++c)
            for (int n = 0; n < frames; ++n)
                out.channel(c)[static_cast<std::size_t>(n)] = kSentinel;
        if (in_fill)
            for (int n = 0; n < frames; ++n)
                in_fill(n, &in.channel(0)[static_cast<std::size_t>(n)],
                        &in.channel(1)[static_cast<std::size_t>(n)]);

        const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
        audio::BufferView<const float> iv(ip, 2, static_cast<std::size_t>(frames));
        auto ov = out.view();

        format::ProcessContext ctx;
        ctx.sample_rate = kSampleRate;
        ctx.num_samples = frames;
        ctx.is_playing = true;
        ctx.is_bypassed = bypassed;
        ctx.tempo_bpm = 120.0;

        midi::MidiBuffer empty;
        midi::MidiBuffer out_midi;
        host.processor()->process(ov, iv, midi ? *midi : empty, out_midi, ctx);

        right.assign(out.channel(1).begin(), out.channel(1).end());
        return std::vector<float>(out.channel(0).begin(), out.channel(0).end());
    }

    format::HeadlessHost host{create_trigger};
    std::vector<float> right;
};

midi::MidiBuffer note_on_at(int offset, uint8_t note = 60, uint8_t vel = 100) {
    midi::MidiBuffer b;
    auto e = midi::MidiEvent::note_on(0, note, vel);
    e.sample_offset = offset;
    b.add(e);
    return b;
}

midi::MidiBuffer note_off_at(int offset, uint8_t note = 60) {
    midi::MidiBuffer b;
    auto e = midi::MidiEvent::note_off(0, note);
    e.sample_offset = offset;
    b.add(e);
    return b;
}

}  // namespace

// ── The curve ────────────────────────────────────────────────────────────────

TEST_CASE("curve_shape pins both ends under every setting", "[brew][trigger]") {
    for (bool exp : {false, true})
        for (float c : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
            CAPTURE(exp, c);
            CHECK_THAT(curve_shape(0.0, c, exp), WithinAbs(0.0f, 1e-6f));
            CHECK_THAT(curve_shape(1.0, c, exp), WithinAbs(1.0f, 1e-6f));
        }
}

TEST_CASE("curve_shape at zero is a bit-exact straight line", "[brew][trigger]") {
    // A curve knob at rest must not colour the signal. Not "close to linear" —
    // equal, so a preset saved with the knob untouched renders identically.
    for (bool exp : {false, true})
        for (int i = 0; i <= 10; ++i) {
            const double p = i / 10.0;
            CHECK(curve_shape(p, 0.0f, exp) == static_cast<float>(p));
        }
}

TEST_CASE("curve_shape is monotone and bends the way the sign says",
          "[brew][trigger]") {
    for (bool exp : {false, true}) {
        CAPTURE(exp);
        // Positive: most of the travel early. Negative: held back.
        CHECK(curve_shape(0.5, 1.0f, exp) > 0.5f);
        CHECK(curve_shape(0.5, -1.0f, exp) < 0.5f);
        for (float c : {-1.0f, 0.0f, 1.0f}) {
            float last = -1.0f;
            for (int i = 0; i <= 32; ++i) {
                const float v = curve_shape(i / 32.0, c, exp);
                CHECK(v >= last);
                last = v;
            }
        }
    }
    // The two laws are genuinely different shapes, not the same one relabelled.
    CHECK(curve_shape(0.5, 1.0f, false) != curve_shape(0.5, 1.0f, true));
}

TEST_CASE("curve_shape clamps its inputs rather than running past them",
          "[brew][trigger]") {
    CHECK(curve_shape(-1.0, 0.0f, false) == 0.0f);
    CHECK(curve_shape(2.0, 0.0f, false) == 1.0f);
    CHECK(curve_shape(0.5, 4.0f, false) == curve_shape(0.5, 1.0f, false));
    CHECK(curve_shape(0.5, -4.0f, true) == curve_shape(0.5, -1.0f, true));
}

// ── The pure envelope ────────────────────────────────────────────────────────

namespace {
EnvelopeSpec linear_spec() {
    EnvelopeSpec s;
    s.attack[0] = {0.0f, 0.0, 0.0f};
    s.attack[1] = {1.0f, 0.01, 0.0f};
    s.attack[2] = {0.0f, 0.02, 0.0f};
    s.sustain = 0.5f;
    s.release[0] = {0.0f, 0.02, 0.0f};
    s.release[1] = {0.0f, 0.0, 0.0f};
    return s;
}
}  // namespace

TEST_CASE("envelope_at walks the attack stages and rests on the sustain",
          "[brew][trigger]") {
    const EnvelopeSpec s = linear_spec();
    CHECK_THAT(envelope_at(s, 0.0), WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(envelope_at(s, 0.005), WithinAbs(0.5f, 1e-6f));   // halfway up A2
    CHECK_THAT(envelope_at(s, 0.01), WithinAbs(1.0f, 1e-6f));    // the peak
    CHECK_THAT(envelope_at(s, 0.02), WithinAbs(0.75f, 1e-6f));   // halfway down A3
    CHECK_THAT(envelope_at(s, 0.03), WithinAbs(0.5f, 1e-6f));    // the sustain
    CHECK_THAT(envelope_at(s, 10.0), WithinAbs(0.5f, 1e-6f));    // and it stays there
}

TEST_CASE("Level A1 at zero makes Time A1 a delay", "[brew][trigger]") {
    EnvelopeSpec s = linear_spec();
    s.attack[0] = {0.0f, 0.05, 0.0f};
    // A rise to zero from zero is a wait.
    for (double t : {0.0, 0.01, 0.049}) CHECK_THAT(envelope_at(s, t), WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(envelope_at(s, 0.055), WithinAbs(0.5f, 1e-6f));   // halfway up A2

    // Lift A1's level and the same time becomes a rise, not a wait.
    s.attack[0].target = 0.4f;
    CHECK_THAT(envelope_at(s, 0.025), WithinAbs(0.2f, 1e-6f));
}

TEST_CASE("release_at falls from wherever the note let go", "[brew][trigger]") {
    const EnvelopeSpec s = linear_spec();
    CHECK_THAT(release_at(s, 0.0, 0.5f), WithinAbs(0.5f, 1e-6f));
    CHECK_THAT(release_at(s, 0.01, 0.5f), WithinAbs(0.25f, 1e-6f));
    CHECK_THAT(release_at(s, 0.02, 0.5f), WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(release_at(s, 1.0, 0.5f), WithinAbs(0.0f, 1e-6f));
    // A short note releases from halfway up the attack, not from the sustain.
    CHECK_THAT(release_at(s, 0.01, 0.2f), WithinAbs(0.1f, 1e-6f));
}

TEST_CASE("R2 is a tail after the release, ending at zero", "[brew][trigger]") {
    EnvelopeSpec s = linear_spec();
    s.release[0] = {0.3f, 0.01, 0.0f};   // R1 falls only to 0.3
    s.release[1] = {0.0f, 0.04, 0.0f};   // R2 takes it the rest of the way
    CHECK_THAT(release_at(s, 0.01, 1.0f), WithinAbs(0.3f, 1e-6f));
    CHECK_THAT(release_at(s, 0.03, 1.0f), WithinAbs(0.15f, 1e-6f));
    CHECK_THAT(release_at(s, 0.05, 1.0f), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("Mult scales every envelope time at once", "[brew][trigger]") {
    EnvelopeSpec s = linear_spec();
    s.time_multiplier = 10.0;
    CHECK_THAT(envelope_at(s, 0.05), WithinAbs(0.5f, 1e-6f));   // halfway up A2
    CHECK_THAT(envelope_at(s, 0.1), WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(attack_seconds(s), WithinAbs(0.3, 1e-9));
    CHECK_THAT(release_seconds(s), WithinAbs(0.2, 1e-9));
    // A negative or nonsense time is a zero-length stage, not a stage that runs
    // backwards.
    s.time_multiplier = -1.0;
    CHECK(s.stage_seconds(Stage::attack_2) == 0.0);
}

TEST_CASE("a zero-length stage reaches its target instantly", "[brew][trigger]") {
    EnvelopeSpec s = linear_spec();
    s.attack[1].seconds = 0.0;   // no attack time at all
    // An attack of zero is a jump to the peak, which is what a user setting it to
    // zero is asking for — not a stage that is skipped and never reached.
    CHECK_THAT(envelope_at(s, 0.0), WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(envelope_at(s, 0.01), WithinAbs(0.75f, 1e-6f));   // halfway down A3
    // Every stage zero: straight to the sustain, and not divided by anything.
    s.attack[0].seconds = 0.0;
    s.attack[2].seconds = 0.0;
    CHECK_THAT(envelope_at(s, 0.0), WithinAbs(0.5f, 1e-6f));
    CHECK_THAT(envelope_at(s, 0.0001), WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("A1 with no time and no level is not a stage, so RTZ has an off position",
          "[brew][trigger]") {
    EnvelopeSpec s = linear_spec();
    REQUIRE(s.attack[0].seconds == 0.0);
    REQUIRE(s.attack[0].target == 0.0f);
    CHECK(stage_is_absent(s, Stage::attack_1));
    // A retrigger from the sustain rises from the sustain, not from a zero A1 has
    // stamped over it.
    CHECK_THAT(envelope_at(s, 0.0, 0.5f), WithinAbs(0.5f, 1e-6f));
    CHECK_THAT(envelope_at(s, 0.005, 0.5f), WithinAbs(0.75f, 1e-6f));

    // Give A1 a level, and it is a stage again: an instant jump to that level.
    s.attack[0].target = 0.2f;
    CHECK_FALSE(stage_is_absent(s, Stage::attack_1));
    CHECK_THAT(envelope_at(s, 0.0, 0.5f), WithinAbs(0.2f, 1e-6f));

    // Give it a duration instead, and it is a delay.
    s.attack[0] = {0.0f, 0.05, 0.0f};
    CHECK_FALSE(stage_is_absent(s, Stage::attack_1));
    CHECK_THAT(envelope_at(s, 0.025, 0.5f), WithinAbs(0.25f, 1e-6f));   // falling to 0
}

// ── The state machine renders the pure function ──────────────────────────────

TEST_CASE("the running envelope is envelope_at, sampled", "[brew][trigger]") {
    // The generator is written in terms of the pure function, so this is exact, not
    // approximate. It is worth pinning anyway: the day someone reintroduces a phase
    // accumulator "for speed", the boundary samples move and this catches it.
    for (bool exp : {false, true})
        for (float curve : {-0.7f, 0.0f, 0.7f}) {
            CAPTURE(exp, curve);
            EnvelopeSpec s = linear_spec();
            s.exponential = exp;
            s.attack[1].curve = curve;
            s.attack[2].curve = -curve;

            Envelope e;
            e.note_on(s);
            for (int n = 0; n < 2000; ++n) {
                const double t = static_cast<double>(n) / kSampleRate;
                const float want = envelope_at(s, t);
                const float got = e.process(s, kSampleRate);
                INFO("sample " << n);
                CHECK(got == want);
            }
        }
}

TEST_CASE("the running release tracks release_at sample for sample",
          "[brew][trigger]") {
    EnvelopeSpec s = linear_spec();
    s.release[0] = {0.2f, 0.01, 0.6f};
    s.release[1] = {0.0f, 0.01, -0.4f};

    Envelope e;
    e.note_on(s);
    for (int n = 0; n < 2000; ++n) (void)e.process(s, kSampleRate);   // reach the sustain
    REQUIRE(e.stage(s) == Stage::sustain);
    const float from = e.value();
    e.note_off();
    for (int n = 0; n < 1200; ++n) {
        const double t = static_cast<double>(n) / kSampleRate;
        const float want = release_at(s, t, from);
        const float got = e.process(s, kSampleRate);
        INFO("sample " << n);
        CHECK(got == want);
    }
    CHECK(e.stage(s) == Stage::idle);
    CHECK(e.value() == 0.0f);
}

TEST_CASE("a note shorter than its attack releases from where it got to",
          "[brew][trigger]") {
    const EnvelopeSpec s = linear_spec();
    Envelope e;
    e.note_on(s);
    for (int n = 0; n < 240; ++n) (void)e.process(s, kSampleRate);   // half of A2
    REQUIRE(e.stage(s) == Stage::attack_2);
    const float from = e.value();
    CHECK_THAT(from, WithinAbs(0.5f, 0.01f));
    e.note_off();
    CHECK(e.stage(s) == Stage::release_1);
    // It falls from `from`, not from the sustain it never reached.
    const float next = e.process(s, kSampleRate);
    CHECK_THAT(next, WithinAbs(from, 1e-3f));
    CHECK(next < 0.51f);
}

TEST_CASE("RTZ decides whether a retrigger snaps back to zero", "[brew][trigger]") {
    EnvelopeSpec s = linear_spec();
    Envelope e;

    s.reset_to_zero = true;
    e.note_on(s);
    for (int n = 0; n < 2000; ++n) (void)e.process(s, kSampleRate);
    REQUIRE_THAT(e.value(), WithinAbs(0.5f, 1e-4f));
    e.note_on(s);
    CHECK(e.value() == 0.0f);

    s.reset_to_zero = false;
    e.reset();
    e.note_on(s);
    for (int n = 0; n < 2000; ++n) (void)e.process(s, kSampleRate);
    const float held = e.value();
    e.note_on(s);
    CHECK_THAT(e.value(), WithinAbs(held, 1e-6f));
    // ...and the rise continues from there rather than restarting.
    CHECK_THAT(e.process(s, kSampleRate), WithinAbs(held, 1e-3f));
}

TEST_CASE("a note_off before any note is a no-op", "[brew][trigger]") {
    const EnvelopeSpec s = linear_spec();
    Envelope e;
    e.note_off();
    CHECK(e.stage(s) == Stage::idle);
    CHECK(e.process(s, kSampleRate) == 0.0f);
}

TEST_CASE("an envelope of only zero-length stages does not crawl",
          "[brew][trigger]") {
    EnvelopeSpec s = linear_spec();
    for (auto& a : s.attack) a.seconds = 0.0;
    for (auto& r : s.release) r.seconds = 0.0;
    Envelope e;
    e.note_on(s);
    // One sample to reach the sustain, not one sample per stage.
    CHECK_THAT(e.process(s, kSampleRate), WithinAbs(s.sustain, 1e-6f));
    CHECK(e.stage(s) == Stage::sustain);
    e.note_off();
    CHECK(e.process(s, kSampleRate) == 0.0f);
    CHECK(e.stage(s) == Stage::idle);
}

// ── Velocity and the jack ────────────────────────────────────────────────────

TEST_CASE("velocity_gain is a wire at zero and inverts below it", "[brew][trigger]") {
    for (float v : {0.0f, 0.3f, 1.0f}) CHECK(velocity_gain(0.0f, v) == 1.0f);
    CHECK_THAT(velocity_gain(1.0f, 0.25f), WithinAbs(0.25f, 1e-6f));
    CHECK_THAT(velocity_gain(-1.0f, 0.25f), WithinAbs(0.75f, 1e-6f));
    // Continuous across zero, and never negative.
    CHECK_THAT(velocity_gain(0.5f, 0.0f), WithinAbs(0.5f, 1e-6f));
    CHECK_THAT(velocity_gain(-0.5f, 1.0f), WithinAbs(0.5f, 1e-6f));
    CHECK(velocity_gain(4.0f, 0.0f) == 0.0f);   // clamped
}

TEST_CASE("map_voltage inverts when Min exceeds Max", "[brew][trigger]") {
    CHECK_THAT(map_voltage(0.0f, 0.0f, 1.0f), WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(map_voltage(1.0f, 0.0f, 1.0f), WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(map_voltage(0.5f, -1.0f, 1.0f), WithinAbs(0.0f, 1e-6f));
    // The reason there is no Invert toggle.
    CHECK_THAT(map_voltage(0.0f, 1.0f, -1.0f), WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(map_voltage(1.0f, 1.0f, -1.0f), WithinAbs(-1.0f, 1e-6f));
    // A signal outside [0,1] cannot push the jack outside its rails.
    CHECK(map_voltage(4.0f, 0.0f, 1.0f) == 1.0f);
    CHECK(map_voltage(-4.0f, 0.0f, 1.0f) == 0.0f);
}

TEST_CASE("a pulse of zero length never latches the jack high", "[brew][trigger]") {
    Pulse p;
    p.fire(0.0, kSampleRate);
    CHECK_FALSE(p.high());
    CHECK_FALSE(p.process());
    p.fire(-1.0, kSampleRate);
    CHECK_FALSE(p.process());
    p.fire(2.0 / kSampleRate, kSampleRate);
    CHECK(p.process());
    CHECK(p.process());
    CHECK_FALSE(p.process());
}

TEST_CASE("the Schmitt gate needs a fall before it rises again", "[brew][trigger]") {
    SchmittGate g;
    CHECK(g.process(0.6f, 0.5f, 0.25f));
    CHECK(g.is_high());
    CHECK_FALSE(g.process(1.0f, 0.5f, 0.25f));
    // Wobbling above the falling threshold does not re-arm it.
    for (float v : {0.4f, 0.7f, 0.3f, 0.9f}) CHECK_FALSE(g.process(v, 0.5f, 0.25f));
    CHECK_FALSE(g.process(0.1f, 0.5f, 0.25f));
    CHECK_FALSE(g.is_high());
    CHECK(g.process(0.6f, 0.5f, 0.25f));
}

// ── The processor ────────────────────────────────────────────────────────────

TEST_CASE("Trigger accepts MIDI and declares two channels each way",
          "[brew][trigger]") {
    auto p = create_trigger();
    const auto d = p->descriptor();
    REQUIRE(d.name == "Trigger");
    CHECK(d.accepts_midi);
    REQUIRE(d.output_buses.size() == 1);
    CHECK(d.output_buses[0].default_channels == 2);
    REQUIRE(d.input_buses.size() == 1);
    CHECK(d.input_buses[0].default_channels == 2);
}

TEST_CASE("every declared control exists on both channels", "[brew][trigger]") {
    Rig r;
    for (std::size_t ch = 0; ch < kChannelCount; ++ch) {
        for (const auto& c : TriggerProcessor::controls()) {
            INFO("control " << c.label << " on channel " << ch);
            CHECK(r.state().info(static_cast<state::ParamID>(param_for(c.id, ch))) !=
                  nullptr);
        }
        for (const auto& c : TriggerProcessor::toggles()) {
            INFO("toggle " << c.label << " on channel " << ch);
            CHECK(r.state().info(static_cast<state::ParamID>(param_for(c.id, ch))) !=
                  nullptr);
        }
    }
}

TEST_CASE("a fresh instance is silent until a note arrives", "[brew][trigger][safety]") {
    Rig r;
    const auto v = r.render(64);
    for (float x : v) CHECK(x == 0.0f);
    for (float x : r.right) CHECK(x == 0.0f);
    CHECK_FALSE(r.proc().held(0));
}

TEST_CASE("Gate rises on note-on and falls on note-off", "[brew][trigger]") {
    Rig r;
    r.set(TriggerProcessor::kAnyNote, 1.0f);
    r.set(TriggerProcessor::kMode, as_param(TriggerSignal::gate));

    auto on = note_on_at(10);
    const auto v = r.render(64, &on);
    CHECK(v[9] == 0.0f);
    CHECK(v[10] == 1.0f);   // sample-accurate
    CHECK(v[63] == 1.0f);
    CHECK(r.proc().held(0));

    auto off = note_off_at(20);
    const auto w = r.render(64, &off);
    CHECK(w[19] == 1.0f);
    CHECK(w[20] == 0.0f);
    CHECK_FALSE(r.proc().held(0));
}

TEST_CASE("the gate counts held notes rather than latching a flag",
          "[brew][trigger]") {
    Rig r;
    r.set(TriggerProcessor::kAnyNote, 1.0f);
    r.set(TriggerProcessor::kMode, as_param(TriggerSignal::gate));

    midi::MidiBuffer b;
    auto a = midi::MidiEvent::note_on(0, 60, 100);
    a.sample_offset = 0;
    auto c = midi::MidiEvent::note_on(0, 64, 100);
    c.sample_offset = 4;
    b.add(a);
    b.add(c);
    (void)r.render(32, &b);
    REQUIRE(r.proc().held(0));

    // Releasing the first of two held notes must not close the gate.
    auto off_first = note_off_at(0, 60);
    const auto v = r.render(32, &off_first);
    CHECK(v[31] == 1.0f);
    CHECK(r.proc().held(0));

    auto off_second = note_off_at(0, 64);
    const auto w = r.render(32, &off_second);
    CHECK(w[31] == 0.0f);
    CHECK_FALSE(r.proc().held(0));
}

TEST_CASE("Note selects one note, and Any Note ignores it", "[brew][trigger]") {
    Rig r;
    r.set(TriggerProcessor::kMode, as_param(TriggerSignal::gate));
    r.set(TriggerProcessor::kNote, 60.0f);

    auto wrong = note_on_at(0, 61);
    for (float x : r.render(16, &wrong)) CHECK(x == 0.0f);

    auto right_note = note_on_at(0, 60);
    CHECK(r.render(16, &right_note)[0] == 1.0f);

    // A note-off for a note we never counted must not close the gate.
    auto stray = note_off_at(0, 61);
    CHECK(r.render(16, &stray)[0] == 1.0f);

    Rig any;
    any.set(TriggerProcessor::kMode, as_param(TriggerSignal::gate));
    any.set(TriggerProcessor::kAnyNote, 1.0f);
    auto other = note_on_at(0, 61);
    CHECK(any.render(16, &other)[0] == 1.0f);
}

TEST_CASE("Trigger emits a pulse of Length, once per note", "[brew][trigger]") {
    Rig r;
    r.set(TriggerProcessor::kAnyNote, 1.0f);
    r.set(TriggerProcessor::kMode, as_param(TriggerSignal::trigger));
    r.set(TriggerProcessor::kLengthMs, 1.0f);   // 48 samples

    auto on = note_on_at(0);
    const auto v = r.render(128, &on);
    CHECK(v[0] == 1.0f);
    CHECK(v[47] == 1.0f);
    CHECK(v[48] == 0.0f);
    // The note is still held, and the pulse has not fired again.
    CHECK(v[127] == 0.0f);
    CHECK(r.proc().held(0));

    // A second note retriggers it *while it is still high*, and the pulse restarts
    // from there rather than running out its original count. Across two blocks the
    // pulse has already expired and a plug-in that never retriggered would pass.
    midi::MidiBuffer pair;
    auto first = midi::MidiEvent::note_on(0, 60, 100);
    first.sample_offset = 0;
    auto second = midi::MidiEvent::note_on(0, 64, 100);
    second.sample_offset = 4;
    pair.add(first);
    pair.add(second);

    Rig s;
    s.set(TriggerProcessor::kAnyNote, 1.0f);
    s.set(TriggerProcessor::kMode, as_param(TriggerSignal::trigger));
    s.set(TriggerProcessor::kLengthMs, 1.0f);
    const auto w = s.render(128, &pair);
    CHECK(w[0] == 1.0f);
    CHECK(w[47] == 1.0f);   // would have ended here without the retrigger
    CHECK(w[51] == 1.0f);   // 4 + 48 - 1
    CHECK(w[52] == 0.0f);
}

TEST_CASE("Velocity samples and holds the last note's velocity", "[brew][trigger]") {
    Rig r;
    r.set(TriggerProcessor::kAnyNote, 1.0f);
    r.set(TriggerProcessor::kMode, as_param(TriggerSignal::velocity));

    auto on = note_on_at(0, 60, 64);
    CHECK_THAT(r.render(16, &on)[15], WithinAbs(64.0f / 127.0f, 1e-6f));

    // Held through the note-off: a velocity that fell back to zero would slam
    // whatever it is patched to.
    auto off = note_off_at(0);
    CHECK_THAT(r.render(16, &off)[15], WithinAbs(64.0f / 127.0f, 1e-6f));

    auto harder = note_on_at(0, 60, 127);
    CHECK_THAT(r.render(16, &harder)[15], WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("Envelope reaches the jack through envelope_value_at", "[brew][trigger]") {
    Rig r;
    r.simple_envelope();
    auto on = note_on_at(0, 60, 127);
    const auto v = r.render(1200, &on);
    for (int n = 0; n < 1200; n += 37) {
        const double t = static_cast<double>(n) / kSampleRate;
        INFO("sample " << n);
        CHECK_THAT(v[static_cast<std::size_t>(n)],
                   WithinAbs(r.proc().envelope_value_at(0, t, 1.0f), 2e-4f));
    }
}

TEST_CASE("Vel scales the envelope by the note's velocity", "[brew][trigger]") {
    Rig r;
    r.simple_envelope();
    r.set(TriggerProcessor::kVelocityAmount, 1.0f);

    auto soft = note_on_at(0, 60, 32);
    const float peak_soft = r.render(600, &soft)[479];   // the peak, after 10 ms
    auto hard = note_on_at(0, 60, 127);
    const float peak_hard = r.render(600, &hard)[479];
    CHECK(peak_soft < peak_hard);
    CHECK_THAT(peak_soft, WithinAbs(peak_hard * (32.0f / 127.0f), 2e-3f));

    // Inverted: a soft note opens the jack further than a hard one.
    Rig inv;
    inv.simple_envelope();
    inv.set(TriggerProcessor::kVelocityAmount, -1.0f);
    auto s2 = note_on_at(0, 60, 32);
    const float inv_soft = inv.render(600, &s2)[479];
    auto h2 = note_on_at(0, 60, 127);
    const float inv_hard = inv.render(600, &h2)[479];
    CHECK(inv_soft > inv_hard);
}

TEST_CASE("Voltage Min and Max set the window every signal lands in",
          "[brew][trigger]") {
    Rig r;
    r.set(TriggerProcessor::kAnyNote, 1.0f);
    r.set(TriggerProcessor::kMode, as_param(TriggerSignal::gate));
    r.set(TriggerProcessor::kVoltageMin, -0.4f);
    r.set(TriggerProcessor::kVoltageMax, 0.8f);

    CHECK_THAT(r.render(8)[0], WithinAbs(-0.4f, 1e-6f));
    auto on = note_on_at(0);
    CHECK_THAT(r.render(8, &on)[0], WithinAbs(0.8f, 1e-6f));

    // Min above Max is how an envelope is inverted. There is no Invert toggle.
    r.set(TriggerProcessor::kVoltageMin, 1.0f);
    r.set(TriggerProcessor::kVoltageMax, -1.0f);
    CHECK_THAT(r.render(8)[0], WithinAbs(-1.0f, 1e-6f));   // still held
    auto off = note_off_at(0);
    CHECK_THAT(r.render(8, &off)[7], WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("Override forces the jack and leaves the generators running",
          "[brew][trigger]") {
    Rig r;
    r.simple_envelope();
    r.set(TriggerProcessor::kOverride, 1.0f);
    r.set(TriggerProcessor::kOverrideValue, -0.6f);

    auto on = note_on_at(0, 60, 127);
    const auto v = r.render(2000, &on);
    for (float x : v) CHECK_THAT(x, WithinAbs(-0.6f, 1e-6f));

    // Releasing the override must not step into a frozen envelope: it has been
    // running all along, and by now it is on the sustain.
    r.set(TriggerProcessor::kOverride, 0.0f);
    const auto w = r.render(8);
    CHECK_THAT(w[7], WithinAbs(0.5f, 1e-3f));
}

TEST_CASE("a CV Trigger fires the channel from its own input", "[brew][trigger]") {
    Rig r;
    r.set(TriggerProcessor::kMode, as_param(TriggerSignal::gate));
    r.set(TriggerProcessor::kCvThreshold, 0.5f);

    auto ramp = [](int n, float* l, float*) { *l = n >= 10 ? 1.0f : 0.0f; };

    // Off by default: a drum loop on the input must not open the gate.
    for (float x : r.render(32, nullptr, false, ramp)) CHECK(x == 0.0f);

    r.set(TriggerProcessor::kCvEnable, 1.0f);
    const auto v = r.render(32, nullptr, false, ramp);
    CHECK(v[9] == 0.0f);
    CHECK(v[10] == 1.0f);
    CHECK(r.proc().held(0));

    // It falls when the voltage does, through the hysteresis.
    const auto w = r.render(32, nullptr, false,
                            [](int n, float* l, float*) { *l = n < 4 ? 1.0f : 0.0f; });
    CHECK(w[3] == 1.0f);
    CHECK(w[4] == 0.0f);
}

TEST_CASE("a CV Trigger rides out noise on a slewed edge", "[brew][trigger]") {
    Rig r;
    r.set(TriggerProcessor::kMode, as_param(TriggerSignal::trigger));
    r.set(TriggerProcessor::kCvEnable, 1.0f);
    r.set(TriggerProcessor::kCvThreshold, 0.5f);
    r.set(TriggerProcessor::kLengthMs, 0.1f);   // ~4 samples

    // A wobble that stays above the falling threshold must fire exactly once.
    const auto v = r.render(64, nullptr, false, [](int n, float* l, float*) {
        static const float wobble[8] = {0.0f, 0.6f, 0.4f, 0.7f, 0.45f, 0.9f, 0.3f, 0.8f};
        *l = n < 8 ? wobble[n] : 1.0f;
    });
    int rises = 0;
    for (std::size_t n = 1; n < v.size(); ++n)
        if (v[n] > 0.5f && v[n - 1] <= 0.5f) ++rises;
    CHECK(rises == 1);
}

TEST_CASE("turning the CV Trigger off releases what it was holding",
          "[brew][trigger]") {
    Rig r;
    r.set(TriggerProcessor::kMode, as_param(TriggerSignal::gate));
    r.set(TriggerProcessor::kCvEnable, 1.0f);
    auto high = [](int, float* l, float*) { *l = 1.0f; };
    CHECK(r.render(16, nullptr, false, high)[15] == 1.0f);
    r.set(TriggerProcessor::kCvEnable, 0.0f);
    CHECK(r.render(16, nullptr, false, high)[15] == 0.0f);
    CHECK_FALSE(r.proc().held(0));
}

TEST_CASE("re-enabling the CV Trigger on a high input fires it again",
          "[brew][trigger]") {
    // Switching the input off has to *release* the gate, not leave it latched. A
    // latched gate would report the channel held the instant the input came back,
    // with no rising edge — so the envelope and the pulse would never fire again.
    Rig r;
    r.set(TriggerProcessor::kMode, as_param(TriggerSignal::trigger));
    r.set(TriggerProcessor::kCvEnable, 1.0f);
    r.set(TriggerProcessor::kLengthMs, 0.1f);   // ~4 samples
    auto high = [](int, float* l, float*) { *l = 1.0f; };

    REQUIRE(r.render(64, nullptr, false, high)[0] == 1.0f);   // fired once
    r.set(TriggerProcessor::kCvEnable, 0.0f);
    (void)r.render(64, nullptr, false, high);
    r.set(TriggerProcessor::kCvEnable, 1.0f);
    CHECK(r.render(64, nullptr, false, high)[0] == 1.0f);     // and fires again
}

TEST_CASE("Override cannot be pushed outside the rails", "[brew][trigger][safety]") {
    // The store clamps a write to the parameter's own range, and that range is the
    // jack's. A host asking for two volts gets one.
    Rig r;
    r.set(TriggerProcessor::kOverride, 1.0f);
    r.set(TriggerProcessor::kOverrideValue, 4.0f);
    CHECK(r.render(8)[0] == 1.0f);
    r.set(TriggerProcessor::kOverrideValue, -4.0f);
    CHECK(r.render(8)[0] == -1.0f);
}

TEST_CASE("MIDI and CV are two ways in, not one gated by the other",
          "[brew][trigger]") {
    Rig r;
    r.set(TriggerProcessor::kMode, as_param(TriggerSignal::gate));
    r.set(TriggerProcessor::kAnyNote, 1.0f);
    r.set(TriggerProcessor::kCvEnable, 1.0f);

    auto on = note_on_at(0);
    CHECK(r.render(16, &on)[15] == 1.0f);   // MIDI alone

    // The note lets go, but the input is holding: the gate stays open.
    auto off = note_off_at(0);
    CHECK(r.render(16, &off, false, [](int, float* l, float*) { *l = 1.0f; })[15] == 1.0f);
    // Both let go.
    CHECK(r.render(16)[15] == 0.0f);
}

TEST_CASE("Smooth slews the gate edge, and zero is a wire", "[brew][trigger]") {
    Rig r;
    r.set(TriggerProcessor::kAnyNote, 1.0f);
    r.set(TriggerProcessor::kMode, as_param(TriggerSignal::gate));

    auto on = note_on_at(0);
    CHECK(r.render(8, &on)[0] == 1.0f);

    Rig s;
    s.set(TriggerProcessor::kAnyNote, 1.0f);
    s.set(TriggerProcessor::kMode, as_param(TriggerSignal::gate));
    s.set(TriggerProcessor::kSmoothMs, 100.0f);
    auto on2 = note_on_at(0);
    const auto v = s.render(64, &on2);
    CHECK(v[0] < 1.0f);
    CHECK(v[63] > v[0]);
    CHECK(v[63] < 1.0f);
}

TEST_CASE("the two channels are independent", "[brew][trigger]") {
    Rig r;
    r.set_ch(TriggerProcessor::kAnyNote, 0, 1.0f);
    r.set_ch(TriggerProcessor::kAnyNote, 1, 1.0f);
    r.set_ch(TriggerProcessor::kMode, 0, as_param(TriggerSignal::gate));
    r.set_ch(TriggerProcessor::kMode, 1, as_param(TriggerSignal::trigger));
    r.set_ch(TriggerProcessor::kLengthMs, 1, 1.0f);   // 48 samples

    auto on = note_on_at(0);
    const auto left = r.render(128, &on);
    CHECK(left[127] == 1.0f);      // a gate holds
    CHECK(r.right[47] == 1.0f);    // a pulse does not
    CHECK(r.right[48] == 0.0f);
}

TEST_CASE("a bypassed generator is silent on both jacks", "[brew][trigger][safety]") {
    Rig r;
    r.set(TriggerProcessor::kAnyNote, 1.0f);
    auto on = note_on_at(0);
    (void)r.render(16, &on);
    REQUIRE(r.proc().held(0));

    const auto v = r.render(16, nullptr, /*bypassed=*/true);
    for (float x : v) CHECK(x == 0.0f);
    for (float x : r.right) CHECK(x == 0.0f);
    CHECK_FALSE(r.proc().held(0));
}

TEST_CASE("display_stage reports where the envelope is", "[brew][trigger]") {
    Rig r;
    r.simple_envelope();
    CHECK(r.proc().display_stage(0) == Stage::idle);
    auto on = note_on_at(0, 60, 127);
    (void)r.render(2000, &on);
    CHECK(r.proc().display_stage(0) == Stage::sustain);
    auto off = note_off_at(0);
    (void)r.render(2000, &off);
    CHECK(r.proc().display_stage(0) == Stage::idle);
}

TEST_CASE("a zero-length block is survivable", "[brew][trigger][safety]") {
    Rig r;
    CHECK(r.render(0).empty());
}
