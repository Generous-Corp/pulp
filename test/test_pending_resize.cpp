// PendingResize: the host's resize notification runs at most once per frame,
// always with the latest size.

#include <catch2/catch_test_macros.hpp>

#include "../core/view/src/pending_resize.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
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

TEST_CASE("Pending resize recorded from inside the callback is delivered before the frame",
          "[pending-resize]") {
    PendingResize pending;
    Calls calls;
    pending.record(401, 300);
    // An aspect-snapping editor answers the size it was told with another one;
    // the frame must lay out at the answer, not the question.
    pending.deliver([&](uint32_t w, uint32_t h) {
        calls.emplace_back(w, h);
        if (calls.size() == 1)
            pending.record(400, 300);
    });
    REQUIRE(calls == Calls{{401, 300}, {400, 300}});
    REQUIRE_FALSE(pending.pending());
}

TEST_CASE("Pending resize delivery does not re-enter itself", "[pending-resize]") {
    PendingResize pending;
    Calls calls;
    int depth = 0, max_depth = 0;
    std::function<void(uint32_t, uint32_t)> callback = [&](uint32_t w, uint32_t h) {
        ++depth;
        max_depth = std::max(max_depth, depth);
        calls.emplace_back(w, h);
        if (calls.size() == 1) {
            // The resize the callback causes records and delivers from inside
            // it, the way an unattached Windows host handles its own WM_SIZE.
            pending.record(500, 400);
            REQUIRE(pending.delivering());
            pending.deliver(callback);
            REQUIRE(calls.size() == 1); // the nested call delivered nothing
        }
        --depth;
    };
    pending.record(401, 300);
    pending.deliver(callback);
    CHECK(max_depth == 1);
    CHECK(calls == Calls{{401, 300}, {500, 400}});
    CHECK_FALSE(pending.pending());
    CHECK_FALSE(pending.delivering());
}

TEST_CASE("Pending resize leaves a callback that always resizes to the next frame",
          "[pending-resize]") {
    PendingResize pending;
    Calls calls;
    uint32_t next = 400;
    auto resize_again = [&](uint32_t w, uint32_t h) {
        calls.emplace_back(w, h);
        pending.record(++next, 300);
    };
    pending.record(next, 300);
    pending.deliver(resize_again);
    REQUIRE(calls.size() == static_cast<size_t>(PendingResize::kMaxPasses));
    REQUIRE(pending.pending());
    pending.deliver(resize_again);
    REQUIRE(calls.size() == static_cast<size_t>(2 * PendingResize::kMaxPasses));
    CHECK(calls.back() == std::make_pair(next - 1, 300u));
}
