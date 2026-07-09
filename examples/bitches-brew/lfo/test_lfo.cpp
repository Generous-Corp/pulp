// LFO — golden shapes, phase lock, and quadrature.
//
// The properties worth guarding are the ones a user would only notice after
// bouncing twice: that the phase is a pure function of the host's position, so
// the same beat always yields the same voltage regardless of block size; and that
// the quadrature output really is a quarter cycle ahead, because that is the only
// thing making the pair usable as an (X, Y) modulation.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "lfo_processor.hpp"

#include <pulp/format/headless.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp;
using namespace pulp::examples::brew;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kTempo = 120.0;

/// Render one block starting at `position_beats` and return channel `ch`.
std::vector<float> render(format::HeadlessHost& host, double position_beats,
                          int frames, int ch, bool playing = true) {
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
    ctx.tempo_bpm = kTempo;
    ctx.position_beats = position_beats;
    host.process(ov, iv, ctx);

    std::vector<float> v(static_cast<std::size_t>(frames));
    for (int n = 0; n < frames; ++n)
        v[static_cast<std::size_t>(n)] =
            out.channel(static_cast<std::size_t>(ch))[static_cast<std::size_t>(n)];
    return v;
}


/// Render a block at an absolute sample position, which is what a free-running
/// LFO reads. `tempo` varies so a test can prove the free mode ignores it.
std::vector<float> render_free(format::HeadlessHost& host, std::int64_t sample_pos,
                               int frames, int ch, double tempo = kTempo,
                               bool playing = true) {
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
    ctx.tempo_bpm = tempo;
    ctx.position_samples = sample_pos;
    ctx.position_beats = static_cast<double>(sample_pos) / kSampleRate * (tempo / 60.0);
    host.process(ov, iv, ctx);

    std::vector<float> v(static_cast<std::size_t>(frames));
    for (int n = 0; n < frames; ++n)
        v[static_cast<std::size_t>(n)] =
            out.channel(static_cast<std::size_t>(ch))[static_cast<std::size_t>(n)];
    return v;
}

/// Select one shape by soloing its depth. The mixer subsumes the selector this
/// replaced, and every test that wants a pure shape says so explicitly.
void solo(format::HeadlessHost& host, state::ParamID shape) {
    for (auto id : {LfoProcessor::kSine, LfoProcessor::kTriangle,
                    LfoProcessor::kSaw, LfoProcessor::kSquare})
        host.state().set_value(id, id == shape ? 1.0f : 0.0f);
}

}  // namespace

TEST_CASE("LFO descriptor is a bipolar modulation pair", "[brew][lfo]") {
    auto proc = create_lfo();
    const auto d = proc->descriptor();
    REQUIRE(d.name == "LFO");
    REQUIRE(d.manufacturer == "Bitches Brew");
    REQUIRE_FALSE(d.accepts_midi);
}

// ------------------------------------------------------------------- shapes

TEST_CASE("LFO shapes hit their golden values", "[brew][lfo][golden]") {
    SECTION("sine crosses zero rising at phase 0") {
        REQUIRE_THAT(lfo_shape(Waveform::sine, 0.0), WithinAbs(0.0, 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::sine, 0.25), WithinAbs(1.0, 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::sine, 0.5), WithinAbs(0.0, 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::sine, 0.75), WithinAbs(-1.0, 1e-6));
    }

    SECTION("triangle starts at its trough and peaks at half a cycle") {
        REQUIRE_THAT(lfo_shape(Waveform::triangle, 0.0), WithinAbs(-1.0, 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::triangle, 0.25), WithinAbs(0.0, 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::triangle, 0.5), WithinAbs(1.0, 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::triangle, 0.75), WithinAbs(0.0, 1e-6));
    }

    SECTION("saws are each other's mirror") {
        for (double p : {0.0, 0.1, 0.5, 0.9}) {
            REQUIRE_THAT(lfo_shape(Waveform::saw_up, p),
                         WithinAbs(-lfo_shape(Waveform::saw_down, p), 1e-6));
        }
        REQUIRE_THAT(lfo_shape(Waveform::saw_up, 0.0), WithinAbs(-1.0, 1e-6));
    }

    SECTION("square starts high and flips at half a cycle") {
        REQUIRE(lfo_shape(Waveform::square, 0.0) == 1.0f);
        REQUIRE(lfo_shape(Waveform::square, 0.499) == 1.0f);
        REQUIRE(lfo_shape(Waveform::square, 0.5) == -1.0f);
        REQUIRE(lfo_shape(Waveform::square, 0.999) == -1.0f);
    }

    // A phase of 1.0 is the same instant as 0.0, and a negative phase must index
    // the end of the cycle rather than the wrong half of it.
    SECTION("phase wraps in both directions") {
        REQUIRE_THAT(lfo_shape(Waveform::saw_up, 1.0),
                     WithinAbs(lfo_shape(Waveform::saw_up, 0.0), 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::saw_up, -0.25),
                     WithinAbs(lfo_shape(Waveform::saw_up, 0.75), 1e-6));
        REQUIRE(wrap_phase(-0.25) == 0.75);
        REQUIRE(wrap_phase(2.25) == 0.25);
    }
}

