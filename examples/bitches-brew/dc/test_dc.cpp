// DC — the suite's held-value guard.
//
// A control voltage is a number that must arrive unchanged. Every assertion
// here uses `==` on floats deliberately: DC is the one place in the codebase
// where "close enough" is the bug. A tolerance would hide exactly the
// smoothing, ramping, or dithering this test exists to forbid.
//
// Scope note. These tests exercise `Processor::process()`. They do NOT prove a
// held value survives the *host bus* — an adapter can hand the host a correct
// buffer flagged as silent, and the host will then substitute silence. That
// defect is invisible from here and is covered at the adapter boundary by
// test_au_v2_effect.cpp and test_vst3_plugin_state.cpp (`[silence]`). Neither
// layer proves a sample value becomes a real voltage; that needs hardware.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dc_processor.hpp"
#include <pulp/format/headless.hpp>

#include <cmath>
#include <vector>

using namespace pulp;
using namespace pulp::examples::brew;

namespace {

/// Render one block and return every output sample, so a test can assert on the
/// whole buffer rather than spot-checking sample 0.
std::vector<float> render_block(format::HeadlessHost& host,
                                int num_samples,
                                int channels = 2,
                                const format::ProcessContext* ctx = nullptr) {
    audio::Buffer<float> in(static_cast<std::size_t>(channels),
                            static_cast<std::size_t>(num_samples));
    audio::Buffer<float> out(static_cast<std::size_t>(channels),
                             static_cast<std::size_t>(num_samples));
    in.clear();
    out.clear();

    std::vector<const float*> in_ptrs(static_cast<std::size_t>(channels));
    for (int c = 0; c < channels; ++c)
        in_ptrs[static_cast<std::size_t>(c)] = in.channel(c).data();
    audio::BufferView<const float> iv(in_ptrs.data(),
                                      static_cast<std::size_t>(channels),
                                      static_cast<std::size_t>(num_samples));
    auto ov = out.view();
    if (ctx != nullptr)
        host.process(ov, iv, *ctx);
    else
        host.process(ov, iv);

    std::vector<float> flat;
    flat.reserve(static_cast<std::size_t>(channels * num_samples));
    for (int c = 0; c < channels; ++c)
        for (int n = 0; n < num_samples; ++n)
            flat.push_back(out.channel(c)[static_cast<std::size_t>(n)]);
    return flat;
}

/// Render one block with a constant level on every input channel.
std::vector<float> render_with_input(format::HeadlessHost& host, int num_samples,
                                     float level, int channels = 2,
                                     const format::ProcessContext* ctx = nullptr) {
    audio::Buffer<float> in(static_cast<std::size_t>(channels),
                            static_cast<std::size_t>(num_samples));
    audio::Buffer<float> out(static_cast<std::size_t>(channels),
                             static_cast<std::size_t>(num_samples));
    in.clear();
    out.clear();
    for (int c = 0; c < channels; ++c)
        for (int n = 0; n < num_samples; ++n)
            in.channel(c)[static_cast<std::size_t>(n)] = level;

    std::vector<const float*> in_ptrs(static_cast<std::size_t>(channels));
    for (int c = 0; c < channels; ++c)
        in_ptrs[static_cast<std::size_t>(c)] = in.channel(c).data();
    audio::BufferView<const float> iv(in_ptrs.data(),
                                      static_cast<std::size_t>(channels),
                                      static_cast<std::size_t>(num_samples));
    auto ov = out.view();
    if (ctx != nullptr) host.process(ov, iv, *ctx); else host.process(ov, iv);

    std::vector<float> flat;
    for (int c = 0; c < channels; ++c)
        for (int n = 0; n < num_samples; ++n)
            flat.push_back(out.channel(c)[static_cast<std::size_t>(n)]);
    return flat;
}

/// Every sample within a tolerance of `expected`. Use for values that arrive via
/// arithmetic; `all_equal` stays exact and guards bit-exactness, which is a
/// different claim and the one the suite actually rests on.
bool all_near(const std::vector<float>& v, float expected, float eps = 1e-6f) {
    for (float x : v)
        if (std::abs(x - expected) > eps) return false;
    return !v.empty();
}

bool all_equal(const std::vector<float>& v, float expected) {
    for (float s : v)
        if (s != expected) return false;
    return !v.empty();
}

}  // namespace

TEST_CASE("DC descriptor is a two-channel CV source", "[brew][dc]") {
    auto proc = create_dc();
    const auto desc = proc->descriptor();
    REQUIRE(desc.name == "DC");
    REQUIRE(desc.manufacturer == "Bitches Brew");
    REQUIRE(desc.category == format::PluginCategory::Effect);
    // No MIDI: DC is driven entirely by its own parameters.
    REQUIRE_FALSE(desc.accepts_midi);
}

