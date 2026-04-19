// Tests for the short-read zero-fill helper used by the Oboe Android
// input-frame-read path (#244). The Android Oboe callback can get a
// partial read while the input stream warms up; the helper zeroes the
// tail so downstream code sees a deterministic, full-sized buffer.

#include <pulp/audio/frame_fill.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using pulp::audio::zero_fill_short_read;

namespace {

std::vector<float> make_poisoned(int total_frames, int channels) {
    std::vector<float> buf(static_cast<std::size_t>(total_frames) *
                           static_cast<std::size_t>(channels),
                           0.5f);  // non-zero sentinel
    return buf;
}

} // namespace

TEST_CASE("zero_fill_short_read: full read is a no-op",
          "[audio][frame-fill][issue-244]") {
    auto buf = make_poisoned(8, 2);
    zero_fill_short_read(buf.data(), /*frames_read=*/8, /*total=*/8, /*ch=*/2);
    for (float v : buf) REQUIRE(v == 0.5f);
}

TEST_CASE("zero_fill_short_read: partial read zeros the tail, keeps head",
          "[audio][frame-fill][issue-244]") {
    auto buf = make_poisoned(8, 2);
    // Simulate frames 0..3 populated (4 frames × 2 ch = 8 samples), tail zeros.
    zero_fill_short_read(buf.data(), /*frames_read=*/4, /*total=*/8, /*ch=*/2);
    for (int i = 0; i < 8; ++i) REQUIRE(buf[i] == 0.5f);
    for (int i = 8; i < 16; ++i) REQUIRE(buf[i] == 0.0f);
}

TEST_CASE("zero_fill_short_read: frames_read == 0 zeros the whole buffer",
          "[audio][frame-fill][issue-244]") {
    auto buf = make_poisoned(4, 2);
    zero_fill_short_read(buf.data(), /*frames_read=*/0, /*total=*/4, /*ch=*/2);
    for (float v : buf) REQUIRE(v == 0.0f);
}

TEST_CASE("zero_fill_short_read: handles mono (single channel)",
          "[audio][frame-fill][issue-244]") {
    auto buf = make_poisoned(8, 1);
    zero_fill_short_read(buf.data(), /*frames_read=*/3, /*total=*/8, /*ch=*/1);
    for (int i = 0; i < 3; ++i) REQUIRE(buf[i] == 0.5f);
    for (int i = 3; i < 8; ++i) REQUIRE(buf[i] == 0.0f);
}

TEST_CASE("zero_fill_short_read: negative frames_read clamped to 0",
          "[audio][frame-fill][issue-244]") {
    auto buf = make_poisoned(4, 2);
    zero_fill_short_read(buf.data(), /*frames_read=*/-1, /*total=*/4, /*ch=*/2);
    for (float v : buf) REQUIRE(v == 0.0f);
}

TEST_CASE("zero_fill_short_read: over-reported frames_read clamped to total",
          "[audio][frame-fill][issue-244]") {
    auto buf = make_poisoned(4, 2);
    zero_fill_short_read(buf.data(), /*frames_read=*/99, /*total=*/4, /*ch=*/2);
    // No zeroing — frames_read >= total is treated as a full read.
    for (float v : buf) REQUIRE(v == 0.5f);
}

TEST_CASE("zero_fill_short_read: null buffer is a no-op",
          "[audio][frame-fill][issue-244]") {
    // Would crash if not guarded.
    zero_fill_short_read(nullptr, 1, 8, 2);
    SUCCEED("no crash");
}

TEST_CASE("zero_fill_short_read: zero channels is a no-op",
          "[audio][frame-fill][issue-244]") {
    auto buf = make_poisoned(4, 1);
    zero_fill_short_read(buf.data(), 2, 4, /*ch=*/0);
    for (float v : buf) REQUIRE(v == 0.5f);
}

TEST_CASE("zero_fill_short_read: zero total_frames is a no-op",
          "[audio][frame-fill][issue-244]") {
    float buf[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    zero_fill_short_read(buf, 0, /*total=*/0, 2);
    for (float v : buf) REQUIRE(v == 0.5f);
}