TEST_CASE("waveform parameter clamps rather than wraps", "[brew][lfo]") {
    REQUIRE(waveform_from_param(0.0f) == Waveform::sine);
    REQUIRE(waveform_from_param(4.0f) == Waveform::square);
    // A host handing back an out-of-range value must not select a different shape.
    REQUIRE(waveform_from_param(-3.0f) == Waveform::sine);
    REQUIRE(waveform_from_param(99.0f) == Waveform::square);
    // Rounding, not truncation: 1.6 is nearer triangle's neighbour than triangle.
    REQUIRE(waveform_from_param(1.6f) == Waveform::saw_up);
}

TEST_CASE("unipolar mapping folds the negative half up", "[brew][lfo]") {
    REQUIRE(to_unipolar(-1.0f) == 0.0f);
    REQUIRE(to_unipolar(0.0f) == 0.5f);
    REQUIRE(to_unipolar(1.0f) == 1.0f);
}

// --------------------------------------------------------------- phase lock

TEST_CASE("LFO phase is locked to the host position", "[brew][lfo][phase]") {
    // One cycle per beat: the phase is the fractional part of the beat count.
    REQUIRE_THAT(lfo_phase(0.0, 1.0), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(lfo_phase(2.25, 1.0), WithinAbs(0.25, 1e-12));
    REQUIRE_THAT(lfo_phase(57.5, 1.0), WithinAbs(0.5, 1e-12));

    SECTION("one cycle per bar means phase 0 on every downbeat") {
        for (double bar = 0.0; bar < 5.0; bar += 1.0)
            REQUIRE_THAT(lfo_phase(bar * 4.0, 4.0), WithinAbs(0.0, 1e-12));
    }

    SECTION("a degenerate rate yields a still LFO rather than a division by zero") {
        REQUIRE(lfo_phase(3.0, 0.0) == 0.0);
    }
}

// The reason for deriving phase from position instead of accumulating it: the
// same beat yields the same voltage no matter how the host chopped the timeline.
TEST_CASE("LFO output is independent of block size", "[brew][lfo][phase]") {
    format::HeadlessHost a(create_lfo), b(create_lfo);
    a.prepare(kSampleRate, 4096, 2, 2);
    b.prepare(kSampleRate, 4096, 2, 2);

    // `a` renders one 2048-sample block; `b` renders it as four 512s.
    const auto whole = render(a, 0.0, 2048, 0);

    std::vector<float> pieces;
    const double step = beats_per_sample(kTempo, kSampleRate) * 512.0;
    for (int i = 0; i < 4; ++i) {
        const auto part = render(b, step * i, 512, 0);
        pieces.insert(pieces.end(), part.begin(), part.end());
    }

    REQUIRE(pieces.size() == whole.size());
    for (std::size_t n = 0; n < whole.size(); ++n) {
        CAPTURE(n);
        REQUIRE_THAT(pieces[n], WithinAbs(whole[n], 1e-6));
    }
}

TEST_CASE("LFO advances within a block rather than stepping per block",
          "[brew][lfo][phase]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 4096, 2, 2);
    solo(host, LfoProcessor::kSaw);

    const auto block = render(host, 0.0, 512, 0);
    // A block-rate LFO would hold one value for all 512 samples; a stepped CV is
    // an audible zipper on whatever it drives.
    REQUIRE(block.front() != block.back());
    for (std::size_t n = 1; n < block.size(); ++n) REQUIRE(block[n] > block[n - 1]);
}

TEST_CASE("a stopped transport holds the LFO at the playhead",
          "[brew][lfo][phase]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 4096, 2, 2);
    solo(host, LfoProcessor::kSaw);

    const auto block = render(host, 0.3, 256, 0, /*playing=*/false);
    for (float v : block) REQUIRE(v == block.front());
}

// --------------------------------------------------------------- quadrature

TEST_CASE("the quadrature output leads the main output by a quarter cycle",
          "[brew][lfo][quadrature]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 4096, 2, 2);
    solo(host, LfoProcessor::kSine);
    host.state().set_value(LfoProcessor::kBeatsPerCycle, 1.0f);

    const auto main = render(host, 0.0, 512, 0);
    const auto quad = render(host, 0.0, 512, 1);

    // Sampling the main output a quarter cycle later must reproduce the quadrature
    // output now. One cycle per beat at 120 BPM = 24000 samples, so a quarter is
    // 6000 — beyond this block, hence sampling `value_at` directly.
    const auto* lfo = static_cast<const LfoProcessor*>(host.processor());
    for (double beat : {0.0, 0.1, 0.37, 0.9}) {
        CAPTURE(beat);
        REQUIRE_THAT(lfo->value_at(beat, kQuadratureOffset),
                     WithinAbs(lfo->value_at(beat + 0.25), 1e-6));
    }

    // And sine + its quadrature trace a unit circle: sin² + cos² = 1.
    for (std::size_t n = 0; n < main.size(); n += 37) {
        CAPTURE(n);
        REQUIRE_THAT(main[n] * main[n] + quad[n] * quad[n], WithinAbs(1.0, 1e-5));
    }
}

// ------------------------------------------------------------- output stage

TEST_CASE("LFO honors the suite's output stage", "[brew][lfo]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 4096, 2, 2);
    solo(host, LfoProcessor::kSquare);

    REQUIRE(render(host, 0.0, 8, 0).front() == 1.0f);

    SECTION("scale attenuates") {
        host.state().set_value(LfoProcessor::kOutputScale, 0.25f);
        REQUIRE(render(host, 0.0, 8, 0).front() == 0.25f);
    }

    SECTION("invert flips polarity") {
        host.state().set_value(LfoProcessor::kInvert, 1.0f);
        REQUIRE(render(host, 0.0, 8, 0).front() == -1.0f);
    }

    // `Unipolar` is gone: `Offset` subsumes it, and generalizes. Half the depth
    // plus half an offset is the old unipolar square, and unlike a toggle it also
    // reaches every partial offset in between.
    SECTION("a depth and an offset keep the whole cycle positive") {
        host.state().set_value(LfoProcessor::kSquare, 0.5f);
        host.state().set_value(LfoProcessor::kOffset, 0.5f);
        REQUIRE(render(host, 0.0, 8, 0).front() == 1.0f);   // square high  → 1.0
        REQUIRE(render(host, 0.5, 8, 0).front() == 0.0f);   // square low   → 0.0
    }

    SECTION("phase offset rotates the cycle") {
        // A square offset by 180° is high exactly where it was low.
        host.state().set_value(LfoProcessor::kPhaseDegrees, 180.0f);
        REQUIRE(render(host, 0.0, 8, 0).front() == -1.0f);
    }
}

// Bypass means stop driving the patch — not freeze at whatever voltage the cycle
// happened to reach.
TEST_CASE("LFO emits nothing while bypassed", "[brew][lfo][safety][bypass]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 512, 2, 2);
    solo(host, LfoProcessor::kSquare);

    audio::Buffer<float> in(2, 512), out(2, 512);
    in.clear();
    out.clear();
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, 512);
    auto ov = out.view();

    format::ProcessContext ctx;
    ctx.sample_rate = kSampleRate;
    ctx.num_samples = 512;
    ctx.is_playing = true;
    ctx.tempo_bpm = kTempo;
    ctx.is_bypassed = true;
    host.process(ov, iv, ctx);

    for (int n = 0; n < 512; ++n) {
        REQUIRE(out.channel(0)[static_cast<std::size_t>(n)] == 0.0f);
        REQUIRE(out.channel(1)[static_cast<std::size_t>(n)] == 0.0f);
    }
}

