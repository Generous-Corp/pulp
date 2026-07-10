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
                    QuantizeSettings s{steps, offset, transpose, 1.0f, false};
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
