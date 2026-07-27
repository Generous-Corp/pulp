#include <pulp/timeline/production_mode.hpp>

namespace pulp::timeline {

std::string_view production_mode_name(ProductionMode mode) noexcept {
    switch (mode) {
    case ProductionMode::Synchronous:
        return "synchronous";
    case ProductionMode::Buffered:
        return "buffered";
    }
    return "synchronous";
}

std::string_view reproducibility_class_name(ReproducibilityClass reproducibility) noexcept {
    switch (reproducibility) {
    case ReproducibilityClass::Deterministic:
        return "deterministic";
    case ReproducibilityClass::Tolerance:
        return "tolerance";
    case ReproducibilityClass::Materialized:
        return "materialized";
    case ReproducibilityClass::BestEffort:
        return "best_effort";
    }
    return "best_effort";
}

std::optional<ProductionMode> production_mode_from_name(std::string_view name) noexcept {
    if (name == "synchronous")
        return ProductionMode::Synchronous;
    if (name == "buffered")
        return ProductionMode::Buffered;
    return std::nullopt;
}

std::optional<ReproducibilityClass>
reproducibility_class_from_name(std::string_view name) noexcept {
    if (name == "deterministic")
        return ReproducibilityClass::Deterministic;
    if (name == "tolerance")
        return ReproducibilityClass::Tolerance;
    if (name == "materialized")
        return ReproducibilityClass::Materialized;
    if (name == "best_effort")
        return ReproducibilityClass::BestEffort;
    return std::nullopt;
}

std::uint8_t replay_strength(ReproducibilityClass reproducibility) noexcept {
    switch (reproducibility) {
    case ReproducibilityClass::BestEffort:
        return 0;
    case ReproducibilityClass::Tolerance:
        return 1;
    case ReproducibilityClass::Materialized:
        return 2;
    case ReproducibilityClass::Deterministic:
        return 3;
    }
    return 0;
}

ReproducibilityClass weakest(ReproducibilityClass left, ReproducibilityClass right) noexcept {
    return replay_strength(right) < replay_strength(left) ? right : left;
}

bool is_bit_reproducible(ReproducibilityClass reproducibility) noexcept {
    return reproducibility == ReproducibilityClass::Deterministic ||
           reproducibility == ReproducibilityClass::Materialized;
}

ProductionDeclarationErrorCode
validate_production_declaration(const ProductionDeclaration& declaration) noexcept {
    if (declaration.lookahead_ms > kMaxProductionLookaheadMs)
        return ProductionDeclarationErrorCode::LookaheadExceedsLimit;
    switch (declaration.mode) {
    case ProductionMode::Synchronous:
        if (declaration.lookahead_ms != 0)
            return ProductionDeclarationErrorCode::SynchronousDeclaresLookahead;
        break;
    case ProductionMode::Buffered:
        if (declaration.lookahead_ms == 0)
            return ProductionDeclarationErrorCode::BufferedDeclaresNoLookahead;
        break;
    }
    return ProductionDeclarationErrorCode::None;
}

} // namespace pulp::timeline
