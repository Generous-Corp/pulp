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

TEST_CASE("serialize stores little-endian width and height bytes",
          "[ui][resizable-shell][coverage][phase3]") {
    ResizableShell s({.initial_size = {0x0304u, 0x020Du}});

    const auto blob = s.serialize();

    REQUIRE(blob.size() == 8);
    REQUIRE(blob[0] == 0x04);
    REQUIRE(blob[1] == 0x03);
    REQUIRE(blob[2] == 0x00);
    REQUIRE(blob[3] == 0x00);
    REQUIRE(blob[4] == 0x0D);
    REQUIRE(blob[5] == 0x02);
    REQUIRE(blob[6] == 0x00);
    REQUIRE(blob[7] == 0x00);
}

TEST_CASE("deserialize ignores trailing bytes after the size payload",
          "[ui][resizable-shell][coverage][phase3]") {
    uint8_t blob[] = {
        0x2Cu, 0x01u, 0x00u, 0x00u, // 300
        0xC8u, 0x00u, 0x00u, 0x00u, // 200
        0xAAu, 0xBBu,
    };

    ResizableShell s({.initial_size = {640, 480}});
    REQUIRE(s.deserialize({blob, sizeof(blob)}));
    REQUIRE(s.current().width == 300);
    REQUIRE(s.current().height == 200);
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

TEST_CASE("non-positive aspect ratio uses plain min max clamping",
          "[ui][resizable-shell][coverage][phase3]") {
    ResizableShell zero({.min_size = {100, 80},
                         .max_size = {500, 400},
                         .initial_size = {300, 300},
                         .aspect_ratio = 0.0});
    auto z = zero.apply({900, 10});
    REQUIRE(z.width == 500);
    REQUIRE(z.height == 80);

    ResizableShell negative({.min_size = {120, 90},
                             .max_size = {600, 450},
                             .initial_size = {300, 300},
                             .aspect_ratio = -2.0});
    auto n = negative.negotiate({10, 900});
    REQUIRE(n.width == 120);
    REQUIRE(n.height == 450);
}