// A fresh instance must emit no voltage. This is a hardware-safety property,
// not a style choice: a CV generator that comes up at full scale points a
// voltage at whatever module happens to be patched to its output.
TEST_CASE("DC emits silence until asked for a value", "[brew][dc][safety]") {
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 512, 2, 2);
    REQUIRE(all_equal(render_block(host, 512), 0.0f));
}

// The guard the whole suite leans on: what you set is exactly what comes out,
// on every sample, at every block size, at every sample rate. Bit-exact.
TEST_CASE("DC holds its value bit-exactly across block sizes and sample rates",
          "[brew][dc][dc-fidelity]") {
    // 1 is not academic — hosts really do render single-sample blocks.
    const int block_sizes[] = {1, 32, 64, 512, 4096};
    const double sample_rates[] = {44100.0, 48000.0, 96000.0, 192000.0};
    const float values[] = {0.5f, -0.5f, 1.0f, -1.0f, 0.0f, 0.001f, 0.333333f};

    for (double sr : sample_rates) {
        for (int block : block_sizes) {
            for (float v : values) {
                format::HeadlessHost host(create_dc);
                host.prepare(sr, 4096, 2, 2);
                host.state().set_value(DcProcessor::kValue, v);

                CAPTURE(sr, block, v);
                REQUIRE(all_equal(render_block(host, block), v));

                // Successive blocks must not drift: no accumulator, no ramp.
                REQUIRE(all_equal(render_block(host, block), v));
            }
        }
    }
}

// The output stage itself (clamp / scale / invert) is brew-core's, and is tested
// there. What DC owes is proof that its parameters actually reach it.
TEST_CASE("DC scale and invert reach the rendered buffer", "[brew][dc]") {
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 512, 2, 2);

    host.state().set_value(DcProcessor::kValue, 1.0f);
    host.state().set_value(DcProcessor::kOutputScale, 0.25f);
    REQUIRE(all_equal(render_block(host, 512), 0.25f));

    host.state().set_value(DcProcessor::kInvert, 1.0f);
    REQUIRE(all_equal(render_block(host, 512), -0.25f));
}

// Bypass means stop driving the patch. Some hosts bypass by short-circuiting
// process(); others keep calling it, and on those a plug-in that ignores the flag
// leaves its voltage at the jack after the user pressed Bypass.
TEST_CASE("DC emits nothing while bypassed", "[brew][dc][safety][bypass]") {
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 512, 2, 2);
    host.state().set_value(DcProcessor::kValue, 0.75f);

    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 512;
    ctx.is_bypassed = true;
    REQUIRE(all_equal(render_block(host, 512, 2, &ctx), 0.0f));

    // And the value comes straight back when the host un-bypasses.
    ctx.is_bypassed = false;
    REQUIRE(all_equal(render_block(host, 512, 2, &ctx), 0.75f));
}

// Whatever is on the input bus is irrelevant: DC generates, it does not process.
// This is also the shape that trips the adapter silence-flag bug — output
// synthesized from a silent input.
TEST_CASE("DC ignores its input bus", "[brew][dc]") {
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 256, 2, 2);
    host.state().set_value(DcProcessor::kValue, 0.75f);
    REQUIRE(all_equal(render_block(host, 256), 0.75f));
}

// ------------------------------------------------------------ the two Out knobs

TEST_CASE("the two output knobs sum", "[brew][dc]") {
    // Two knobs rather than one because automation of the bipolar sweep can then
    // ride on a fixed unipolar offset. Summing is the whole point; if `Unipolar`
    // merely replaced `Value` there would be no reason for it to exist.
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 64, 2, 2);
    host.state().set_value(DcProcessor::kValue, -0.25f);
    host.state().set_value(DcProcessor::kUnipolar, 0.75f);
    REQUIRE(all_equal(render_block(host, 32), 0.5f));

    // And the sum is clamped once, at the jack, not inside the knobs.
    host.state().set_value(DcProcessor::kValue, 0.9f);
    host.state().set_value(DcProcessor::kUnipolar, 0.9f);
    REQUIRE(all_equal(render_block(host, 32), 1.0f));
}

