// Quantizer — the staircase, and the properties a discrete CV must not break.
//
// Scope note: `Processor::process()` and the pure transfer only. Nothing here
// proves a sample becomes a real voltage, and nothing here quantizes to musical
// semitones — that needs the interface's full-scale voltage, which is unmeasured.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "quantizer_processor.hpp"
#include <pulp/format/headless.hpp>

#include <cmath>
#include <algorithm>
#include <set>
#include <vector>

using namespace pulp;
using namespace pulp::examples::brew;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<float> shape(format::HeadlessHost& host, const std::vector<float>& in_samples,
                         const format::ProcessContext* ctx = nullptr) {
    const auto n = in_samples.size();
    audio::Buffer<float> in(2, n), out(2, n);
    in.clear();
    out.clear();
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < n; ++i) in.channel(c)[i] = in_samples[i];
    const float* ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ptrs, 2, n);
    auto ov = out.view();
    if (ctx) host.process(ov, iv, *ctx); else host.process(ov, iv);
    return std::vector<float>(out.channel(0).begin(), out.channel(0).end());
}

/// True when `v` sits on a multiple of `w`. `std::fmod` returns a remainder just
/// *under* the divisor when the value is a hair below an exact multiple, so the
/// distance to the nearest multiple is the smaller of the remainder and its
/// complement — not the remainder alone.
bool on_lattice(float v, double w) {
    const double r = std::fmod(std::abs(static_cast<double>(v)), w);
    return std::min(r, w - r) < 1e-6;
}

std::vector<float> ramp(int n) {
    std::vector<float> v;
    v.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v.push_back(-1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(n - 1));
    return v;
}

}  // namespace

TEST_CASE("Quantizer descriptor is a two-channel CV insert", "[brew][quantizer]") {
    auto proc = create_quantizer();
    const auto desc = proc->descriptor();
    REQUIRE(desc.name == "Quantizer");
    REQUIRE_FALSE(desc.accepts_midi);
    REQUIRE(desc.input_buses.size() == 1);
}

// A ramp through the whole range must emerge with exactly as many distinct values
// as there are lattice points inside it. This is the definition of the plug-in,
// and it is the assertion a subtly-wrong rounding rule fails.
TEST_CASE("a ramp becomes exactly N+1 distinct levels", "[brew][quantizer]") {
    QuantizeSettings s;
    s.steps = 8.0f;   // lattice at -1, -0.75, ..., +1 → nine points
    std::set<float> levels;
    for (float x : ramp(4001)) levels.insert(quantize_transfer(x, s));
    REQUIRE(levels.size() == 9);

    // And each is an exact multiple of the step width, not merely close to one.
    for (float v : levels) {
        CAPTURE(v);
        REQUIRE(on_lattice(v, step_width(8.0f)));
    }
}

// Symmetry about the origin. `std::round` breaks ties away from zero, so a value
// halfway between two steps would jump outward above zero and inward below it —
// putting a DC offset on a bipolar CV that no amount of scaling removes.
TEST_CASE("the staircase is symmetric about zero", "[brew][quantizer][safety]") {
    QuantizeSettings s;
    s.steps = 10.0f;
    for (double x = 0.0; x <= 1.0; x += 0.001) {
        CAPTURE(x);
        REQUIRE_THAT(quantize_value(-x, s), WithinAbs(-quantize_value(x, s), 1e-12));
    }
    // Including the exact tie: half a step above zero and half a step below. This
    // is the one input where the rounding rule shows itself, and the one that
    // `std::floor(t + 0.5)` gets wrong.
    const double w = step_width(10.0f);
    REQUIRE_THAT(quantize_value(0.5 * w, s), WithinAbs(w, 1e-12));
    REQUIRE_THAT(quantize_value(-0.5 * w, s), WithinAbs(-w, 1e-12));
}

