// Step LFO — the pattern, and the two properties a position-derived sequencer
// must have: it lands on the same step however the playhead arrived, and its
// randomness repeats on a re-render.
//
// Scope note: `Processor::process()` and the pure step functions. Nothing here
// proves a step becomes a voltage at a jack.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "step_processor.hpp"
#include <pulp/format/headless.hpp>

#include <cmath>
#include <set>
#include <vector>

using namespace pulp;
using namespace pulp::examples::brew;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kTempo = 120.0;

struct Rig {
    Rig() { host.prepare(kSampleRate, 4096, 2, 2); }

    state::StateStore& state() { return host.state(); }
    const StepProcessor& proc() const {
        return *static_cast<const StepProcessor*>(host.processor());
    }

    /// Render one block, returning the CV channel.
    std::vector<float> render(double position_beats, int frames = 512,
                              bool playing = true, bool bypassed = false) {
        audio::Buffer<float> in(2, static_cast<std::size_t>(frames));
        audio::Buffer<float> out(2, static_cast<std::size_t>(frames));
        in.clear();
        out.clear();
        const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
        audio::BufferView<const float> iv(ip, 2, static_cast<std::size_t>(frames));
        auto ov = out.view();

        format::ProcessContext ctx;
        ctx.sample_rate = kSampleRate;
        ctx.num_samples = frames;
        ctx.is_playing = playing;
        ctx.is_bypassed = bypassed;
        ctx.tempo_bpm = kTempo;
        ctx.position_beats = position_beats;
        host.process(ov, iv, ctx);

        gate.assign(out.channel(1).begin(), out.channel(1).end());
        return std::vector<float>(out.channel(0).begin(), out.channel(0).end());
    }

    /// Set the eight levels to distinct, easily-recognised values.
    void set_ramp() {
        for (int i = 0; i < kMaxSequencerSteps; ++i)
            state().set_value(StepProcessor::step_param(i),
                              -1.0f + 0.25f * static_cast<float>(i));
    }

    format::HeadlessHost host{create_step};
    std::vector<float> gate;
};

}  // namespace

TEST_CASE("Step LFO descriptor emits a pattern and a gate", "[brew][step]") {
    auto proc = create_step();
    const auto desc = proc->descriptor();
    REQUIRE(desc.name == "Step LFO");
    REQUIRE(desc.output_buses.size() == 1);
    REQUIRE(desc.output_buses[0].default_channels == 2);
}

// ------------------------------------------------------------------ speed mode

TEST_CASE("speed mode decides whether length changes the pattern's period",
          "[brew][step][speed]") {
    // Cycle: the pattern occupies the rate, so halving its length halves the step.
    REQUIRE(beats_per_step(SpeedMode::cycle, 4.0, 8) == 0.5);
    REQUIRE(beats_per_step(SpeedMode::cycle, 4.0, 4) == 1.0);

    // Step: each step occupies the rate, so length does not touch it at all.
    REQUIRE(beats_per_step(SpeedMode::step, 0.5, 8) == 0.5);
    REQUIRE(beats_per_step(SpeedMode::step, 0.5, 4) == 0.5);

    // Degenerate settings yield zero, which callers read as "no pattern".
    REQUIRE(beats_per_step(SpeedMode::cycle, 0.0, 8) == 0.0);
    REQUIRE(beats_per_step(SpeedMode::cycle, 4.0, 0) == 0.0);
}

TEST_CASE("the two speed modes are not a rescaling of each other",
          "[brew][step][speed]") {
    // If they were, one would be redundant. In cycle mode the step duration
    // depends on the length; in step mode it does not. Pin the difference.
    Rig rig;
    rig.set_ramp();
    rig.state().set_value(StepProcessor::kRate, 4.0f);

    rig.state().set_value(StepProcessor::kSpeedMode, 0.0f);  // cycle
    rig.state().set_value(StepProcessor::kLength, 8.0f);
    const double cycle_8 = rig.proc().step_beats();
    rig.state().set_value(StepProcessor::kLength, 4.0f);
    const double cycle_4 = rig.proc().step_beats();
    REQUIRE(cycle_4 == 2.0 * cycle_8);

    rig.state().set_value(StepProcessor::kSpeedMode, 1.0f);  // per step
    const double step_4 = rig.proc().step_beats();
    rig.state().set_value(StepProcessor::kLength, 8.0f);
    REQUIRE(rig.proc().step_beats() == step_4);
}

