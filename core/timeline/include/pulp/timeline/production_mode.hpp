#pragma once

/// @file production_mode.hpp
/// How content is produced, and how honestly a render of it can be replayed.
///
/// These are declarations of intent, not scheduling policy. A production mode
/// says whether content is produced in band with the audio callback or ahead of
/// it; a reproducibility class says what a second render of the same document is
/// entitled to claim about the first. Both travel with content so a replay,
/// bounce, or diff can report which parts of a render are bit-reproducible and
/// which are not, instead of asserting bit-equality over material that never
/// promised it.
///
/// The declared lookahead is wall-clock milliseconds and is never latency.
/// Production cost is wall-clock (decode time, a GPU round trip), so a lookahead
/// expressed in musical ticks would silently shrink as tempo rises. A producer
/// that needs *musical* context ("the next bar's chords") is declaring a compile
/// context subscription, not a lookahead. Nothing here may reach a latency or
/// delay-compensation computation: a lookahead is time a producer is given to
/// work, not time a signal is delayed by.

#include <cstdint>
#include <optional>
#include <string_view>

namespace pulp::timeline {

/// Where content is produced relative to the audio callback that consumes it.
enum class ProductionMode : std::uint8_t {
    /// Produced in band, inside the callback that consumes it. Cannot starve.
    Synchronous,
    /// Produced ahead of the playhead on another thread and consumed through a
    /// ring. Bounded wall-clock production cost, and it can starve.
    Buffered,
};

/// What a second render of the same document may claim about the first.
///
/// Ordered by replay strength in `replay_strength()`, weakest first, so a render
/// spanning several classes can report the weakest claim it is entitled to make.
enum class ReproducibilityClass : std::uint8_t {
    /// Bit-exact from the document and its declared inputs alone.
    Deterministic,
    /// Reproducible only within a declared numeric tolerance — a different
    /// device, driver, or vectorization may move the low bits.
    Tolerance,
    /// Bit-exact against a pinned, content-hashed artifact rather than against
    /// the producer. Regenerating mints a new artifact identity.
    Materialized,
    /// No replay guarantee. Live input and unseeded generation live here; a
    /// bit-exact gate must exclude the audio while still asserting the inputs.
    BestEffort,
};

/// Canonical wire name, stable across builds and safe to persist.
std::string_view production_mode_name(ProductionMode mode) noexcept;
std::string_view reproducibility_class_name(ReproducibilityClass reproducibility) noexcept;

/// Parse a canonical wire name. Fails closed: an unknown name is rejected rather
/// than defaulted, so a document from a newer build never silently reads as the
/// strongest claim.
std::optional<ProductionMode> production_mode_from_name(std::string_view name) noexcept;
std::optional<ReproducibilityClass>
reproducibility_class_from_name(std::string_view name) noexcept;

/// Replay strength, ascending: `BestEffort` < `Tolerance` < `Materialized` <
/// `Deterministic`. Only an ordering, not a quality score.
std::uint8_t replay_strength(ReproducibilityClass reproducibility) noexcept;

/// The weaker of two claims — the only correct way to aggregate a render that
/// spans several classes.
ReproducibilityClass weakest(ReproducibilityClass left, ReproducibilityClass right) noexcept;

/// True when a second render is entitled to claim bit-equality with the first.
bool is_bit_reproducible(ReproducibilityClass reproducibility) noexcept;

/// Upper bound on a declared lookahead. A declaration past this is a document
/// error, not a very patient producer.
inline constexpr std::uint64_t kMaxProductionLookaheadMs = 60'000;

/// What a piece of content declares about its own production.
struct ProductionDeclaration {
    ProductionMode mode = ProductionMode::Synchronous;
    ReproducibilityClass reproducibility = ReproducibilityClass::Deterministic;
    /// Wall-clock milliseconds a producer is given to work ahead of the
    /// playhead. Zero for synchronous content. Never a latency figure.
    std::uint64_t lookahead_ms = 0;
};

enum class ProductionDeclarationErrorCode : std::uint8_t {
    None,
    /// Synchronous content produces in band, so a lookahead is meaningless.
    SynchronousDeclaresLookahead,
    /// A buffered producer with no lookahead is a synchronous producer that can
    /// starve — the category error worth rejecting at the declaration.
    BufferedDeclaresNoLookahead,
    LookaheadExceedsLimit,
};

/// Validate a declaration. Returns `None` when the declaration is coherent.
ProductionDeclarationErrorCode
validate_production_declaration(const ProductionDeclaration& declaration) noexcept;

} // namespace pulp::timeline