TEST_CASE("zero passes through untouched at the default offset",
          "[brew][quantizer]") {
    QuantizeSettings s;
    for (float steps : {2.0f, 3.0f, 12.0f, 64.0f}) {
        s.steps = steps;
        CAPTURE(steps);
        REQUIRE(quantize_value(0.0, s) == 0.0);
    }
    // Offset moves the lattice off zero, which is exactly what it is for.
    s.steps = 12.0f;
    s.offset = 0.5f;
    REQUIRE(quantize_value(0.0, s) != 0.0);
}

// The rails are lattice points, so full scale in is full scale out. A quantizer
// that could not reach the rail would silently attenuate every signal fed to it.
TEST_CASE("the rails survive quantization", "[brew][quantizer]") {
    QuantizeSettings s;
    for (float steps : {2.0f, 5.0f, 12.0f, 64.0f}) {
        s.steps = steps;
        CAPTURE(steps);
        REQUIRE_THAT(quantize_value(1.0, s), WithinAbs(1.0, 1e-12));
        REQUIRE_THAT(quantize_value(-1.0, s), WithinAbs(-1.0, 1e-12));
    }
}

// Transpose shifts by whole steps, so its output is still on the lattice. A
// transpose expressed in volts would land between treads and undo the plug-in.
TEST_CASE("transpose moves by exact steps and stays on the lattice",
          "[brew][quantizer]") {
    QuantizeSettings s;
    s.steps = 8.0f;
    const double w = step_width(8.0f);

    QuantizeSettings up = s;
    up.transpose = 2.0f;
    // Below the rail, a two-step transpose is exactly two step widths.
    REQUIRE_THAT(quantize_value(0.0, up), WithinAbs(2.0 * w, 1e-12));
    REQUIRE_THAT(quantize_value(-0.5, up), WithinAbs(quantize_value(-0.5, s) + 2.0 * w, 1e-12));

    // And it clamps at the rail rather than wrapping or overshooting.
    up.transpose = 24.0f;
    REQUIRE(quantize_value(0.0, up) == 1.0);
}

// Never leaves the rails, whatever the knobs say.
TEST_CASE("Quantizer never leaves [-1, +1]", "[brew][quantizer][safety]") {
    for (float steps : {2.0f, 7.0f, 64.0f})
        for (float offset : {0.0f, 0.37f, 1.0f})
            for (float transpose : {-24.0f, 0.0f, 24.0f})
                for (float x : ramp(33)) {
                    QuantizeSettings s{.steps = steps,
                                       .offset = offset,
                                       .transpose = transpose};
                    CAPTURE(steps, offset, transpose, x);
                    const float y = quantize_transfer(x, s);
                    REQUIRE(y >= -1.0f);
                    REQUIRE(y <= 1.0f);
                    REQUIRE(std::isfinite(y));
                }
}

// A degenerate step count is a division by zero waiting to happen.
TEST_CASE("step count is clamped away from degeneracy", "[brew][quantizer][safety]") {
    REQUIRE(step_width(0.0f) == step_width(kMinQuantizeSteps));
    REQUIRE(step_width(-5.0f) == step_width(kMinQuantizeSteps));
    REQUIRE(step_width(1e6f) == step_width(kMaxQuantizeSteps));
    QuantizeSettings s;
    s.steps = 0.0f;
    REQUIRE(std::isfinite(quantize_value(0.3, s)));
}

// Bypass on an insert is a wire, not a mute.
TEST_CASE("Quantizer passes its input through while bypassed",
          "[brew][quantizer][bypass]") {
    format::HeadlessHost host(create_quantizer);
    host.prepare(48000.0, 512, 2, 2);
    const auto in = ramp(32);

    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 32;
    ctx.is_bypassed = true;
    REQUIRE(shape(host, in, &ctx) == in);

    ctx.is_bypassed = false;
    const auto out = shape(host, in, &ctx);
    REQUIRE(out != in);
    // Every emitted value sits on a lattice point of the default 12-step grid.
    for (float v : out) {
        CAPTURE(v);
        REQUIRE(on_lattice(v, step_width(12.0f)));
    }
}