// -------------------------------------------------------------- position rule

TEST_CASE("the step index is a pure function of position", "[brew][step][safety]") {
    // The whole design. Bar 57 plays the step bar 57 plays, whatever the playhead
    // did to get there — so a bounce is reproducible and a locate lands right.
    Rig played, located;
    played.set_ramp();
    located.set_ramp();
    played.state().set_value(StepProcessor::kSpeedMode, 1.0f);
    located.state().set_value(StepProcessor::kSpeedMode, 1.0f);
    played.state().set_value(StepProcessor::kRate, 0.25f);
    located.state().set_value(StepProcessor::kRate, 0.25f);

    constexpr int kFrames = 512;
    const double span = (kTempo / (60.0 * kSampleRate)) * kFrames;

    std::vector<float> from_playing;
    for (int b = 0; b <= 40; ++b) from_playing = played.render(span * b, kFrames);
    const auto from_locate = located.render(span * 40.0, kFrames);

    REQUIRE(from_playing == from_locate);
}

TEST_CASE("a negative position reads a step, not memory before the array",
          "[brew][step][safety]") {
    // Hosts do report negative beats (a count-in, a pickup bar). `%` on a negative
    // index would return a negative step and index outside the pattern.
    REQUIRE(wrap_index(-1, 8) == 7);
    REQUIRE(wrap_index(-8, 8) == 0);
    REQUIRE(wrap_index(-9, 8) == 7);
    REQUIRE(wrap_index(3, 0) == 0);

    Rig rig;
    rig.set_ramp();
    const auto out = rig.render(-3.7, 256);
    for (float v : out) {
        REQUIRE(std::isfinite(v));
        REQUIRE(v >= -1.0f);
        REQUIRE(v <= 1.0f);
    }
}

TEST_CASE("the pattern wraps at its length, not at eight",
          "[brew][step][safety]") {
    Rig rig;
    rig.set_ramp();
    rig.state().set_value(StepProcessor::kSpeedMode, 1.0f);
    rig.state().set_value(StepProcessor::kRate, 1.0f);
    rig.state().set_value(StepProcessor::kLength, 3.0f);

    // Steps 0,1,2 then back to 0. Sample the middle of each of four beats.
    const auto& p = rig.proc();
    REQUIRE_THAT(p.value_at(0.5), WithinAbs(p.value_at(3.5), 1e-6f));
    REQUIRE_THAT(p.value_at(1.5), WithinAbs(p.value_at(4.5), 1e-6f));
    // And the fourth programmed step never plays.
    REQUIRE(p.value_at(3.5) != rig.state().get_value(StepProcessor::step_param(3)));
}

// ------------------------------------------------------------------ randomness

TEST_CASE("random is a hash of the absolute step, so a bounce repeats",
          "[brew][step][random]") {
    Rig a, b;
    a.set_ramp();
    b.set_ramp();
    for (auto* r : {&a, &b}) {
        r->state().set_value(StepProcessor::kRandom, 0.8f);
        r->state().set_value(StepProcessor::kSeed, 42.0f);
        r->state().set_value(StepProcessor::kSpeedMode, 1.0f);
        r->state().set_value(StepProcessor::kRate, 0.25f);
    }
    // Two independent instances, same timeline, must agree sample for sample.
    REQUIRE(a.render(12.5, 512) == b.render(12.5, 512));

    // And a re-render of the same block agrees with itself — no hidden state.
    REQUIRE(a.render(12.5, 512) == a.render(12.5, 512));
}

TEST_CASE("random does not repeat every pattern cycle", "[brew][step][random]") {
    // Keyed on the *absolute* step, not the wrapped one: the pattern's shape
    // loops, its dither does not. Keying on the wrapped index would make an
    // 8-step pattern with random exactly 8 steps long, which is the bug.
    std::set<float> values;
    for (std::int64_t i = 0; i < 64; ++i) values.insert(step_random(i, 7));
    REQUIRE(values.size() == 64);

    // The same wrapped index across four cycles gives four different values.
    REQUIRE(step_random(0, 7) != step_random(8, 7));
    REQUIRE(step_random(8, 7) != step_random(16, 7));
}

