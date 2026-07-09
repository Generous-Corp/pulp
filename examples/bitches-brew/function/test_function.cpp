// Function — the transfer curve, and the properties a CV shaper must not break.
//
// Scope note. These tests exercise the pure transfer function and
// `Processor::process()`. They do NOT prove a shaped value survives the host bus
// (see test_dc.cpp for why), and nothing here proves a sample becomes a real
// voltage. That needs hardware.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "function_processor.hpp"
#include <pulp/format/headless.hpp>

#include <cmath>
#include <vector>

using namespace pulp;
using namespace pulp::examples::brew;
using Catch::Matchers::WithinAbs;

namespace {

/// Render one block from a caller-supplied input, and return channel 0.
std::vector<float> shape(format::HeadlessHost& host,
                         const std::vector<float>& in_samples,
                         const format::ProcessContext* ctx = nullptr) {
    const auto n = in_samples.size();
    audio::Buffer<float> in(2, n);
    audio::Buffer<float> out(2, n);
    in.clear();
    out.clear();
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < n; ++i) in.channel(c)[i] = in_samples[i];

    const float* ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ptrs, 2, n);
    auto ov = out.view();
    if (ctx != nullptr)
        host.process(ov, iv, *ctx);
    else
        host.process(ov, iv);

    return std::vector<float>(out.channel(0).begin(), out.channel(0).end());
}

/// A ramp across the whole bipolar range, plus the exact rails and zero.
std::vector<float> sweep(int n = 65) {
    std::vector<float> v;
    v.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v.push_back(-1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(n - 1));
    return v;
}

constexpr FunctionSettings identity{};

}  // namespace

TEST_CASE("Function descriptor is a two-channel CV insert", "[brew][function]") {
    auto proc = create_function();
    const auto desc = proc->descriptor();
    REQUIRE(desc.name == "Function");
    REQUIRE(desc.manufacturer == "Bitches Brew");
    REQUIRE_FALSE(desc.accepts_midi);
    // Unlike the generators, Function needs its input bus.
    REQUIRE(desc.input_buses.size() == 1);
    REQUIRE(desc.input_buses[0].default_channels == 2);
}

// The property a freshly inserted shaping stage must hold: it is a wire. Not
// "close to a wire" — bit-exact, so Function can never be the thing that changed
// a control voltage until the user asks it to.
TEST_CASE("Function's defaults are a bit-exact wire", "[brew][function][identity]") {
    for (float x : sweep(129)) {
        CAPTURE(x);
        REQUIRE(function_transfer(x, identity) == x);
    }

    format::HeadlessHost host(create_function);
    host.prepare(48000.0, 512, 2, 2);
    const auto in = sweep(64);
    REQUIRE(shape(host, in) == in);
}

// Every curve passes through the origin and both rails, for every amount. This
// is what makes the Amount knob safe to sweep live: it bends the middle of the
// response without ever moving where full scale lands.
TEST_CASE("Curves fix the origin and the rails", "[brew][function][curve]") {
    for (int c = 0; c < kCurveCount; ++c) {
        const auto curve = static_cast<Curve>(c);
        for (float amount : {1.0f, 1.7f, 3.0f, 8.0f}) {
            CAPTURE(c, amount);
            REQUIRE(apply_curve(curve, amount, 0.0) == 0.0);
            REQUIRE_THAT(apply_curve(curve, amount, 1.0), WithinAbs(1.0, 1e-12));
            // Absolute rectifies, so -1 maps to +1 rather than to -1.
            const double at_min = apply_curve(curve, amount, -1.0);
            REQUIRE_THAT(at_min, WithinAbs(curve == Curve::absolute ? 1.0 : -1.0, 1e-12));
        }
    }
}

// Odd symmetry: a bipolar CV keeps its polarity through every curve but the
// rectifier. A curve that broke this would silently invert half of an LFO.
TEST_CASE("Curves are odd-symmetric except absolute", "[brew][function][curve]") {
    for (int c = 0; c < kCurveCount; ++c) {
        const auto curve = static_cast<Curve>(c);
        for (double x = 0.05; x < 1.0; x += 0.05) {
            CAPTURE(c, x);
            const double pos = apply_curve(curve, 3.0, x);
            const double neg = apply_curve(curve, 3.0, -x);
            if (curve == Curve::absolute)
                REQUIRE_THAT(neg, WithinAbs(pos, 1e-12));
            else
                REQUIRE_THAT(neg, WithinAbs(-pos, 1e-12));
        }
    }
}

