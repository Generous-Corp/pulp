// Framework-layer edge-case corpus for the audio contract.
//
// Item 7 from the #356 stage-1 audit: deterministic edge-case corpus
// (silence, impulse, DC, denormal-prone tails, sample-rate change,
// zero-length buffer) and item 3 (plugin-level automation + smoothing
// contract under block transitions).
//
// Invariants pinned:
//   1. Zero-length process() is a safe no-op — the framework must
//      tolerate the degenerate block size hosts can hand us.
//   2. Denormal-prone input (sub-normal floats) comes out finite. A
//      future regression here manifests as an audio-thread CPU cliff,
//      not a loud bug.
//   3. Impulse in ⇒ impulse out at sample N for PulpGain (unity gain
//      is zero-latency; the sample never gets delayed or duplicated).
//   4. Extreme DC offset doesn't produce Inf/NaN and stays finite.
//   5. Back-to-back blocks with different gain settings produce
//      deterministic output (param updates are block-boundary crisp
//      even without smoothing).
//   6. Re-prepare() with identical config is idempotent (same SR +
//      same block ⇒ same output on the same input).
//
// This file deliberately does NOT cover:
//   - NaN/Inf input policy — that's test_negative_path.cpp's turf.
//   - SR × block matrix — #48 / test_audio_determinism_matrix.cpp.
//   - Per-processor golden behaviour — per-example test_*.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/headless.hpp>

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "pulp_gain.hpp"

using Catch::Matchers::WithinAbs;

namespace {

constexpr double kSR = 48000.0;

pulp::audio::Buffer<float> make_stereo(int samples, float value) {
    pulp::audio::Buffer<float> b(2, samples);
    for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < samples; ++i) {
            b.channel(ch)[i] = value;
        }
    }
    return b;
}

void process_once(pulp::format::HeadlessHost& host,
                  const pulp::audio::Buffer<float>& in,
                  pulp::audio::Buffer<float>& out) {
    const float* ip[2] = {in.channel(0).data(),
                          in.num_channels() > 1 ? in.channel(1).data()
                                                : in.channel(0).data()};
    pulp::audio::BufferView<const float> iv(ip, in.num_channels(),
                                            in.num_samples());
    auto ov = out.view();
    host.process(ov, iv);
}

}  // namespace

// ── 1. Zero-length block is a safe no-op ─────────────────────────────

TEST_CASE("Audio edges: zero-length process() is a safe no-op",
          "[audio][edges][issue-356]") {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);
    host.prepare(kSR, 512);

    // Actually process 0 samples. Hosts do this occasionally when a
    // bus is disabled or during transport boundaries.
    const float* ip[2] = {nullptr, nullptr};
    float* op[2] = {nullptr, nullptr};
    pulp::audio::BufferView<const float> iv(ip, 2, 0);
    pulp::audio::BufferView<float> ov(op, 2, 0);

    // The contract: process() returns cleanly. If PulpGain's loop or
    // the framework wrapper ever dereferences without a size guard
    // this will segfault here.
    host.process(ov, iv);
    SUCCEED("zero-length process() returned cleanly");
}

// ── 2. Denormal-prone input stays finite ─────────────────────────────

TEST_CASE("Audio edges: sub-normal float input comes out finite",
          "[audio][edges][denormal][issue-356]") {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);
    host.prepare(kSR, 256);

    // std::numeric_limits<float>::min() is the smallest *normal* value;
    // dividing by 2 gets us squarely in sub-normal territory without
    // flushing to zero at compile time.
    const float subnormal = std::numeric_limits<float>::min() / 2.0f;

    pulp::audio::Buffer<float> in(2, 256), out(2, 256);
    for (int i = 0; i < 256; ++i) {
        in.channel(0)[i] = subnormal;
        in.channel(1)[i] = -subnormal;
    }

    process_once(host, in, out);

    for (int i = 0; i < 256; ++i) {
        REQUIRE(std::isfinite(out.channel(0)[i]));
        REQUIRE(std::isfinite(out.channel(1)[i]));
    }
}

