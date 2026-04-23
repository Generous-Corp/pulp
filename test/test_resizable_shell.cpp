// ResizableShell tests (workstream 07 slice 7.5).

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/resizable_shell.hpp>

using namespace pulp::view;

TEST_CASE("defaults to initial size", "[ui][resizable-shell]") {
    ResizableShell s;
    REQUIRE(s.current().width == 640);
    REQUIRE(s.current().height == 480);
}

TEST_CASE("min clamp", "[ui][resizable-shell]") {
    ResizableShell s({.min_size = {320, 240}, .initial_size = {400, 300}});
    auto got = s.apply({10, 10});
    REQUIRE(got.width == 320);
    REQUIRE(got.height == 240);
}

TEST_CASE("max clamp", "[ui][resizable-shell]") {
    ResizableShell s({.max_size = {800, 600}, .initial_size = {400, 300}});
    auto got = s.apply({9000, 9000});
    REQUIRE(got.width == 800);
    REQUIRE(got.height == 600);
}

TEST_CASE("aspect lock shrinks the overgrown dimension",
          "[ui][resizable-shell]") {
    // 16:9 aspect — target ratio ≈ 1.7778
    ResizableShell s({.min_size = {100, 50},
                      .max_size = {2000, 2000},
                      .initial_size = {640, 360},
                      .aspect_ratio = 16.0 / 9.0});

    // User drags width wider than 16:9 allows for the height.
    auto got = s.apply({2000, 500});
    REQUIRE(got.height == 500);
    // Expected width ≈ 500 * 16/9 = 888.88 → 889
    REQUIRE(got.width == 889);
}

TEST_CASE("aspect lock with shrunk height - width governs",
          "[ui][resizable-shell]") {
    ResizableShell s({.aspect_ratio = 2.0});
    // Tall narrow rectangle — height too large, must shrink.
    auto got = s.apply({400, 300});
    REQUIRE(got.width == 400);
    REQUIRE(got.height == 200);  // 400 / 2 = 200
}

TEST_CASE("aspect lock respects min bounds",
          "[ui][resizable-shell]") {
    ResizableShell s({.min_size = {200, 150},
                      .aspect_ratio = 2.0});
    // 200x200 would snap to height=100 (below min 150).
    // Shell must rebuild width from the clamped min height.
    auto got = s.apply({200, 200});
    REQUIRE(got.height >= 150);
    REQUIRE(got.width == static_cast<uint32_t>(got.height * 2));
}

TEST_CASE("serialize + deserialize round-trip", "[ui][resizable-shell]") {
    ResizableShell s({.initial_size = {1024, 768}});
    s.apply({1000, 700});
    auto blob = s.serialize();

    ResizableShell other({.initial_size = {640, 480}});
    REQUIRE(other.deserialize(blob));
    REQUIRE(other.current().width == 1000);
    REQUIRE(other.current().height == 700);
}

TEST_CASE("deserialize clamps against current config",
          "[ui][resizable-shell]") {
    ResizableShell wide({.initial_size = {4096, 4096}});
    wide.apply({3000, 3000});
    auto blob = wide.serialize();

    // Tighter config — the saved 3000x3000 must clamp down.
    ResizableShell tight({.max_size = {800, 600}, .initial_size = {400, 300}});
    REQUIRE(tight.deserialize(blob));
    REQUIRE(tight.current().width == 800);
    REQUIRE(tight.current().height == 600);
}

TEST_CASE("deserialize rejects too-short blob",
          "[ui][resizable-shell]") {
    ResizableShell s;
    uint8_t b[] = {1, 2, 3};
    REQUIRE_FALSE(s.deserialize({b, sizeof(b)}));
}

TEST_CASE("aspect min-correction re-applies max clamp",
          "[ui][resizable-shell]") {
    // #206 review case: tight bounds with aspect lock, over-requested
    // dimensions. Min-correction would otherwise push past max.
    ResizableShell s({.min_size = {200, 150},
                      .max_size = {210, 160},
                      .initial_size = {205, 155},
                      .aspect_ratio = 2.0});
    auto got = s.negotiate({210, 160});
    REQUIRE(got.width  <= 210);
    REQUIRE(got.height <= 160);
}

TEST_CASE("constructor normalizes initial size through negotiate",
          "[ui][resizable-shell]") {
    // Out-of-range initial — current() must report the clamped size
    // immediately, not the bogus initial.
    ResizableShell s({.max_size = {300, 300},
                      .initial_size = {640, 480}});
    REQUIRE(s.current().width  <= 300);
    REQUIRE(s.current().height <= 300);
}