// ------------------------------------------------------------------ the mixer

// The mixer subsumes the selector it replaced: one depth at full and the rest at
// zero is exactly the old single-shape behaviour.
TEST_CASE("depths sum, and a solo depth is the pure shape", "[brew][lfo][mix]") {
    LfoMix m{};
    m.sine = 1.0f;
    m.triangle = 0.0f;
    for (double p = 0.0; p < 1.0; p += 0.05) {
        CAPTURE(p);
        REQUIRE_THAT(lfo_mix_value(m, p, 0), WithinAbs(lfo_shape(Waveform::sine, p), 1e-6));
    }

    // Two shapes at once is their sum, sample for sample.
    LfoMix both = m;
    both.saw = 1.0f;
    for (double p = 0.0; p < 1.0; p += 0.05) {
        CAPTURE(p);
        REQUIRE_THAT(lfo_mix_value(both, p, 0),
                     WithinAbs(lfo_shape(Waveform::sine, p) + lfo_shape(Waveform::saw_up, p),
                               1e-6));
    }
}

// Depths are bipolar. A negative depth subtracts the shape, which is how the
// down-saw of the old enum is reached without a fifth entry.
TEST_CASE("a negative depth subtracts its shape", "[brew][lfo][mix]") {
    LfoMix up{};
    up.sine = 0.0f;
    up.saw = 1.0f;
    LfoMix down = up;
    down.saw = -1.0f;
    for (double p = 0.05; p < 1.0; p += 0.1) {
        CAPTURE(p);
        REQUIRE_THAT(lfo_mix_value(down, p, 0), WithinAbs(-lfo_mix_value(up, p, 0), 1e-6));
    }
}