// Monotone: a curve must never fold. A folding transfer function maps two input
// voltages to one output, which makes the plug-in unusable as a shaper.
TEST_CASE("Curves are monotone across the range", "[brew][function][curve]") {
    for (int c = 0; c < kCurveCount; ++c) {
        const auto curve = static_cast<Curve>(c);
        // Absolute folds at zero by definition; test each half separately.
        const double from = curve == Curve::absolute ? 0.0 : -1.0;
        double prev = apply_curve(curve, 4.0, from);
        for (double x = from + 0.01; x <= 1.0; x += 0.01) {
            const double y = apply_curve(curve, 4.0, x);
            CAPTURE(c, x, prev, y);
            REQUIRE(y >= prev - 1e-12);
            prev = y;
        }
    }
}

// Exponential bends toward the origin, logarithmic away from it, and the two are
// reciprocals — so the same Amount undoes the other. That relationship is the
// reason a single knob can drive both.
TEST_CASE("Exponential and logarithmic are inverses", "[brew][function][curve]") {
    for (double x = -0.95; x < 1.0; x += 0.1) {
        for (double amount : {1.5, 2.0, 5.5}) {
            CAPTURE(x, amount);
            const double bent = apply_curve(Curve::exponential, amount, x);
            REQUIRE_THAT(apply_curve(Curve::logarithmic, amount, bent),
                         WithinAbs(x, 1e-9));
        }
    }
    // And they bend in opposite directions.
    REQUIRE(apply_curve(Curve::exponential, 2.0, 0.5) < 0.5);
    REQUIRE(apply_curve(Curve::logarithmic, 2.0, 0.5) > 0.5);
}

// Amount = 1 is no bend at all, which is why the parameter's range starts there.
TEST_CASE("Amount of 1 leaves every curve unbent", "[brew][function][curve]") {
    for (double x = -1.0; x <= 1.0; x += 0.1) {
        CAPTURE(x);
        REQUIRE_THAT(apply_curve(Curve::exponential, 1.0, x), WithinAbs(x, 1e-12));
        REQUIRE_THAT(apply_curve(Curve::logarithmic, 1.0, x), WithinAbs(x, 1e-12));
    }
}

// An out-of-range curve index from a host must clamp, never wrap onto a
// different shape.
TEST_CASE("Curve parameter clamps rather than wraps", "[brew][function][curve]") {
    REQUIRE(curve_from_param(-3.0f) == Curve::linear);
    REQUIRE(curve_from_param(0.4f) == Curve::linear);
    REQUIRE(curve_from_param(1.4f) == Curve::exponential);
    // std::lround rounds halves away from zero, so 1.5 lands on the *higher*
    // shape. Pinned because a host stepping a discrete parameter through its
    // normalized range can land exactly on a tie.
    REQUIRE(curve_from_param(1.5f) == Curve::logarithmic);
    REQUIRE(curve_from_param(3.0f) == Curve::absolute);
    REQUIRE(curve_from_param(99.0f) == Curve::absolute);
}

// The clamp between the input stage and the curve. `in_scale` can push a signal
// past full scale, and |x|^k on x > 1 runs away from the rail rather than toward
// it. Without the clamp, a negative out_offset carries that excess to the jack
// instead of having it absorbed by the final clamp.
//
// Concretely: in_scale 2 on x = 0.6 is 1.2. Squared that is 1.44; with an offset
// of -0.5 the output is 0.94. Clamped first it is 1.0, squared 1.0, offset 0.5.
TEST_CASE("Function clamps before the curve, not only after",
          "[brew][function][clamp]") {
    FunctionSettings s;
    s.in_scale = 2.0f;
    s.curve = Curve::exponential;
    s.amount = 2.0f;
    s.out_offset = -0.5f;
    REQUIRE_THAT(function_transfer(0.6f, s), WithinAbs(0.5f, 1e-6f));

    // And the shape of the failure it guards: an unclamped curve would exceed
    // what the same input at the rail produces, which is impossible for a
    // monotone map onto [-1, 1].
    FunctionSettings rail = s;
    REQUIRE(function_transfer(0.6f, s) == function_transfer(1.0f, rail));
}

