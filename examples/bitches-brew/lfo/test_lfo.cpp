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

#include <cmath>
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
    host.state().set_value(LfoProcessor::kWaveform,
                           static_cast<float>(Waveform::saw_up));

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
    host.state().set_value(LfoProcessor::kWaveform,
                           static_cast<float>(Waveform::saw_up));

    const auto block = render(host, 0.3, 256, 0, /*playing=*/false);
    for (float v : block) REQUIRE(v == block.front());
}

// --------------------------------------------------------------- quadrature

TEST_CASE("the quadrature output leads the main output by a quarter cycle",
          "[brew][lfo][quadrature]") {
    format::HeadlessHost host(create_lfo);
    host.prepare(kSampleRate, 4096, 2, 2);
    host.state().set_value(LfoProcessor::kWaveform,
                           static_cast<float>(Waveform::sine));
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
    host.state().set_value(LfoProcessor::kWaveform,
                           static_cast<float>(Waveform::square));

    REQUIRE(render(host, 0.0, 8, 0).front() == 1.0f);

    SECTION("scale attenuates") {
        host.state().set_value(LfoProcessor::kOutputScale, 0.25f);
        REQUIRE(render(host, 0.0, 8, 0).front() == 0.25f);
    }

    SECTION("invert flips polarity") {
        host.state().set_value(LfoProcessor::kInvert, 1.0f);
        REQUIRE(render(host, 0.0, 8, 0).front() == -1.0f);
    }

    SECTION("unipolar keeps the whole cycle positive") {
        host.state().set_value(LfoProcessor::kUnipolar, 1.0f);
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
    host.state().set_value(LfoProcessor::kWaveform,
                           static_cast<float>(Waveform::square));

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