// The sum is deliberately not clamped mid-chain: four depths at full reach 4.0,
// and flattening that before `offset` and the output scale have spoken would
// silently discard a mix the user asked for. `resolve_output` clamps once, at the
// jack.
TEST_CASE("the mix is clamped at the jack, not inside the mixer",
          "[brew][lfo][mix]") {
    LfoMix loud{};
    loud.sine = 1.0f;
    loud.triangle = 1.0f;
    loud.square = 1.0f;

    // Somewhere in the cycle three shapes at full depth reach past the rail. The
    // mixer must hand that overshoot on rather than swallowing it.
    float peak = 0.0f;
    for (int i = 0; i < 512; ++i)
        peak = std::max(peak, lfo_mix_value(loud, static_cast<double>(i) / 512, 0));
    REQUIRE(peak > 1.0f);

    // The jack sees it clamped, and exactly at the rail — proof the clamp is the
    // output stage's and not a quiet rescale inside the mixer.
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 4096, 2, 2);
    for (auto id : {LfoProcessor::kTriangle, LfoProcessor::kSquare})
        host.state().set_value(id, 1.0f);
    bool saw_rail = false;
    for (int i = 0; i < 64; ++i) {
        for (float v : render(host, static_cast<double>(i) / 64.0, 4, 0)) {
            REQUIRE(v <= 1.0f);
            REQUIRE(v >= -1.0f);
            if (v == 1.0f) saw_rail = true;
        }
    }
    REQUIRE(saw_rail);
}

// ------------------------------------------------------- asymmetry / pulse width

