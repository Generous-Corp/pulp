// Function — the transfer curves, and the properties a CV shaper must not break.
//
// Note that the curves do NOT share a property set. `power` is odd-symmetric,
// monotone, and pinned at the rails. `exponential` and `logarithmic` are the
// conventional definitions and are none of those things — the logarithm is not
// even defined on half its input range. Asserting a shared invariant across all
// five would either be false or would quietly force the conventional curves into
// a shape they do not have. So each curve is held to what it actually promises.
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

/// A ramp across the whole bipolar range, hitting the exact rails and zero.
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

// ---------------------------------------------------------------------------
// The conventional curves. These are the defaults because a patch written
// against any other CV utility expects them, not because they are well behaved.
// ---------------------------------------------------------------------------

TEST_CASE("exponential is 2^x - 1", "[brew][function][curve]") {
    for (double x = -1.0; x <= 1.0; x += 0.125) {
        CAPTURE(x);
        REQUIRE_THAT(apply_curve(Curve::exponential, 1.0, x),
                     WithinAbs(std::exp2(x) - 1.0, 1e-12));
    }
    // Passes through the origin and reaches +1 at full scale...
    REQUIRE_THAT(apply_curve(Curve::exponential, 1.0, 0.0), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(apply_curve(Curve::exponential, 1.0, 1.0), WithinAbs(1.0, 1e-12));
    // ...but bottoms out at -0.5, not -1. It is not odd-symmetric, so it shifts
    // the centre of a symmetric signal. That is the function, not a bug.
    REQUIRE_THAT(apply_curve(Curve::exponential, 1.0, -1.0), WithinAbs(-0.5, 1e-12));
}

TEST_CASE("logarithmic is 1 + log2(x), and zero below the origin",
          "[brew][function][curve]") {
    REQUIRE_THAT(apply_curve(Curve::logarithmic, 1.0, 1.0), WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(apply_curve(Curve::logarithmic, 1.0, 0.5), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(apply_curve(Curve::logarithmic, 1.0, 0.25), WithinAbs(-1.0, 1e-12));

    // Half the bipolar range is flat. Documented, and the reason `power` exists.
    for (double x = -1.0; x < 0.0; x += 0.1) {
        CAPTURE(x);
        REQUIRE(apply_curve(Curve::logarithmic, 1.0, x) == 0.0);
    }

    // log2(0) is negative infinity. A curve that returns -inf at the origin is
    // not a curve anyone can patch, so zero is folded in with the negatives.
    REQUIRE(apply_curve(Curve::logarithmic, 1.0, 0.0) == 0.0);

    // And it dives past the rail just above the origin, where the clamp catches it.
    REQUIRE(std::isfinite(apply_curve(Curve::logarithmic, 1.0, 1e-30)));
    REQUIRE(apply_curve(Curve::logarithmic, 1.0, 1e-30) == -1.0);
}

TEST_CASE("absolute rectifies", "[brew][function][curve]") {
    for (double x = -1.0; x <= 1.0; x += 0.1) {
        CAPTURE(x);
        REQUIRE_THAT(apply_curve(Curve::absolute, 1.0, x), WithinAbs(std::abs(x), 1e-12));
    }
}

// The Amount knob belongs to `power` alone. A knob that silently does nothing is
// a knob a user will turn while wondering why the sound is not changing.
TEST_CASE("only the power curve reads Amount", "[brew][function][curve]") {
    REQUIRE(curve_uses_amount(Curve::power));
    REQUIRE_FALSE(curve_uses_amount(Curve::linear));
    REQUIRE_FALSE(curve_uses_amount(Curve::exponential));
    REQUIRE_FALSE(curve_uses_amount(Curve::logarithmic));
    REQUIRE_FALSE(curve_uses_amount(Curve::absolute));

    for (double a : {0.125, 1.0, 8.0}) {
        CAPTURE(a);
        REQUIRE_THAT(apply_curve(Curve::exponential, a, 0.3),
                     WithinAbs(std::exp2(0.3) - 1.0, 1e-12));
        REQUIRE_THAT(apply_curve(Curve::logarithmic, a, 0.7),
                     WithinAbs(1.0 + std::log2(0.7), 1e-12));
    }
}

// ---------------------------------------------------------------------------
// `power` — ours. Every property below is one the conventional curves lack, and
// each is why it exists.
// ---------------------------------------------------------------------------

TEST_CASE("power fixes the origin and both rails, for every exponent",
          "[brew][function][power]") {
    for (double k : {0.125, 0.5, 1.0, 3.0, 8.0}) {
        CAPTURE(k);
        REQUIRE(apply_curve(Curve::power, k, 0.0) == 0.0);
        REQUIRE_THAT(apply_curve(Curve::power, k, 1.0), WithinAbs(1.0, 1e-12));
        REQUIRE_THAT(apply_curve(Curve::power, k, -1.0), WithinAbs(-1.0, 1e-12));
    }
}

TEST_CASE("power is odd-symmetric, so a bipolar CV keeps its polarity",
          "[brew][function][power]") {
    for (double k : {0.25, 1.0, 4.0}) {
        for (double x = 0.05; x < 1.0; x += 0.05) {
            CAPTURE(k, x);
            REQUIRE_THAT(apply_curve(Curve::power, k, -x),
                         WithinAbs(-apply_curve(Curve::power, k, x), 1e-12));
        }
    }
}

TEST_CASE("power never folds", "[brew][function][power]") {
    for (double k : {0.125, 1.0, 8.0}) {
        double prev = apply_curve(Curve::power, k, -1.0);
        for (double x = -0.99; x <= 1.0; x += 0.01) {
            const double y = apply_curve(Curve::power, k, x);
            CAPTURE(k, x, prev, y);
            REQUIRE(y >= prev - 1e-12);
            prev = y;
        }
    }
}

// k and 1/k are exact inverses, which is what lets one knob span both directions.
// Neither conventional curve has an inverse in this set.
TEST_CASE("power's exponent and its reciprocal undo each other",
          "[brew][function][power]") {
    for (double x = -0.95; x < 1.0; x += 0.1) {
        for (double k : {1.5, 2.0, 5.5}) {
            CAPTURE(x, k);
            const double bent = apply_curve(Curve::power, k, x);
            REQUIRE_THAT(apply_curve(Curve::power, 1.0 / k, bent), WithinAbs(x, 1e-9));
        }
    }
    // And they bend in opposite directions about the identity.
    REQUIRE(apply_curve(Curve::power, 2.0, 0.5) < 0.5);
    REQUIRE(apply_curve(Curve::power, 0.5, 0.5) > 0.5);
    // k = 1 is the identity, which is why the knob's default sits there.
    for (double x = -1.0; x <= 1.0; x += 0.1)
        REQUIRE_THAT(apply_curve(Curve::power, 1.0, x), WithinAbs(x, 1e-12));
}

// An out-of-range exponent from a host must not blow up the curve.
TEST_CASE("power clamps its exponent", "[brew][function][power][safety]") {
    REQUIRE_THAT(apply_curve(Curve::power, 1e6, 0.5),
                 WithinAbs(apply_curve(Curve::power, kMaxPower, 0.5), 1e-12));
    REQUIRE_THAT(apply_curve(Curve::power, -3.0, 0.5),
                 WithinAbs(apply_curve(Curve::power, kMinPower, 0.5), 1e-12));
}

// ---------------------------------------------------------------------------
// The chain around the curve.
// ---------------------------------------------------------------------------

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
    REQUIRE(curve_from_param(4.0f) == Curve::power);
    REQUIRE(curve_from_param(99.0f) == Curve::power);
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
    s.curve = Curve::power;
    s.amount = 2.0f;
    s.out_offset = -0.5f;
    REQUIRE_THAT(function_transfer(0.6f, s), WithinAbs(0.5f, 1e-6f));

    // And the shape of the failure it guards: an unclamped curve would exceed
    // what the same input at the rail produces, which is impossible for a
    // monotone map onto [-1, 1].
    REQUIRE(function_transfer(0.6f, s) == function_transfer(1.0f, s));
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

    // Flip the input, then rectify: still positive, because |-x| = |x|.
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

// --------------------------------------------------- Enable, and two channels
//
// `shape()` above returns channel 0 only, which is why every test before this
// point is blind to the right channel. These are the ones that look at both.

namespace {

state::ParamID fpid(state::ParamID id, std::size_t ch) {
    return static_cast<state::ParamID>(param_for(id, ch));
}

/// Render a constant on both inputs; return the last sample of each output.
std::pair<float, float> both(format::HeadlessHost& host, float level) {
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

TEST_CASE("Function's Enable passes the channel through unmodified",
          "[brew][function][enable]") {
    format::HeadlessHost host{create_function};
    host.prepare(48000.0, 512, 2, 2);
    // Absolute would rectify a negative input to +0.75.
    host.state().set_value(fpid(FunctionProcessor::kCurve, 0),
                           static_cast<float>(static_cast<int>(Curve::absolute)));
    host.state().set_value(fpid(FunctionProcessor::kCurve, 1),
                           static_cast<float>(static_cast<int>(Curve::absolute)));

    SECTION("enabled by default, so a fresh instance shapes") {
        const auto [l, r] = both(host, -0.75f);
        REQUIRE(l == 0.75f);
        REQUIRE(r == 0.75f);
    }

    // Off is a wire, not a mute. Zeroing here would drop the voltage the upstream
    // plug-in is generating, which is not what disabling a shaping stage means.
    SECTION("disabling one channel wires it, and leaves the other shaping") {
        host.state().set_value(fpid(FunctionProcessor::kEnable, 0), 0.0f);
        const auto [l, r] = both(host, -0.75f);
        REQUIRE(l == -0.75f);
        REQUIRE(r == 0.75f);
    }

    SECTION("a disabled channel is bit-exact, not merely close") {
        host.state().set_value(fpid(FunctionProcessor::kEnable, 1), 0.0f);
        // A value that no curve leaves untouched, and that no rounding survives.
        const auto [l, r] = both(host, -0.3333333f);
        REQUIRE(r == -0.3333333f);
        REQUIRE(l == 0.3333333f);
    }
}

TEST_CASE("Function's channels shape independently", "[brew][function][stereo]") {
    format::HeadlessHost host{create_function};
    host.prepare(48000.0, 512, 2, 2);

    SECTION("each channel follows its own curve") {
        host.state().set_value(fpid(FunctionProcessor::kCurve, 0),
                               static_cast<float>(static_cast<int>(Curve::absolute)));
        host.state().set_value(fpid(FunctionProcessor::kCurve, 1),
                               static_cast<float>(static_cast<int>(Curve::linear)));
        const auto [l, r] = both(host, -0.5f);
        REQUIRE(l == 0.5f);
        REQUIRE(r == -0.5f);
    }

    SECTION("each channel follows its own scale and offset") {
        host.state().set_value(fpid(FunctionProcessor::kOutOffset, 1), 0.25f);
        const auto [l, r] = both(host, 0.5f);
        REQUIRE(l == 0.5f);
        REQUIRE(std::abs(r - 0.75f) < 1e-6f);
    }

    SECTION("polarity is per-jack") {
        host.state().set_value(fpid(FunctionProcessor::kInvert, 1), 1.0f);
        const auto [l, r] = both(host, 0.5f);
        REQUIRE(l == 0.5f);
        REQUIRE(r == -0.5f);
    }
}

TEST_CASE("Function registers every control on both channels",
          "[brew][function][stereo]") {
    state::StateStore store;
    auto proc = create_function();
    proc->define_parameters(store);
    for (const auto& c : FunctionProcessor::controls()) {
        REQUIRE(store.info(fpid(c.id, 0)) != nullptr);
        REQUIRE(store.info(fpid(c.id, 1)) != nullptr);
    }
}