TEST_CASE("the processor keys random on the absolute step, not the wrapped one",
          "[brew][step][random]") {
    // Testing `step_random` alone does not pin this: what matters is which index
    // `level_at` hands it. Keyed on the wrapped index, a 4-long pattern would
    // repeat its dither every 4 steps and `Random` would just be a fixed offset
    // baked into each step.
    Rig rig;
    // Park every step at zero, not on the ramp. `step_value` adds the dither and
    // clamps to the rails, so a step programmed at -1.0 swallows every negative
    // dither — two different random values would compare equal there, and the
    // test would fail for a reason that has nothing to do with the keying.
    for (int i = 0; i < kMaxSequencerSteps; ++i)
        rig.state().set_value(StepProcessor::step_param(i), 0.0f);
    rig.state().set_value(StepProcessor::kRandom, 0.5f);
    rig.state().set_value(StepProcessor::kSeed, 5.0f);
    rig.state().set_value(StepProcessor::kLength, 4.0f);

    const auto& p = rig.proc();
    // Absolute steps 0 and 4 are the same *pattern* step, so their programmed
    // levels agree — but their dither must not.
    REQUIRE(p.level_at(0) != p.level_at(4));
    REQUIRE(p.level_at(1) != p.level_at(5));

    // With random off they agree exactly, which is what makes the check above
    // about the dither rather than about the levels.
    rig.state().set_value(StepProcessor::kRandom, 0.0f);
    REQUIRE(p.level_at(0) == p.level_at(4));
}

TEST_CASE("random at zero leaves the pattern exactly as programmed",
          "[brew][step][random]") {
    // A randomness control that alters the pattern at zero is unusable.
    for (std::int64_t i = -4; i < 8; ++i) {
        CAPTURE(i);
        REQUIRE(step_value(0.375f, i, 0.0f, 99) == 0.375f);
        REQUIRE(step_value(0.375f, i, -1.0f, 99) == 0.375f);
    }
    // And a non-zero amount does move it.
    REQUIRE(step_value(0.375f, 3, 0.5f, 99) != 0.375f);
}

TEST_CASE("a random step never leaves the rails", "[brew][step][safety]") {
    for (std::int64_t i = -100; i < 100; ++i)
        for (float programmed : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
            const float v = step_value(programmed, i, 1.0f, 3);
            CAPTURE(i, programmed, v);
            REQUIRE(v >= -1.0f);
            REQUIRE(v <= 1.0f);
        }
}

TEST_CASE("a different seed rerolls the sequence", "[brew][step][random]") {
    bool any_differ = false;
    for (std::int64_t i = 0; i < 16; ++i)
        if (step_random(i, 1) != step_random(i, 2)) any_differ = true;
    REQUIRE(any_differ);
}

// ----------------------------------------------------------------- glide, gate

TEST_CASE("glide slides into a step and then rests on it", "[brew][step][glide]") {
    // Slewing across the whole step would mean a step never arrives at the level
    // it was programmed to: that is a triangle wave with extra parameters.
    REQUIRE(glide_toward(-1.0f, 1.0f, 0.0, 0.5) == -1.0f);
    REQUIRE_THAT(glide_toward(-1.0f, 1.0f, 0.25, 0.5), WithinAbs(0.0f, 1e-6f));
    REQUIRE(glide_toward(-1.0f, 1.0f, 0.5, 0.5) == 1.0f);
    REQUIRE(glide_toward(-1.0f, 1.0f, 0.9, 0.5) == 1.0f);

    // Zero glide is a hard edge at the boundary and nowhere else.
    REQUIRE(glide_toward(-1.0f, 1.0f, 0.0, 0.0) == 1.0f);
}

TEST_CASE("the processor glides from the step before, not from itself",
          "[brew][step][glide]") {
    // `glide_toward` being correct says nothing about which two levels the
    // processor feeds it. Handing it the current step twice makes glide a no-op
    // that no test of the pure function can see.
    Rig rig;
    rig.set_ramp();
    rig.state().set_value(StepProcessor::kSpeedMode, 1.0f);
    rig.state().set_value(StepProcessor::kRate, 1.0f);
    rig.state().set_value(StepProcessor::kGlide, 0.5f);

    const auto& p = rig.proc();
    const float step1 = rig.state().get_value(StepProcessor::step_param(1));
    const float step2 = rig.state().get_value(StepProcessor::step_param(2));
    REQUIRE(step1 != step2);

    // At the instant step 2 begins, the output is still sitting on step 1's level
    // and has not yet started to move.
    REQUIRE_THAT(p.value_at(2.0), WithinAbs(step1, 1e-6f));
    // A quarter of the way through the glide, it is halfway between them.
    REQUIRE_THAT(p.value_at(2.25), WithinAbs(0.5f * (step1 + step2), 1e-6f));
    // Past the glide, it has arrived.
    REQUIRE_THAT(p.value_at(2.75), WithinAbs(step2, 1e-6f));
}