// Asymmetry moves the waveform's centre in time. With an identity pulse width, a
// square's duty cycle *is* the asymmetry — which is the cleanest way to observe a
// warp that applies to every shape.
TEST_CASE("asymmetry moves the waveform's centre", "[brew][lfo][mix]") {
    REQUIRE_THAT(warp_phase(0.3, 0.5), WithinAbs(0.3, 1e-12));  // 0.5 is identity
    REQUIRE_THAT(warp_phase(0.25, 0.25), WithinAbs(0.5, 1e-12));  // centre pulled early

    LfoMix m{};
    m.sine = 0.0f;
    m.square = 1.0f;
    m.asymmetry = 0.25f;
    int high = 0;
    const int n = 1000;
    for (int i = 0; i < n; ++i)
        if (lfo_mix_value(m, static_cast<double>(i) / n, 0) > 0.0f) ++high;
    REQUIRE(std::abs(high - 250) <= 2);
}

TEST_CASE("pulse width sets the square's duty cycle", "[brew][lfo][mix]") {
    LfoMix m{};
    m.sine = 0.0f;
    m.square = 1.0f;
    m.pulse_width = 0.8f;
    int high = 0;
    const int n = 1000;
    for (int i = 0; i < n; ++i)
        if (lfo_mix_value(m, static_cast<double>(i) / n, 0) > 0.0f) ++high;
    REQUIRE(std::abs(high - 800) <= 2);
}

// ---------------------------------------------------------------- sample & hold

// One level per cycle, held flat across it. That is what a noise source feeding a
// hardware sample-and-hold does, and a level that moved within the cycle would be
// noise, not S&H.
TEST_CASE("random holds one level for a whole cycle", "[brew][lfo][random]") {
    LfoMix m{};
    m.sine = 0.0f;
    m.random = 1.0f;
    const float held = lfo_mix_value(m, 0.0, 7);
    for (double p = 0.0; p < 1.0; p += 0.05) {
        CAPTURE(p);
        REQUIRE(lfo_mix_value(m, p, 7) == held);
    }
    // And it is a different level in the next cycle.
    REQUIRE(lfo_mix_value(m, 0.0, 8) != held);
}

// The property that separates this from every other CV utility: render twice, get
// the same samples. A generator advanced once per cycle could not promise it.
TEST_CASE("random is a pure function of the cycle and the seed",
          "[brew][lfo][random]") {
    LfoMix a{};
    a.sine = 0.0f;
    a.random = 1.0f;
    a.seed = 0;
    LfoMix b = a;

    // Two independent evaluations of cycle 12345 agree, bit for bit.
    for (std::int64_t cycle : {std::int64_t{0}, std::int64_t{12345}, std::int64_t{-7}}) {
        CAPTURE(cycle);
        REQUIRE(lfo_mix_value(a, 0.3, cycle) == lfo_mix_value(b, 0.3, cycle));
    }

    // A different seed rerolls the whole sequence.
    b.seed = 1;
    int differing = 0;
    for (std::int64_t cycle = 0; cycle < 32; ++cycle)
        if (lfo_mix_value(a, 0.0, cycle) != lfo_mix_value(b, 0.0, cycle)) ++differing;
    REQUIRE(differing >= 30);

    // And a negative cycle index — the timeline before the project's origin, which
    // a host will happily ask for — is as valid as any other.
    REQUIRE(std::isfinite(lfo_mix_value(a, 0.0, -99)));
}

TEST_CASE("the quadrature output holds the same random level across the circle",
          "[brew][lfo][random]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 4096, 2, 2);
    host.state().set_value(LfoProcessor::kSine, 0.0f);
    host.state().set_value(LfoProcessor::kRandom, 1.0f);
    host.state().set_value(LfoProcessor::kBeatsPerCycle, 1.0f);

    // The quadrature output is keyed on the cycle its own phase sits in, so it
    // steps a quarter cycle before the main output rather than tearing.
    const auto* lfo = static_cast<const LfoProcessor*>(host.processor());
    REQUIRE(lfo->value_at(0.0, kQuadratureOffset) == lfo->value_at(0.25));
    REQUIRE(lfo->value_at(0.6, kQuadratureOffset) == lfo->value_at(0.85));

    // The case that matters, and the only one that distinguishes the two keyings:
    // a quadrature phase that has crossed into the *next* cycle while the main
    // phase has not. Keying the S&H on the main cycle passes every assertion
    // above and tears exactly here.
    REQUIRE(lfo->value_at(0.8, kQuadratureOffset) == lfo->value_at(1.05));
    REQUIRE(lfo->value_at(0.8, kQuadratureOffset) != lfo->value_at(0.8));
}

