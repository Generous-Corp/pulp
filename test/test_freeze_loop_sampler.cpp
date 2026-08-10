// FreezeLoopSampler — capture recent audio and play it as a seamless loop.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <pulp/signal/freeze_loop_sampler.hpp>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {
constexpr double kPi = 3.14159265358979323846;

// Write `n` frames of a mono sine into the sampler (one channel).
void write_sine(FreezeLoopSampler& s, double cyc, int n, int from = 0) {
    std::vector<float> buf(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        buf[static_cast<size_t>(i)] = static_cast<float>(std::sin(2.0 * kPi * cyc * (from + i)));
    const float* in[1] = {buf.data()};
    s.write(in, n);
}

float max_step(const std::vector<float>& y) {
    float m = 0.0f;
    for (size_t i = 1; i < y.size(); ++i)
        m = std::max(m, std::abs(y[i] - y[i - 1]));
    return m;
}
} // namespace

TEST_CASE("FreezeLoopSampler loops captured audio click-free", "[signal][freeze-loop]") {
    FreezeLoopSampler s;
    s.prepare(/*channels=*/1, /*capacity=*/8192, /*crossfade=*/256);
    // A loop length deliberately NOT an integer number of sine periods, so a
    // hard wrap would click.
    write_sine(s, 0.013, 4096);
    s.freeze(2000);
    REQUIRE(s.frozen());
    REQUIRE(s.loop_length() == 2000);

    std::vector<float> out(8000, 0.0f);
    float* op[1] = {out.data()};
    s.read(op, 8000);
    // Periodic at the loop length.
    for (int i = 0; i < 2000; ++i)
        REQUIRE_THAT(out[static_cast<size_t>(i)],
                     WithinAbs(out[static_cast<size_t>(i + 2000)], 1e-5f));
    // The natural per-sample step of this sine is ~2*pi*0.013 ≈ 0.082; the
    // crossfade must keep the looped output within a small multiple of that
    // (a hard click would be a large jump toward ~2.0).
    REQUIRE(max_step(out) < 0.2f);
}

TEST_CASE("FreezeLoopSampler64 snapshots and restores double loops", "[signal][freeze-loop][f64]") {
    FreezeLoopSampler64 a;
    a.prepare(1, 2048, 64);
    std::vector<double> in(1024);
    for (int i = 0; i < 1024; ++i)
        in[static_cast<size_t>(i)] = std::sin(2.0 * kPi * 0.017 * i);
    const double* ip[1] = {in.data()};
    a.write(ip, static_cast<int>(in.size()));
    a.freeze(512);

    const auto blob = a.snapshot();
    FreezeLoopSampler64 b;
    b.prepare(1, 2048, 64);
    REQUIRE(b.restore(blob));

    std::vector<double> ao(700), bo(700);
    double* ap[1] = {ao.data()};
    double* bp[1] = {bo.data()};
    a.read(ap, static_cast<int>(ao.size()));
    b.read(bp, static_cast<int>(bo.size()));
    for (std::size_t i = 0; i < ao.size(); ++i)
        REQUIRE_THAT(bo[i], WithinAbs(ao[i], 1e-12));
}

TEST_CASE("FreezeLoopSampler crossfade beats a hard loop at the boundary",
          "[signal][freeze-loop]") {
    auto boundary_step = [](int crossfade) {
        FreezeLoopSampler s;
        s.prepare(1, 8192, crossfade);
        write_sine(s, 0.0131, 4096);
        s.freeze(1500);
        std::vector<float> out(4500, 0.0f);
        float* op[1] = {out.data()};
        s.read(op, 4500);
        return max_step(out);
    };
    const float hard = boundary_step(0);
    const float faded = boundary_step(256);
    INFO("hard step=" << hard << "  crossfaded step=" << faded);
    REQUIRE(faded < hard);        // crossfade removes the click
    REQUIRE(faded < hard * 0.5f); // by a wide margin
}

TEST_CASE("FreezeLoopSampler release stops looping", "[signal][freeze-loop]") {
    FreezeLoopSampler s;
    s.prepare(1, 4096, 128);
    write_sine(s, 0.02, 2048);
    s.freeze(1000);
    REQUIRE(s.frozen());
    s.release();
    REQUIRE_FALSE(s.frozen());
    std::vector<float> out(500, 0.123f);
    float* op[1] = {out.data()};
    s.read(op, 500); // no-op when released
    for (float v : out)
        REQUIRE(v == 0.123f);
}