// Output can never leave the rails, whatever the knobs say. A CV plug-in that
// overshoots hands the interface a number it will clip anyway — but silently,
// and after the plug-in has already decided the shape.
TEST_CASE("Function never leaves [-1, +1]", "[brew][function][safety]") {
    for (int c = 0; c < kCurveCount; ++c) {
        for (float in_scale : {-4.0f, -1.0f, 0.0f, 1.0f, 4.0f}) {
            for (float in_off : {-1.0f, 0.0f, 1.0f}) {
                for (float out_off : {-1.0f, 0.0f, 1.0f}) {
                    FunctionSettings s;
                    s.curve = static_cast<Curve>(c);
                    s.amount = 6.0f;
                    s.in_scale = in_scale;
                    s.in_offset = in_off;
                    s.out_offset = out_off;
                    for (float x : sweep(17)) {
                        CAPTURE(c, in_scale, in_off, out_off, x);
                        const float y = function_transfer(x, s);
                        REQUIRE(y >= -1.0f);
                        REQUIRE(y <= 1.0f);
                        REQUIRE(std::isfinite(y));
                    }
                }
            }
        }
    }
}

// Rig compensation: a negative in_scale flips the incoming polarity, Invert flips
// the outgoing one. They are different knobs at different ends of the curve, and
// on a rectifier they are emphatically not the same thing.
TEST_CASE("In Scale and Invert act at opposite ends", "[brew][function]") {
    FunctionSettings s;
    s.curve = Curve::absolute;

    // Rectify, then invert: always negative.
    s.invert = true;
    REQUIRE_THAT(function_transfer(0.5f, s), WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(function_transfer(-0.5f, s), WithinAbs(-0.5f, 1e-6f));

    // Flip the input, then rectify: still positive, because |−x| = |x|.
    s.invert = false;
    s.in_scale = -1.0f;
    REQUIRE_THAT(function_transfer(0.5f, s), WithinAbs(0.5f, 1e-6f));
}

// Bypass on an insert is a wire, not a mute. Muting would drop the voltage the
// upstream plug-in is generating, which is not what Bypass asks for — and is the
// opposite of what the generators in this suite do when bypassed.
TEST_CASE("Function passes its input through while bypassed",
          "[brew][function][bypass]") {
    format::HeadlessHost host(create_function);
    host.prepare(48000.0, 512, 2, 2);
    host.state().set_value(FunctionProcessor::kCurve,
                           static_cast<float>(Curve::absolute));
    host.state().set_value(FunctionProcessor::kOutputScale, 0.25f);

    const auto in = sweep(32);

    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 32;
    ctx.is_bypassed = true;
    REQUIRE(shape(host, in, &ctx) == in);

    // Un-bypassed, the same input is rectified and scaled.
    ctx.is_bypassed = false;
    const auto out = shape(host, in, &ctx);
    for (std::size_t i = 0; i < in.size(); ++i) {
        CAPTURE(i, in[i]);
        REQUIRE_THAT(out[i], WithinAbs(std::abs(in[i]) * 0.25f, 1e-6f));
    }
}

// The editor's dot reads real DSP state. A dot parked at the origin while a CV is
// flowing would be a lie about a signal the user cannot otherwise see.
TEST_CASE("Function publishes its operating point for the editor",
          "[brew][function]") {
    format::HeadlessHost host(create_function);
    host.prepare(48000.0, 512, 2, 2);
    host.state().set_value(FunctionProcessor::kCurve,
                           static_cast<float>(Curve::absolute));

    const auto* proc = static_cast<const FunctionProcessor*>(host.processor());
    REQUIRE(proc->display_input() == 0.0f);

    shape(host, {0.1f, 0.2f, -0.75f});
    REQUIRE_THAT(proc->display_input(), WithinAbs(-0.75f, 1e-6f));
    REQUIRE_THAT(proc->display_output(), WithinAbs(0.75f, 1e-6f));  // rectified
}