// ---------------------------------------------------------------------- offset

TEST_CASE("offset shifts the whole waveform", "[brew][lfo][mix]") {
    LfoMix m{};
    m.sine = 1.0f;
    LfoMix shifted = m;
    shifted.offset = 0.25f;
    for (double p = 0.0; p < 1.0; p += 0.1) {
        CAPTURE(p);
        REQUIRE_THAT(lfo_mix_value(shifted, p, 0),
                     WithinAbs(lfo_mix_value(m, p, 0) + 0.25f, 1e-6));
    }
}

// ------------------------------------------------------------------- free run

TEST_CASE("free run measures cycles in seconds, not beats", "[brew][lfo][free]") {
    REQUIRE(lfo_cycles(RateMode::tempo, 4.0, 99.0, 2.0, 3.0) == 2.0);
    REQUIRE(lfo_cycles(RateMode::free, 99.0, 4.0, 2.0, 3.0) == 12.0);

    // A degenerate tempo rate yields no cycles rather than dividing by zero.
    REQUIRE(lfo_cycles(RateMode::tempo, 4.0, 0.0, 0.0, 1.0) == 0.0);
}

TEST_CASE("the free rate is clamped at both ends", "[brew][lfo][free][safety]") {
    // Zero hertz is a stopped LFO the user cannot restart from the knob; a
    // megahertz LFO is an audio-rate oscillator aliasing into the CV.
    REQUIRE(lfo_cycles(RateMode::free, 0.0, 10.0, 1.0, 0.0) ==
            10.0 * kMinFreeHz);
    REQUIRE(lfo_cycles(RateMode::free, 0.0, 10.0, 1.0, 1e6) ==
            10.0 * kMaxFreeHz);
    REQUIRE(lfo_cycles(RateMode::free, 0.0, 10.0, 1.0, -5.0) ==
            10.0 * kMinFreeHz);
}

TEST_CASE("a free-running LFO ignores the tempo", "[brew][lfo][free]") {
    // The whole point of the mode. Same sample position, different tempo, same
    // voltage — otherwise it is not free of the transport at all.
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 512, 2, 2);
    host.state().set_value(LfoProcessor::kRateMode, 1.0f);
    host.state().set_value(LfoProcessor::kFreeHz, 2.0f);

    const auto at_120 = render_free(host, 12345, 256, 0, 120.0);
    const auto at_71 = render_free(host, 12345, 256, 0, 71.5);
    REQUIRE(at_120 == at_71);

    // ...and the tempo-locked mode does not.
    host.state().set_value(LfoProcessor::kRateMode, 0.0f);
    REQUIRE(render_free(host, 12345, 256, 0, 120.0) !=
            render_free(host, 12345, 256, 0, 71.5));
}

TEST_CASE("free run is derived from the position, never accumulated",
          "[brew][lfo][free][safety]") {
    // Free-running must not mean stateful. Play into a block, versus locate
    // straight to it: the same samples, or a bounce is not reproducible and a
    // locate lands in the wrong place.
    format::HeadlessHost played(create_lfo), located(create_lfo);
    for (auto* h : {&played, &located}) {
        h->prepare(kSampleRate, 512, 2, 2);
        h->state().set_value(LfoProcessor::kRateMode, 1.0f);
        h->state().set_value(LfoProcessor::kFreeHz, 3.7f);
        h->state().set_value(LfoProcessor::kRandom, 0.6f);
    }
    constexpr int kFrames = 256;
    std::vector<float> from_playing;
    for (std::int64_t b = 0; b <= 20; ++b)
        from_playing = render_free(played, b * kFrames, kFrames, 0);
    const auto from_locate = render_free(located, 20 * kFrames, kFrames, 0);
    REQUIRE(from_playing == from_locate);
}