TEST_CASE("Multiplier scales the sum and may invert it", "[brew][dc]") {
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 64, 2, 2);
    host.state().set_value(DcProcessor::kValue, 0.4f);
    host.state().set_value(DcProcessor::kMultiplier, -1.5f);
    REQUIRE(all_equal(render_block(host, 32), -0.6f));

    // It scales the *sum*, not just Value: otherwise the unipolar offset would
    // survive a multiplier of zero.
    host.state().set_value(DcProcessor::kUnipolar, 0.5f);
    host.state().set_value(DcProcessor::kMultiplier, 0.0f);
    REQUIRE(all_equal(render_block(host, 32), 0.0f));
}

TEST_CASE("Multiplier and Output Scale are not the same control", "[brew][dc]") {
    // Output Scale is unipolar rig calibration and belongs to the interface.
    // Multiplier is bipolar and belongs to the patch. Both apply.
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 64, 2, 2);
    host.state().set_value(DcProcessor::kValue, 0.8f);
    host.state().set_value(DcProcessor::kMultiplier, -1.0f);
    host.state().set_value(DcProcessor::kOutputScale, 0.5f);
    REQUIRE(all_equal(render_block(host, 32), -0.4f));
}

// ------------------------------------------------------------------- the input

TEST_CASE("Input Mul gates the output with the input", "[brew][dc]") {
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 64, 2, 2);
    host.state().set_value(DcProcessor::kValue, 0.6f);

    // At zero the input is ignored entirely — the documented resting behavior.
    REQUIRE(all_equal(render_with_input(host, 32, 0.5f), 0.6f));

    // At one the output is the product.
    host.state().set_value(DcProcessor::kInputMul, 1.0f);
    REQUIRE(all_equal(render_with_input(host, 32, 0.5f), 0.3f));
    // ...so an input at zero closes the gate completely.
    REQUIRE(all_equal(render_with_input(host, 32, 0.0f), 0.0f));

    // And halfway is halfway between "ignored" and "fully gated". Tolerant:
    // 0.6 * 0.75 lands one ulp off 0.45f, and this is an arithmetic claim, not a
    // bit-exactness one.
    host.state().set_value(DcProcessor::kInputMul, 0.5f);
    REQUIRE(all_near(render_with_input(host, 32, 0.5f), 0.45f));
}

TEST_CASE("Input Add sums the input into the output", "[brew][dc]") {
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 64, 2, 2);
    host.state().set_value(DcProcessor::kValue, 0.25f);
    host.state().set_value(DcProcessor::kInputAdd, 1.0f);
    REQUIRE(all_equal(render_with_input(host, 32, 0.5f), 0.75f));

    // Bipolar: it can subtract.
    host.state().set_value(DcProcessor::kInputAdd, -1.0f);
    REQUIRE(all_equal(render_with_input(host, 32, 0.5f), -0.25f));
}

TEST_CASE("the added input is not fed through the multiplier", "[brew][dc]") {
    // Multiply, then add. If Input Add went in before Input Mul, the added
    // signal would be gated by itself — a ring modulator eating its own input.
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 64, 2, 2);
    host.state().set_value(DcProcessor::kValue, 0.5f);
    host.state().set_value(DcProcessor::kInputMul, 1.0f);
    host.state().set_value(DcProcessor::kInputAdd, 1.0f);
    // 0.5 * 0.4 + 0.4 = 0.6, not (0.5 + 0.4) * 0.4 = 0.36.
    const auto out = render_with_input(host, 32, 0.4f);
    REQUIRE(out.front() == Catch::Approx(0.6f).margin(1e-6));
}

TEST_CASE("DC ignores an input bus that is not there", "[brew][dc][safety]") {
    // A generator on an input-less track gets no input buffer at all. Reading one
    // would be a crash, and defaulting the input to anything but zero would emit
    // a voltage the user did not ask for.
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 64, 0, 2);
    host.state().set_value(DcProcessor::kValue, 0.5f);
    host.state().set_value(DcProcessor::kInputMul, 1.0f);
    host.state().set_value(DcProcessor::kInputAdd, 1.0f);

    audio::Buffer<float> out(2, 32);
    out.clear();
    audio::BufferView<const float> no_input;
    auto ov = out.view();
    host.process(ov, no_input);
    // Fully gated by an absent input, which reads as zero.
    for (std::size_t n = 0; n < 32; ++n) REQUIRE(out.channel(0)[n] == 0.0f);
}

// -------------------------------------------------------------------- Smooth