// ── 3. Impulse at sample N comes out at sample N ─────────────────────

TEST_CASE("Audio edges: impulse preserves position through PulpGain",
          "[audio][edges][latency][issue-356]") {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);
    host.prepare(kSR, 512);

    pulp::audio::Buffer<float> in(2, 512), out(2, 512);
    for (int i = 0; i < 512; ++i) {
        in.channel(0)[i] = 0.0f;
        in.channel(1)[i] = 0.0f;
    }
    in.channel(0)[42] = 1.0f;   // single impulse on L at sample 42
    in.channel(1)[200] = -1.0f; // and on R at sample 200

    process_once(host, in, out);

    for (int i = 0; i < 512; ++i) {
        if (i == 42) {
            REQUIRE_THAT(out.channel(0)[42], WithinAbs(1.0, 1e-6));
        } else {
            REQUIRE(out.channel(0)[i] == 0.0f);
        }
        if (i == 200) {
            REQUIRE_THAT(out.channel(1)[200], WithinAbs(-1.0, 1e-6));
        } else {
            REQUIRE(out.channel(1)[i] == 0.0f);
        }
    }
}

// ── 4. Extreme DC offset stays finite ────────────────────────────────

TEST_CASE("Audio edges: full-scale DC input stays finite",
          "[audio][edges][dc][issue-356]") {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);
    host.prepare(kSR, 512);
    host.state().set_value(2, 12.0f);  // Output Gain = +12 dB (≈ 4×)

    auto in = make_stereo(512, 1.0f);  // 0 dBFS DC
    pulp::audio::Buffer<float> out(2, 512);

    process_once(host, in, out);

    for (int i = 0; i < 512; ++i) {
        REQUIRE(std::isfinite(out.channel(0)[i]));
        REQUIRE(std::isfinite(out.channel(1)[i]));
    }
}

// ── 5. Parameter change between blocks is crisp ──────────────────────

TEST_CASE("Audio edges: gain change between blocks lands on the next block",
          "[audio][edges][automation][issue-356]") {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);
    host.prepare(kSR, 256);

    // Block 1: unity gain ⇒ input passes through at 1.0×.
    // Block 2: output gain -6 dB ⇒ ~0.501×.
    auto in = make_stereo(256, 0.4f);
    pulp::audio::Buffer<float> out_block1(2, 256), out_block2(2, 256);

    process_once(host, in, out_block1);

    host.state().set_value(2, -6.0f);  // Output Gain param
    process_once(host, in, out_block2);

    // Block 1: unity. Block 2: reduced. PulpGain has no smoothing, so
    // the new gain takes effect immediately at the start of block 2.
    REQUIRE_THAT(out_block1.channel(0)[128], WithinAbs(0.4, 1e-5));
    REQUIRE(out_block2.channel(0)[0] < 0.4f * 0.6f);
}

// ── 6. Re-prepare() is idempotent at identical config ────────────────

TEST_CASE("Audio edges: re-prepare at identical config produces same output",
          "[audio][edges][prepare][issue-356]") {
    auto in = make_stereo(256, 0.25f);

    pulp::format::HeadlessHost host1(pulp::examples::create_pulp_gain);
    host1.prepare(kSR, 256);
    pulp::audio::Buffer<float> out1(2, 256);
    process_once(host1, in, out1);

    pulp::format::HeadlessHost host2(pulp::examples::create_pulp_gain);
    host2.prepare(kSR, 256);
    host2.prepare(kSR, 256);  // redundant prepare — must be a no-op
    pulp::audio::Buffer<float> out2(2, 256);
    process_once(host2, in, out2);

    for (int i = 0; i < 256; ++i) {
        REQUIRE(out1.channel(0)[i] == out2.channel(0)[i]);
        REQUIRE(out1.channel(1)[i] == out2.channel(1)[i]);
    }
}