TEST_CASE("Quantizer publishes its operating point for the editor",
          "[brew][quantizer]") {
    format::HeadlessHost host(create_quantizer);
    host.prepare(48000.0, 512, 2, 2);
    host.state().set_value(QuantizerProcessor::kSteps, 4.0f);

    const auto* proc = static_cast<const QuantizerProcessor*>(host.processor());
    shape(host, {0.0f, 0.1f, 0.6f});
    REQUIRE_THAT(proc->display_input(), WithinAbs(0.6f, 1e-6f));
    // Four steps over [-1,1] → width 0.5 → 0.6 snaps to 0.5.
    REQUIRE_THAT(proc->display_output(), WithinAbs(0.5f, 1e-6f));
}

// ---------------------------------------------------------------- scales
//
// A scale quantizer is usually said to need calibration, because a semitone is a
// fixed voltage. That is true of Calibrated mode. It is not true of the scale:
// once twelve lattice steps span an octave, restricting the lattice to a mode is
// arithmetic on the step index. Nothing below mentions volts.

TEST_CASE("scale masks name the right notes", "[brew][quantizer][scale]") {
    // Semitones above the root. These are definitions, not measurements: a
    // regression here means the mask literals were transcribed wrong.
    const auto degrees = [](Scale s) {
        std::vector<int> v;
        for (int i = 0; i < kSemitonesPerOctave; ++i)
            if (scale_admits(s, i)) v.push_back(i);
        return v;
    };
    REQUIRE(degrees(Scale::chromatic) ==
            std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
    REQUIRE(degrees(Scale::major) == std::vector<int>{0, 2, 4, 5, 7, 9, 11});
    REQUIRE(degrees(Scale::natural_minor) == std::vector<int>{0, 2, 3, 5, 7, 8, 10});
    REQUIRE(degrees(Scale::harmonic_minor) == std::vector<int>{0, 2, 3, 5, 7, 8, 11});
    REQUIRE(degrees(Scale::pentatonic_major) == std::vector<int>{0, 2, 4, 7, 9});
    REQUIRE(degrees(Scale::pentatonic_minor) == std::vector<int>{0, 3, 5, 7, 10});
    REQUIRE(degrees(Scale::blues) == std::vector<int>{0, 3, 5, 6, 7, 10});
    REQUIRE(degrees(Scale::whole_tone) == std::vector<int>{0, 2, 4, 6, 8, 10});
    REQUIRE(degrees(Scale::dorian) == std::vector<int>{0, 2, 3, 5, 7, 9, 10});
    REQUIRE(degrees(Scale::mixolydian) == std::vector<int>{0, 2, 4, 5, 7, 9, 10});

    REQUIRE(scale_degree_count(Scale::major) == 7);
    REQUIRE(scale_degree_count(Scale::pentatonic_minor) == 5);
    REQUIRE(scale_degree_count(Scale::chromatic) == 12);
}

TEST_CASE("floor_mod is Euclidean, because step indices go negative",
          "[brew][quantizer][scale]") {
    REQUIRE(floor_mod(-1, 12) == 11);
    REQUIRE(floor_mod(-12, 12) == 0);
    REQUIRE(floor_mod(-13, 12) == 11);
    REQUIRE(floor_mod(13, 12) == 1);
}

TEST_CASE("snap_to_scale finds the nearest note, ties low",
          "[brew][quantizer][scale]") {
    // C major from a root of 0. Semitone 1 (C#) sits between C and D, one step
    // from each; the tie goes to the lower.
    REQUIRE(snap_to_scale(0, Scale::major, 0) == 0);
    REQUIRE(snap_to_scale(1, Scale::major, 0) == 0);
    REQUIRE(snap_to_scale(3, Scale::major, 0) == 2);  // D# -> D (tie, low)
    REQUIRE(snap_to_scale(6, Scale::major, 0) == 5);  // F# -> F (tie, low)
    REQUIRE(snap_to_scale(11, Scale::major, 0) == 11);

    SECTION("below zero, where floor_mod earns its keep") {
        REQUIRE(snap_to_scale(-1, Scale::major, 0) == -1);  // B, in the scale
        REQUIRE(snap_to_scale(-2, Scale::major, 0) == -3);  // A# -> A (tie, low)
    }

    SECTION("a root other than zero moves the whole lattice") {
        // D major admits D, E, F#... so semitone 2 is in, 3 is not.
        REQUIRE(snap_to_scale(2, Scale::major, 2) == 2);
        REQUIRE(snap_to_scale(3, Scale::major, 2) == 2);
    }
}

// The manual's own example: "for a normal major scale, a Transpose value of +7
// will transpose up by an octave."
TEST_CASE("transpose moves by scale degrees, so +7 in a major scale is an octave",
          "[brew][quantizer][scale]") {
    REQUIRE(transpose_in_scale(0, Scale::major, 0, 7) == 12);
    REQUIRE(transpose_in_scale(0, Scale::major, 0, -7) == -12);
    REQUIRE(transpose_in_scale(0, Scale::major, 0, 1) == 2);   // C -> D
    REQUIRE(transpose_in_scale(4, Scale::major, 0, 1) == 5);   // E -> F, a semitone
    REQUIRE(transpose_in_scale(0, Scale::major, 0, 0) == 0);

    SECTION("a five-note scale needs five degrees to make an octave") {
        REQUIRE(transpose_in_scale(0, Scale::pentatonic_minor, 0, 5) == 12);
        REQUIRE(transpose_in_scale(0, Scale::pentatonic_minor, 0, 1) == 3);
    }

    SECTION("an out-of-scale start is snapped before it is moved") {
        REQUIRE(transpose_in_scale(1, Scale::major, 0, 1) == 2);  // snaps to C, then D
    }

    SECTION("chromatic transposes by semitones, as a twelve-note scale must") {
        REQUIRE(transpose_in_scale(0, Scale::chromatic, 0, 7) == 7);
        REQUIRE(transpose_in_scale(0, Scale::chromatic, 0, 12) == 12);
    }
}

TEST_CASE("scale mode restricts the staircase to the scale's notes",
          "[brew][quantizer][scale]") {
    QuantizeSettings s;
    s.mode = QuantMode::scale;
    s.steps = 24.0f;  // 24 divisions of [-1,+1] -> one octave per full-scale unit
    s.scale = Scale::major;

    // Sweep the whole range and check every output lands on a major-scale step.
    const double w = step_width(s.steps);
    for (float x : ramp(97)) {
        const double y = quantize_value(static_cast<double>(x), s);
        const int i = static_cast<int>(std::lround(y / w));
        CAPTURE(x, y, i);
        // The rails are clamps, not lattice points, so let them through.
        if (std::abs(y) >= 1.0 - 1e-9) continue;
        REQUIRE(scale_admits(Scale::major, i));
    }
}

// -------------------------------------------- Enable, Smooth, and two channels

namespace {

state::ParamID qpid(state::ParamID id, std::size_t ch) {
    return static_cast<state::ParamID>(param_for(id, ch));
}

/// Render a constant on both inputs; return the last sample of each output.
std::pair<float, float> both_channels(format::HeadlessHost& host, float level) {
    constexpr std::size_t n = 16;
    audio::Buffer<float> in(2, n), out(2, n);
    in.clear();
    out.clear();
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < n; ++i) in.channel(c)[i] = level;
    const float* ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ptrs, 2, n);
    auto ov = out.view();
    host.process(ov, iv);
    return {out.channel(0)[n - 1], out.channel(1)[n - 1]};
}

}  // namespace