TEST_CASE("FreezeLoopSampler reset exposes no stale legacy freeze audio",
          "[signal][freeze-loop][reset]") {
    FreezeLoopSampler sampler;
    sampler.prepare(1, 64, 0);
    const std::array<float, 4> input{1, 2, 3, 4};
    const float* inputs[]{input.data()};
    sampler.write(inputs, static_cast<int>(input.size()));
    sampler.reset();
    sampler.freeze(1);
    REQUIRE_FALSE(sampler.frozen());
    std::array<float, 1> output{0.25f};
    float* outputs[]{output.data()};
    sampler.read(outputs, 1);
    REQUIRE(output[0] == 0.25f);
}

TEST_CASE("FreezeLoopSampler snapshot/restore round-trips the loop", "[signal][freeze-loop]") {
    FreezeLoopSampler a;
    a.prepare(1, 8192, 256);
    write_sine(a, 0.017, 4096);
    a.freeze(1800);
    auto blob = a.snapshot();
    REQUIRE(!blob.empty());

    FreezeLoopSampler b;
    b.prepare(1, 8192, 256);
    REQUIRE(b.restore(blob));
    REQUIRE(b.frozen());
    REQUIRE(b.loop_length() == 1800);

    std::vector<float> oa(1800, 0.0f), ob(1800, 0.0f);
    float* pa[1] = {oa.data()};
    float* pb[1] = {ob.data()};
    a.read(pa, 1800);
    b.read(pb, 1800);
    for (int i = 0; i < 1800; ++i)
        REQUIRE_THAT(oa[static_cast<size_t>(i)], WithinAbs(ob[static_cast<size_t>(i)], 1e-6f));
    const auto before_invalid_restore = b.snapshot();
    auto malformed = before_invalid_restore;
    malformed.pop_back();
    REQUIRE_FALSE(b.restore(malformed));
    REQUIRE(b.snapshot() == before_invalid_restore);
    malformed = before_invalid_restore;
    malformed[3] = std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(b.restore(malformed));
    REQUIRE(b.snapshot() == before_invalid_restore);
    malformed = before_invalid_restore;
    malformed.push_back(0.0f);
    REQUIRE_FALSE(b.restore(malformed));
    REQUIRE(b.snapshot() == before_invalid_restore);
    // A malformed blob is rejected.
    FreezeLoopSampler c;
    c.prepare(1, 8192, 256);
    REQUIRE_FALSE(c.restore({2.0f, 100.0f}));
    REQUIRE_FALSE(c.frozen());
}

TEST_CASE("FreezeLoopSampler stereo channels stay independent", "[signal][freeze-loop]") {
    FreezeLoopSampler s;
    s.prepare(2, 4096, 64);
    std::vector<float> l(2048), r(2048);
    for (int i = 0; i < 2048; ++i) {
        l[static_cast<size_t>(i)] = static_cast<float>(std::sin(2.0 * kPi * 0.02 * i));
        r[static_cast<size_t>(i)] = static_cast<float>(std::sin(2.0 * kPi * 0.05 * i));
    }
    const float* in[2] = {l.data(), r.data()};
    s.write(in, 2048);
    s.freeze(1000);
    std::vector<float> ol(1000), orr(1000);
    float* op[2] = {ol.data(), orr.data()};
    s.read(op, 1000);
    // The two lanes hold different content.
    float diff = 0.0f;
    for (int i = 0; i < 1000; ++i)
        diff += std::abs(ol[static_cast<size_t>(i)] - orr[static_cast<size_t>(i)]);
    REQUIRE(diff > 1.0f);
}

TEST_CASE("FreezeLoopSampler exact recent capture rejects instead of clamping",
          "[signal][freeze-loop][history]") {
    FreezeLoopSampler sampler;
    sampler.prepare(1, 8, 0);
    const std::array<float, 5> first{1, 2, 3, 4, 5};
    const float* first_input[]{first.data()};
    sampler.write(first_input, static_cast<int>(first.size()));
    REQUIRE(sampler.available_history() == 5);
    REQUIRE(sampler.try_freeze_recent(6) == FreezeLoopSampler::CaptureResult::InsufficientHistory);
    REQUIRE_FALSE(sampler.frozen());

    REQUIRE(sampler.try_freeze_recent(5) == FreezeLoopSampler::CaptureResult::Success);
    REQUIRE(sampler.available_history() == 0);
    for (int frame = 0; frame < 5; ++frame)
        REQUIRE(sampler.loop_sample(0, frame) == first[static_cast<std::size_t>(frame)]);

    const std::array<float, 8> second{6, 7, 8, 9, 10, 11, 12, 13};
    const float* second_input[]{second.data()};
    sampler.write(second_input, static_cast<int>(second.size()));
    REQUIRE(sampler.available_history() == sampler.capacity());
    REQUIRE(sampler.try_freeze_recent(8) == FreezeLoopSampler::CaptureResult::Success);
    constexpr std::array<float, 8> expected{6, 7, 8, 9, 10, 11, 12, 13};
    for (int frame = 0; frame < 8; ++frame)
        REQUIRE(sampler.loop_sample(0, frame) == expected[static_cast<std::size_t>(frame)]);
    REQUIRE(sampler.try_freeze_recent(9) == FreezeLoopSampler::CaptureResult::ExceedsCapacity);
}