TEST_CASE("the gate is high for the first half of every step",
          "[brew][step][gate]") {
    REQUIRE(step_gate(0.0));
    REQUIRE(step_gate(0.499));
    REQUIRE_FALSE(step_gate(0.5));
    REQUIRE_FALSE(step_gate(0.999));

    // Through the processor: an edge downstream on every step.
    Rig rig;
    rig.set_ramp();
    rig.state().set_value(StepProcessor::kSpeedMode, 1.0f);
    rig.state().set_value(StepProcessor::kRate, 0.0625f);  // a step per 31.25 ms
    rig.render(0.0, 4096);
    bool saw_high = false, saw_low = false;
    for (float g : rig.gate) (g > 0.5f ? saw_high : saw_low) = true;
    REQUIRE(saw_high);
    REQUIRE(saw_low);
}

TEST_CASE("step_fraction never reaches one", "[brew][step][safety]") {
    // A fraction of exactly 1.0 places a sample in the next step while the index
    // says otherwise — the two disagree, and the gate flickers at the boundary.
    //
    // `x - floor(x)` is exact for positive x, so no positive position can trip
    // this. A position a hair *below* zero can: floor is -1, and `x - (-1)` rounds
    // up to exactly 1.0. Hosts report exactly that during a count-in.
    REQUIRE(step_fraction(-1e-30, 1.0) < 1.0);
    REQUIRE(step_fraction(-1e-18, 0.25) < 1.0);
    for (double b : {0.9999999999, 1.9999999999, 7.999999999999999})
        REQUIRE(step_fraction(b, 1.0) < 1.0);
    REQUIRE(step_fraction(0.0, 0.0) == 0.0);
}

// ---------------------------------------------------------------------- output

TEST_CASE("Step LFO holds both outputs at zero when bypassed",
          "[brew][step][bypass]") {
    // A generator bypassed is a generator switched off, not one frozen at
    // whatever voltage its pattern happened to reach.
    Rig rig;
    rig.set_ramp();
    const auto out = rig.render(1.3, 256, /*playing=*/true, /*bypassed=*/true);
    for (float v : out) REQUIRE(v == 0.0f);
    for (float g : rig.gate) REQUIRE(g == 0.0f);
    REQUIRE(rig.proc().display_step() == -1);
}

TEST_CASE("a stopped transport holds the step under the playhead",
          "[brew][step]") {
    Rig rig;
    rig.set_ramp();
    rig.state().set_value(StepProcessor::kSpeedMode, 1.0f);
    rig.state().set_value(StepProcessor::kRate, 1.0f);
    const auto out = rig.render(2.5, 512, /*playing=*/false);
    for (float v : out) REQUIRE(v == out.front());
    REQUIRE_THAT(out.front(),
                 WithinAbs(rig.state().get_value(StepProcessor::step_param(2)), 1e-6f));
}

TEST_CASE("output scale and invert reach the pattern", "[brew][step]") {
    Rig rig;
    rig.set_ramp();
    rig.state().set_value(StepProcessor::kSpeedMode, 1.0f);
    rig.state().set_value(StepProcessor::kRate, 1.0f);
    const float full = rig.proc().value_at(0.5);

    rig.state().set_value(StepProcessor::kOutputScale, 0.5f);
    REQUIRE_THAT(rig.proc().value_at(0.5), WithinAbs(0.5f * full, 1e-6f));

    rig.state().set_value(StepProcessor::kOutputScale, 1.0f);
    rig.state().set_value(StepProcessor::kInvert, 1.0f);
    REQUIRE_THAT(rig.proc().value_at(0.5), WithinAbs(-full, 1e-6f));
}

TEST_CASE("Step LFO publishes the playing step for the editor", "[brew][step]") {
    Rig rig;
    rig.set_ramp();
    rig.state().set_value(StepProcessor::kSpeedMode, 1.0f);
    rig.state().set_value(StepProcessor::kRate, 1.0f);
    rig.state().set_value(StepProcessor::kLength, 4.0f);
    rig.render(6.2, 64);
    REQUIRE(rig.proc().display_step() == 2);  // step 6 wraps to 2 in a 4-long pattern
}
