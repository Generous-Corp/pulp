// test_gpu_adapter_probe.cpp -- verdict rules of the Vellum-backed GPU adapter
// probe. The rules need no GPU: evidence and frames are built by hand.

#include <catch2/catch_test_macros.hpp>

#include <pulp_tooling/gpu_probe/adapter_probe.hpp>

#include <string>

using namespace pulp::tooling::gpu_adapter;

namespace {

Evidence healthy() {
    Evidence e;
    e.available = true;
    e.native_surface = false;
    e.fallback = false;
    e.renderer = "Skia Graphite";
    e.backend = "Metal";
    e.adapter = "Apple M5";
    e.texture_format = "RGBA8Unorm";
    return e;
}

Frame correct_frame() {
    Frame f;
    f.width = kSceneWidth;
    f.height = kSceneHeight;
    f.rgba.assign(std::size_t{kSceneWidth} * kSceneHeight * 4, 0);
    for (const auto& s : kSwatches) {
        const auto at = (std::size_t{s.y} * f.width + s.x) * 4;
        f.rgba[at] = s.r;
        f.rgba[at + 1] = s.g;
        f.rgba[at + 2] = s.b;
        f.rgba[at + 3] = 255;
    }
    return f;
}

} // namespace

TEST_CASE("a named hardware adapter and the right pixels pass", "[gpu-probe][adapter]") {
    const auto frame = correct_frame();
    const auto result = classify(healthy(), &frame);
    CHECK(result.verdict == Verdict::pass);
    CHECK(result.reason == "ok");
    CHECK(result.swatches_checked == kSwatches.size());
    CHECK(result.swatches_mismatched == 0);
    CHECK(exit_code(result.verdict) == 0);
}

TEST_CASE("an empty adapter name fails even when the pixels are right", "[gpu-probe][adapter]") {
    const auto frame = correct_frame();
    auto evidence = healthy();
    evidence.adapter.clear();
    const auto result = classify(evidence, &frame);
    CHECK(result.verdict == Verdict::fail);
    CHECK(result.reason == "empty-adapter");
    CHECK(exit_code(result.verdict) == 1);
    CHECK(to_json(result).find("\"reason\":\"empty-adapter\"") != std::string::npos);
}

TEST_CASE("a device that never came up is unavailable, not failed", "[gpu-probe][adapter]") {
    Evidence evidence; // available == false
    const auto result = classify(evidence, nullptr);
    CHECK(result.verdict == Verdict::unavailable);
    CHECK(exit_code(result.verdict) == 2);
}

TEST_CASE("a software fallback and a missing renderer fail", "[gpu-probe][adapter]") {
    const auto frame = correct_frame();
    auto fallback = healthy();
    fallback.fallback = true;
    CHECK(classify(fallback, &frame).reason == "software-fallback");
    auto unnamed = healthy();
    unnamed.backend.clear();
    CHECK(classify(unnamed, &frame).reason == "empty-renderer");
}

TEST_CASE("a wrong swatch or a missing frame fails", "[gpu-probe][adapter]") {
    auto frame = correct_frame();
    const auto& s = kSwatches[1];
    frame.rgba[(std::size_t{s.y} * frame.width + s.x) * 4] ^= 0x40;
    const auto wrong = classify(healthy(), &frame);
    CHECK(wrong.verdict == Verdict::fail);
    CHECK(wrong.reason == "oracle-mismatch");
    CHECK(wrong.swatches_mismatched == 1);

    CHECK(classify(healthy(), nullptr).reason == "no-frame");
    Frame small;
    small.width = 4;
    small.height = 4;
    small.rgba.assign(64, 0);
    CHECK(classify(healthy(), &small).reason == "no-frame");
}

TEST_CASE("a one-step rounding difference stays within tolerance", "[gpu-probe][adapter]") {
    auto frame = correct_frame();
    const auto& s = kSwatches[2];
    frame.rgba[(std::size_t{s.y} * frame.width + s.x) * 4 + 1] += kChannelTolerance;
    CHECK(classify(healthy(), &frame).verdict == Verdict::pass);
    frame.rgba[(std::size_t{s.y} * frame.width + s.x) * 4 + 1] += 1;
    CHECK(classify(healthy(), &frame).verdict == Verdict::fail);
}