TEST_CASE("FreezeLoopSampler legacy freeze clamps nonpositive lengths",
          "[signal][freeze-loop][compatibility]") {
    FreezeLoopSampler sampler;
    sampler.prepare(1, 8, 0);
    const std::array<float, 3> history{1.0f, 2.0f, 3.0f};
    const float* input[]{history.data()};
    sampler.write(input, static_cast<int>(history.size()));

    sampler.freeze(0);
    REQUIRE(sampler.frozen());
    REQUIRE(sampler.loop_length() == 1);
    REQUIRE(sampler.loop_sample(0, 0) == 3.0f);
}

TEST_CASE("FreezeLoopSampler preparation rejects unsafe and unrepresentable bounds",
          "[signal][freeze-loop][prepare]") {
    FreezeLoopSampler sampler;
    sampler.prepare(1, 64, 0);
    REQUIRE(sampler.channels() == 1);
    REQUIRE(sampler.capacity() == 64);

    sampler.prepare(-1, 64, 0);
    REQUIRE(sampler.channels() == 0);
    REQUIRE(sampler.capacity() == 0);

    sampler.prepare(1, 64, std::numeric_limits<int>::max());
    REQUIRE(sampler.channels() == 0);
    REQUIRE(sampler.capacity() == 0);

    sampler.prepare(1, 16'777'217, 0);
    REQUIRE(sampler.channels() == 0);
    REQUIRE(sampler.capacity() == 0);
}

TEST_CASE("FreezeLoopSampler writes reject invalid audio buffers",
          "[signal][freeze-loop][buffers]") {
    FreezeLoopSampler sampler;
    sampler.prepare(1, 64, 0);
    sampler.write(nullptr, 1);
    REQUIRE(sampler.available_history() == 0);

    const float* missing_channel[]{nullptr};
    sampler.write(missing_channel, 1);
    REQUIRE(sampler.available_history() == 0);

    sampler.prepare(-1, 64, 0);
    sampler.write(nullptr, 1);
    REQUIRE(sampler.available_history() == 0);
}

TEST_CASE("FreezeLoopSampler sanitizes non-finite history samples",
          "[signal][freeze-loop][finite]") {
    FreezeLoopSampler sampler;
    sampler.prepare(1, 8, 0);
    const std::array<float, 3> input{1.0f, std::numeric_limits<float>::infinity(), 3.0f};
    const float* inputs[]{input.data()};
    sampler.write(inputs, static_cast<int>(input.size()));
    REQUIRE(sampler.try_freeze_recent(3) == FreezeLoopSampler::CaptureResult::Success);
    REQUIRE(sampler.loop_sample(0, 0) == 1.0f);
    REQUIRE(sampler.loop_sample(0, 1) == 0.0f);
    REQUIRE(sampler.loop_sample(0, 2) == 3.0f);
}

TEST_CASE("FreezeLoopSampler reads reject invalid audio buffers and frame counts",
          "[signal][freeze-loop][buffers]") {
    FreezeLoopSampler sampler;
    sampler.prepare(1, 64, 0);
    const std::array<float, 4> input{1, 2, 3, 4};
    const float* inputs[]{input.data()};
    sampler.write(inputs, static_cast<int>(input.size()));
    REQUIRE(sampler.try_freeze_recent(4) == FreezeLoopSampler::CaptureResult::Success);
    const auto before = sampler.snapshot();

    sampler.read(nullptr, 1);
    float* missing_channel[]{nullptr};
    sampler.read(missing_channel, 1);
    float output = 0.0f;
    float* outputs[]{&output};
    sampler.read(outputs, -1);
    REQUIRE(sampler.snapshot() == before);
}