// A control missing from `controls()` does not fail to compile — it simply never
// gets registered, and `get_value` then reads zero. That is how Output Scale
// vanished from this plug-in during the stereo split: the staircase went silent
// and only the operating-point test noticed. Parameter IDs are contiguous from 1,
// so the table can be checked against them directly.
TEST_CASE("Quantizer's control table covers every parameter id",
          "[brew][quantizer][stereo]") {
    state::StateStore store;
    auto proc = create_quantizer();
    proc->define_parameters(store);

    constexpr int kLastId = QuantizerProcessor::kSmoothMs;
    REQUIRE(static_cast<int>(QuantizerProcessor::controls().size()) == kLastId);
    for (int id = 1; id <= kLastId; ++id)
        for (std::size_t ch = 0; ch < kChannelCount; ++ch) {
            CAPTURE(id, ch);
            REQUIRE(store.info(qpid(static_cast<state::ParamID>(id), ch)) != nullptr);
        }
    REQUIRE(store.all_params().size() ==
            QuantizerProcessor::controls().size() * kChannelCount);
}

TEST_CASE("Quantizer's Enable passes the channel through unchanged",
          "[brew][quantizer][enable]") {
    format::HeadlessHost host{create_quantizer};
    host.prepare(48000.0, 512, 2, 2);
    // Four steps over [-1,+1]: width 0.5, so 0.6 snaps to 0.5.
    host.state().set_value(qpid(QuantizerProcessor::kSteps, 0), 4.0f);
    host.state().set_value(qpid(QuantizerProcessor::kSteps, 1), 4.0f);

    SECTION("enabled by default") {
        const auto [l, r] = both_channels(host, 0.6f);
        REQUIRE(l == 0.5f);
        REQUIRE(r == 0.5f);
    }

    SECTION("off is a wire, bit-exactly, and scoped to one channel") {
        host.state().set_value(qpid(QuantizerProcessor::kEnable, 0), 0.0f);
        const auto [l, r] = both_channels(host, 0.6f);
        REQUIRE(l == 0.6f);
        REQUIRE(r == 0.5f);
    }
}