TEST_CASE("one free-run cycle is exactly one over the rate, in seconds",
          "[brew][lfo][free]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 512, 2, 2);
    host.state().set_value(LfoProcessor::kRateMode, 1.0f);
    host.state().set_value(LfoProcessor::kFreeHz, 2.0f);

    const auto* lfo = static_cast<const LfoProcessor*>(host.processor());
    // At 2 Hz a cycle is half a second. Points one cycle apart agree.
    REQUIRE_THAT(lfo->value_at_time(0.125),
                 Catch::Matchers::WithinAbs(lfo->value_at_time(0.625), 1e-6));
    // And a quarter cycle apart, they do not.
    REQUIRE(lfo->value_at_time(0.125) != lfo->value_at_time(0.25));
}

TEST_CASE("a stopped transport holds a free-running LFO still",
          "[brew][lfo][free]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 512, 2, 2);
    host.state().set_value(LfoProcessor::kRateMode, 1.0f);
    host.state().set_value(LfoProcessor::kFreeHz, 5.0f);
    const auto out = render_free(host, 4321, 128, 0, kTempo, /*playing=*/false);
    for (float v : out) REQUIRE(v == out.front());
}

// ----------------------------------------------------- phase and the hold agree

TEST_CASE("the sample-and-hold steps where the waveform wraps, not elsewhere",
          "[brew][lfo][random]") {
    // The phase offset shifts the shape. If the hold's cycle index does not take
    // the same offset, the held random value steps in the middle of the visible
    // cycle — the scope shows a waveform whose random component jumps at a point
    // that corresponds to nothing.
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 512, 2, 2);
    host.state().set_value(LfoProcessor::kSine, 0.0f);
    host.state().set_value(LfoProcessor::kRandom, 1.0f);
    host.state().set_value(LfoProcessor::kPhaseDegrees, 180.0f);

    const auto* lfo = static_cast<const LfoProcessor*>(host.processor());
    // With a half-cycle offset, the wrap sits at cycles = 0.5. Either side of it
    // the hold must differ; within a half-cycle either side it must not.
    const float before = lfo->value_at_cycles(0.49);
    const float after = lfo->value_at_cycles(0.51);
    REQUIRE(before != after);
    REQUIRE(lfo->value_at_cycles(0.1) == before);
    REQUIRE(lfo->value_at_cycles(0.9) == after);
    // ...and it does *not* step at cycles = 0, where an un-offset index would.
    REQUIRE(lfo->value_at_cycles(-0.1) == lfo->value_at_cycles(0.1));
}

TEST_CASE("straight swing leaves the LFO bit-identical", "[brew][lfo][swing]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 512, 2, 2);

    // Swing has a defined identity, and it must be *exact*: a user who never
    // touches the control, and a user who sets it to 50 and back, must render the
    // same file. An `Approx` here would hide a warp that is merely small.
    const auto straight = render(host, 0.37, 512, 0);

    host.state().set_value(LfoProcessor::kSwingPercent, 50.0f);
    REQUIRE(render(host, 0.37, 512, 0) == straight);

    host.state().set_value(LfoProcessor::kSwingUnit, 1.0f);
    REQUIRE(render(host, 0.37, 512, 0) == straight);
}

TEST_CASE("swing delays the LFO's second eighth", "[brew][lfo][swing]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 512, 2, 2);
    host.state().set_value(LfoProcessor::kBeatsPerCycle, 1.0f);

    // The phase the LFO reaches at a sounding beat is the phase the straight LFO
    // would have reached at the *straight* beat that sounding beat stands for.
    // At 66% swing on eighths the swing pair is one beat long, and its midpoint —
    // straight beat 0.5 — is pushed out to sounding beat 0.66.
    const float straight_mid = render(host, 0.5, 1, 0)[0];

    host.state().set_value(LfoProcessor::kSwingPercent, 66.0f);
    const float swung_early = render(host, 0.5, 1, 0)[0];
    REQUIRE_THAT(render(host, 0.66, 1, 0)[0], WithinAbs(straight_mid, 1e-5f));

    // ...and it is genuinely warped, not merely offset: the sounding beat that
    // used to carry the midpoint no longer does.
    REQUIRE(swung_early != straight_mid);

    // Downbeats are fixed points of the warp — a swung LFO still starts its cycle
    // on the beat, or it would drift against everything else in the project.
    host.state().set_value(LfoProcessor::kSwingPercent, 50.0f);
    const float straight_downbeat = render(host, 1.0, 1, 0)[0];
    host.state().set_value(LfoProcessor::kSwingPercent, 66.0f);
    REQUIRE(render(host, 1.0, 1, 0)[0] == straight_downbeat);
}

