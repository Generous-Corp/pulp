// PendingResize: the host's resize notification runs at most once per frame,
// always with the latest size.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/pending_resize.hpp>

#include <cstdint>
#include <utility>
#include <vector>

using pulp::view::PendingResize;

namespace {

using Calls = std::vector<std::pair<uint32_t, uint32_t>>;

void frame(PendingResize& pending, Calls& calls) {
    pending.deliver([&](uint32_t w, uint32_t h) { calls.emplace_back(w, h); });
}

} // namespace

TEST_CASE("Pending resize flood delivers once with the last size", "[pending-resize]") {
    PendingResize pending;
    Calls calls;
    for (uint32_t i = 1; i <= 60; ++i)
        pending.record(400 + i, 300 + i);
    frame(pending, calls);
    REQUIRE(calls == Calls{{460, 360}});
    frame(pending, calls);
    REQUIRE(calls.size() == 1);
}

TEST_CASE("Pending resize across frames delivers each frame's last size", "[pending-resize]") {
    PendingResize pending;
    Calls calls;
    // 60 events inside 4 frames: 15 per frame.
    for (uint32_t f = 0; f < 4; ++f) {
        for (uint32_t i = 1; i <= 15; ++i)
            pending.record(400 + f * 15 + i, 300);
        frame(pending, calls);
    }
    REQUIRE(calls == Calls{{415, 300}, {430, 300}, {445, 300}, {460, 300}});
}

TEST_CASE("Pending resize with nothing recorded delivers nothing", "[pending-resize]") {
    PendingResize pending;
    Calls calls;
    REQUIRE_FALSE(pending.pending());
    frame(pending, calls);
    REQUIRE(calls.empty());
}

TEST_CASE("Pending resize recorded from inside the callback waits for the next frame",
          "[pending-resize]") {
    PendingResize pending;
    Calls calls;
    pending.record(401, 300);
    // An aspect-snapping editor answers the size it was told with another one.
    pending.deliver([&](uint32_t w, uint32_t h) {
        calls.emplace_back(w, h);
        pending.record(400, 300);
    });
    REQUIRE(calls == Calls{{401, 300}});
    REQUIRE(pending.pending());
    frame(pending, calls);
    REQUIRE(calls == Calls{{401, 300}, {400, 300}});
}
