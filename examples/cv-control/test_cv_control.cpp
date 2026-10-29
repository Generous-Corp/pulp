#include "cv_control.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <limits>
#include <thread>

using pulp::examples::cv_control::FaceFrame;
using pulp::examples::cv_control::FaceMapping;
using pulp::examples::cv_control::ControlReplay;
using pulp::examples::cv_control::ControlSample;

TEST_CASE("face mapping is coherent, smoothed, and confidence gated") {
    FaceMapping mapping(0.6f, 0.5f);
    mapping.submit({1, 1.0f, 0.9f, true});
    REQUIRE_THAT(mapping.value(), Catch::Matchers::WithinAbs(0.5f, 1e-6f));
    REQUIRE(mapping.valid());
    mapping.submit({2, 0.0f, 0.2f, true});
    REQUIRE_THAT(mapping.value(), Catch::Matchers::WithinAbs(0.25f, 1e-6f));
    REQUIRE_FALSE(mapping.valid());
}

TEST_CASE("tracking loss and stale replay expire deterministically") {
    FaceMapping mapping;
    mapping.submit({7, 0.8f, 1.0f, true});
    mapping.expire();
    REQUIRE(mapping.value() == 0.0f);
    REQUIRE_FALSE(mapping.valid());
}

TEST_CASE("malformed values fail closed") {
    FaceMapping mapping;
    mapping.submit({1, std::numeric_limits<float>::quiet_NaN(), 1.0f, true});
    REQUIRE(mapping.value() == 0.0f);
    mapping.submit({2, 3.0f, 1.0f, true});
    REQUIRE_THAT(mapping.value(), Catch::Matchers::WithinAbs(0.25f, 1e-6f));
}

TEST_CASE("provider restart replaces an expired frame") {
    FaceMapping mapping;
    mapping.submit({10, 0.9f, 1.0f, true});
    mapping.expire();
    mapping.submit({11, 0.4f, 1.0f, true});
    REQUIRE(mapping.valid());
    REQUIRE(mapping.sequence() == 11);
}

TEST_CASE("concurrent producer updates remain bounded and finite") {
    FaceMapping mapping;
    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= 1000; ++i)
            mapping.submit({i, static_cast<float>(i % 100) / 99.0f, 1.0f, true});
    });
    producer.join();
    REQUIRE(std::isfinite(mapping.value()));
    REQUIRE(mapping.value() >= 0.0f);
    REQUIRE(mapping.value() <= 1.0f);
}

TEST_CASE("motion or sensor samples use the same mapping") {
    FaceMapping mapping;
    mapping.submit({42, 0.75f, 1.0f, true}); // e.g. normalized wrist rotation
    REQUIRE(mapping.valid());
    REQUIRE(mapping.sequence() == 42);
}

TEST_CASE("camera-free replay feeds the same mapping") {
    const ControlSample frames[] = {{1, .2f, 1.0f, true}, {2, .8f, 1.0f, true}};
    ControlReplay replay(frames);
    FaceMapping mapping;
    REQUIRE(replay.next(mapping));
    REQUIRE(replay.next(mapping));
    REQUIRE_FALSE(replay.next(mapping));
    REQUIRE(replay.position() == 2);
    replay.reset();
    REQUIRE(replay.position() == 0);
}