TEST_CASE("Quantizer's channels quantize independently",
          "[brew][quantizer][stereo]") {
    format::HeadlessHost host{create_quantizer};
    host.prepare(48000.0, 512, 2, 2);
    host.state().set_value(qpid(QuantizerProcessor::kSteps, 0), 4.0f);   // width 0.5
    host.state().set_value(qpid(QuantizerProcessor::kSteps, 1), 2.0f);   // width 1.0
    const auto [l, r] = both_channels(host, 0.6f);
    REQUIRE(l == 0.5f);
    REQUIRE(r == 1.0f);
}

TEST_CASE("Quantizer's Smooth glides between lattice points, and is a wire at zero",
          "[brew][quantizer][smooth]") {
    format::HeadlessHost host{create_quantizer};
    host.prepare(48000.0, 512, 2, 2);
    host.state().set_value(qpid(QuantizerProcessor::kSteps, 0), 4.0f);

    SECTION("zero is a wire: the staircase stays exact") {
        const auto out = shape(host, {0.6f, 0.6f, 0.6f});
        for (float v : out) REQUIRE(v == 0.5f);
    }

    // Smoothing runs after the snap, so the output glides between lattice points
    // rather than the glide being quantized. A slew of 1000 ms cannot cross half a
    // full swing in three samples, so the first samples must be short of the step.
    SECTION("a slew limit holds the output short of the step it snapped to") {
        host.state().set_value(qpid(QuantizerProcessor::kSmoothMs, 0), 1000.0f);
        const auto out = shape(host, {0.6f, 0.6f, 0.6f});
        REQUIRE(out.front() > 0.0f);
        REQUIRE(out.front() < 0.5f);
        REQUIRE(out.back() < 0.5f);
        REQUIRE(out.back() > out.front());
    }
}