TEST_CASE("swing moves the sixteenth when asked", "[brew][lfo][swing]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 512, 2, 2);
    host.state().set_value(LfoProcessor::kBeatsPerCycle, 1.0f);
    host.state().set_value(LfoProcessor::kSwingPercent, 66.0f);

    // Eighth-swing and sixteenth-swing are different warps of the same timeline.
    // A control that quietly ignored its unit would pass every test above.
    //
    // Beat 0.4 specifically. The two warps agree exactly wherever a position is
    // in the *first* half of both swing pairs — at 0.3 both map through the same
    // `p / 2a` — so a test there passes whether or not the unit is read. 0.4 is
    // past the sixteenth pair's midpoint (0.33) and short of the eighth's (0.66),
    // which is the only region where the two disagree.
    const auto eighths = render(host, 0.4, 256, 0);
    host.state().set_value(LfoProcessor::kSwingUnit, 1.0f);
    REQUIRE(render(host, 0.4, 256, 0) != eighths);
}

TEST_CASE("a free-running LFO ignores swing", "[brew][lfo][swing][free]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 512, 2, 2);
    host.state().set_value(LfoProcessor::kRateMode, 1.0f);
    host.state().set_value(LfoProcessor::kFreeHz, 3.0f);

    // Swing subdivides a beat. A free-running LFO has no beats, so a swung hertz
    // rate is just a wrong hertz rate. Ignored, not approximated.
    const auto free = render_free(host, 12345, 256, 0);
    host.state().set_value(LfoProcessor::kSwingPercent, 66.0f);
    REQUIRE(render_free(host, 12345, 256, 0) == free);
    host.state().set_value(LfoProcessor::kSwingUnit, 1.0f);
    REQUIRE(render_free(host, 12345, 256, 0) == free);
}

TEST_CASE("a swung LFO still locates and bounces identically",
          "[brew][lfo][swing]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 512, 2, 2);
    host.state().set_value(LfoProcessor::kSwingPercent, 62.5f);

    // The whole point of warping the position rather than accumulating a phase:
    // swing costs nothing in determinism. Reaching beat 2.25 by playing through
    // it and by dropping the playhead on it must give the same samples.
    render(host, 1.0, 512, 0);   // play through the region first
    render(host, 2.0, 512, 0);
    const auto played = render(host, 2.25, 128, 0);

    // A fresh instance dropped straight onto beat 2.25 must agree sample for
    // sample. Nothing in the swung path may carry state between blocks.
    format::HeadlessHost fresh(create_lfo);
    fresh.prepare(kSampleRate, 512, 2, 2);
    fresh.state().set_value(LfoProcessor::kSwingPercent, 62.5f);
    REQUIRE(render(fresh, 2.25, 128, 0) == played);

    // And block size must not matter: two 64-frame blocks reproduce one 128-frame
    // one. Not bit-exactly — the host hands the second block a beat position it
    // computed as `2.25 + bps*64`, and `bps*64 + bps*n` is not the same double as
    // `bps*(64+n)`. That is the host's rounding, not state carried across the
    // boundary, so the claim is agreement to within it.
    format::HeadlessHost split(create_lfo);
    split.prepare(kSampleRate, 512, 2, 2);
    split.state().set_value(LfoProcessor::kSwingPercent, 62.5f);
    const double bps = beats_per_sample(kTempo, kSampleRate);
    auto a = render(split, 2.25, 64, 0);
    const auto b = render(split, 2.25 + bps * 64.0, 64, 0);
    a.insert(a.end(), b.begin(), b.end());
    REQUIRE(a.size() == played.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE_THAT(a[i], WithinAbs(played[i], 1e-6f));
}