TEST_CASE("Smooth at zero is a wire", "[brew][dc][smooth][safety]") {
    // The suite's bit-exactness rule survives the arrival of a smoother only if
    // the smoother is exactly absent at zero. Not "close to". Exactly.
    for (float v : {-1.0f, -0.37f, 0.0f, 0.125f, 0.9f}) {
        Smoother s;
        CAPTURE(v);
        REQUIRE(s.process(v, 0.0f, 48000.0) == v);
        REQUIRE(s.process(v, 0.0f, 48000.0) == v);
    }
    // And a nonsense sample rate cannot make it filter either.
    Smoother s;
    REQUIRE(s.process(0.42f, 50.0f, 0.0) == 0.42f);
}

TEST_CASE("positive Smooth slews at a constant rate", "[brew][dc][smooth]") {
    // Calibrated for a full-scale swing: -1 to +1 in exactly `ms`.
    constexpr double kSr = 48000.0;
    constexpr float kMs = 100.0f;
    Smoother s;
    s.reset(-1.0f);
    const int samples = static_cast<int>(kMs * 0.001 * kSr);
    for (int i = 0; i < samples - 1; ++i) (void)s.process(1.0f, kMs, kSr);
    REQUIRE(s.value() < 1.0f);
    (void)s.process(1.0f, kMs, kSr);
    REQUIRE(s.value() == Catch::Approx(1.0f).margin(1e-4));

    // Constant *rate*: half the distance takes half the time.
    Smoother h;
    h.reset(0.0f);
    for (int i = 0; i < samples / 2; ++i) (void)h.process(1.0f, kMs, kSr);
    REQUIRE(h.value() == Catch::Approx(1.0f).margin(1e-4));
}

TEST_CASE("negative Smooth low-passes and never overshoots",
          "[brew][dc][smooth]") {
    constexpr double kSr = 48000.0;
    Smoother s;
    s.reset(0.0f);
    float prev = 0.0f;
    for (int i = 0; i < 48000; ++i) {
        const float v = s.process(1.0f, -100.0f, kSr);
        REQUIRE(v >= prev);       // monotone toward the target
        REQUIRE(v <= 1.0f);       // and never past it
        prev = v;
    }
    REQUIRE(s.value() == Catch::Approx(1.0f).margin(1e-3));

    // A one-pole approaches asymptotically: it is still short of the target at
    // one time constant, where a slew would have arrived exactly.
    Smoother t;
    t.reset(0.0f);
    const int tau_samples = static_cast<int>(0.1 * kSr);
    for (int i = 0; i < tau_samples; ++i) (void)t.process(1.0f, -100.0f, kSr);
    REQUIRE(t.value() < 0.99f);
    REQUIRE(t.value() > 0.5f);
}

TEST_CASE("Smooth ramps DC's output rather than stepping it",
          "[brew][dc][smooth]") {
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 512, 2, 2);
    host.state().set_value(DcProcessor::kSmoothMs, 100.0f);
    host.state().set_value(DcProcessor::kValue, 1.0f);

    const auto out = render_block(host, 512);
    REQUIRE(out.front() > 0.0f);
    REQUIRE(out.front() < 0.05f);   // 512 samples is ~1% of the way
    REQUIRE(out[511] > out.front());
    REQUIRE(out[511] < 1.0f);

    // Each channel smooths its own signal. Sharing one smoother would make
    // channel 1 continue from where channel 0 left off, and it would be ahead.
    REQUIRE(out[0] == out[512]);
    REQUIRE(out[511] == out[1023]);
}

TEST_CASE("bypass zeroes the output and parks the smoother",
          "[brew][dc][smooth][bypass]") {
    // Releasing bypass must ramp up from silence, not jump to the value the
    // patch last saw. A CV generator that snaps back to full scale on un-bypass
    // is the failure this guards.
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 512, 2, 2);
    host.state().set_value(DcProcessor::kSmoothMs, 100.0f);
    host.state().set_value(DcProcessor::kValue, 1.0f);
    (void)render_block(host, 512);   // climb partway

    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 64;
    ctx.is_bypassed = true;
    REQUIRE(all_equal(render_block(host, 64, 2, &ctx), 0.0f));

    ctx.is_bypassed = false;
    const auto after = render_block(host, 64, 2, &ctx);
    REQUIRE(after.front() < 0.01f);   // starts again from zero
    REQUIRE(after.front() > 0.0f);
}

TEST_CASE("DC publishes its emitted value for the rail", "[brew][dc]") {
    format::HeadlessHost host(create_dc);
    host.prepare(48000.0, 64, 2, 2);
    host.state().set_value(DcProcessor::kValue, -0.4f);
    (void)render_block(host, 32);
    const auto* dc = static_cast<const DcProcessor*>(host.processor());
    REQUIRE(dc->display_output() == Catch::Approx(-0.4f).margin(1e-6));
}
