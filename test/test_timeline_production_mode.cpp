#include <catch2/catch_test_macros.hpp>

#include <pulp/timeline/production_mode.hpp>

using namespace pulp::timeline;

TEST_CASE("production mode and reproducibility names round-trip", "[timeline][production]") {
    REQUIRE(production_mode_from_name(production_mode_name(ProductionMode::Synchronous)) ==
            ProductionMode::Synchronous);
    REQUIRE(production_mode_from_name(production_mode_name(ProductionMode::Buffered)) ==
            ProductionMode::Buffered);

    const ReproducibilityClass classes[] = {
        ReproducibilityClass::Deterministic, ReproducibilityClass::Tolerance,
        ReproducibilityClass::Materialized, ReproducibilityClass::BestEffort};
    for (const auto value : classes)
        REQUIRE(reproducibility_class_from_name(reproducibility_class_name(value)) == value);
}

TEST_CASE("unknown production names are rejected rather than defaulted",
          "[timeline][production]") {
    REQUIRE_FALSE(production_mode_from_name("").has_value());
    REQUIRE_FALSE(production_mode_from_name("Synchronous").has_value());
    REQUIRE_FALSE(production_mode_from_name("streamed").has_value());
    REQUIRE_FALSE(reproducibility_class_from_name("").has_value());
    REQUIRE_FALSE(reproducibility_class_from_name("besteffort").has_value());
    REQUIRE_FALSE(reproducibility_class_from_name("exact").has_value());
}

TEST_CASE("replay strength orders the reproducibility claims", "[timeline][production]") {
    REQUIRE(replay_strength(ReproducibilityClass::BestEffort) <
            replay_strength(ReproducibilityClass::Tolerance));
    REQUIRE(replay_strength(ReproducibilityClass::Tolerance) <
            replay_strength(ReproducibilityClass::Materialized));
    REQUIRE(replay_strength(ReproducibilityClass::Materialized) <
            replay_strength(ReproducibilityClass::Deterministic));

    REQUIRE(is_bit_reproducible(ReproducibilityClass::Deterministic));
    REQUIRE(is_bit_reproducible(ReproducibilityClass::Materialized));
    REQUIRE_FALSE(is_bit_reproducible(ReproducibilityClass::Tolerance));
    REQUIRE_FALSE(is_bit_reproducible(ReproducibilityClass::BestEffort));
}

TEST_CASE("aggregating reproducibility keeps the weaker claim", "[timeline][production]") {
    REQUIRE(weakest(ReproducibilityClass::Deterministic, ReproducibilityClass::BestEffort) ==
            ReproducibilityClass::BestEffort);
    REQUIRE(weakest(ReproducibilityClass::BestEffort, ReproducibilityClass::Deterministic) ==
            ReproducibilityClass::BestEffort);
    REQUIRE(weakest(ReproducibilityClass::Materialized, ReproducibilityClass::Tolerance) ==
            ReproducibilityClass::Tolerance);
    REQUIRE(weakest(ReproducibilityClass::Deterministic, ReproducibilityClass::Materialized) ==
            ReproducibilityClass::Materialized);
    REQUIRE(weakest(ReproducibilityClass::Tolerance, ReproducibilityClass::Tolerance) ==
            ReproducibilityClass::Tolerance);
}

TEST_CASE("a production declaration must be coherent about its lookahead",
          "[timeline][production]") {
    ProductionDeclaration synchronous;
    REQUIRE(validate_production_declaration(synchronous) ==
            ProductionDeclarationErrorCode::None);

    synchronous.lookahead_ms = 10;
    REQUIRE(validate_production_declaration(synchronous) ==
            ProductionDeclarationErrorCode::SynchronousDeclaresLookahead);

    ProductionDeclaration buffered;
    buffered.mode = ProductionMode::Buffered;
    buffered.reproducibility = ReproducibilityClass::BestEffort;
    REQUIRE(validate_production_declaration(buffered) ==
            ProductionDeclarationErrorCode::BufferedDeclaresNoLookahead);

    buffered.lookahead_ms = 250;
    REQUIRE(validate_production_declaration(buffered) == ProductionDeclarationErrorCode::None);

    buffered.lookahead_ms = kMaxProductionLookaheadMs;
    REQUIRE(validate_production_declaration(buffered) == ProductionDeclarationErrorCode::None);

    buffered.lookahead_ms = kMaxProductionLookaheadMs + 1;
    REQUIRE(validate_production_declaration(buffered) ==
            ProductionDeclarationErrorCode::LookaheadExceedsLimit);
}
