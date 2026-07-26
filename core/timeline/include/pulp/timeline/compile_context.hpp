#pragma once

#include <pulp/timeline/model.hpp>

#include <cstddef>
#include <cstdint>

namespace pulp::timeline {

/// A kind of timeline context a renderer's compile hook may read *beyond its
/// own clip's content*.
///
/// This enum is the vocabulary of the compile-context subscription contract.
/// The contract exists because the compiler's dirty set is exact, not diffed:
/// a committed transaction says precisely which items changed, and the compiler
/// recompiles only their owners. A renderer that reads a sequence-owned lane
/// while compiling breaks that reasoning — its clip did not change, so nothing
/// would recompile it, and it would render stale harmony forever.
///
/// The contract closes the hole from both ends:
///   * a renderer declares the context kinds it reads at registration, and the
///     compiler keeps a reverse index from kind to declared readers, so a lane
///     edit dirties exactly them and nothing else;
///   * the read side is a CompileContextView carrying the same declaration, so
///     an undeclared kind reads as absent. A hook cannot silently depend on
///     context the compiler does not know to invalidate it for.
///
/// Adding a kind is a data change here plus a reverse-index case in the
/// compiler; it is never a reason for a consumer to widen an invalidation.
enum class CompileContextKind : std::uint8_t {
    ChordScale,
    Groove,
};

inline constexpr std::size_t kCompileContextKindCount = 2;

/// The declared set of context kinds one content renderer reads.
///
/// Default-constructed means "reads nothing beyond its own clip", which is what
/// every built-in content kind declares today — so the contract cannot change
/// the invalidation of anything that existed before it.
class CompileContextSubscriptions {
  public:
    constexpr CompileContextSubscriptions() noexcept = default;

    static constexpr CompileContextSubscriptions none() noexcept {
        return {};
    }

    constexpr CompileContextSubscriptions& subscribe(CompileContextKind kind) noexcept {
        bits_ = static_cast<std::uint8_t>(bits_ | mask(kind));
        return *this;
    }

    constexpr bool reads(CompileContextKind kind) const noexcept {
        return (bits_ & mask(kind)) != 0;
    }

    constexpr bool any() const noexcept {
        return bits_ != 0;
    }

    constexpr bool operator==(const CompileContextSubscriptions&) const noexcept = default;

  private:
    static constexpr std::uint8_t mask(CompileContextKind kind) noexcept {
        return static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(kind));
    }

    std::uint8_t bits_ = 0;
};

static_assert(kCompileContextKindCount <= 8,
              "CompileContextSubscriptions stores one bit per kind in a std::uint8_t. Widen the "
              "storage before adding a ninth context kind.");

/// The read side of the contract: a read-only window onto one pinned snapshot's
/// context, narrowed to what its owner declared.
///
/// The view borrows the snapshot; the caller pins it (the compiler holds the
/// same shared_ptr<const Project> the request carried) for the view's lifetime.
/// Every accessor returns null for a kind the owner did not declare, so the
/// declaration is load-bearing at the point of the read and not merely
/// paperwork filed at registration.
class CompileContextView {
  public:
    CompileContextView(const Project& project, ItemId sequence_id,
                       CompileContextSubscriptions subscriptions) noexcept;

    CompileContextSubscriptions subscriptions() const noexcept {
        return subscriptions_;
    }

    /// The sequence's chord/scale lane, or null when ChordScale was not
    /// declared or the sequence is not in this snapshot.
    const ChordScaleLane* chord_scale_lane() const noexcept;

    /// The harmony in force at `position`, or null when ChordScale was not
    /// declared, the lane is empty, or `position` precedes its first event.
    const ChordScaleEvent* chord_scale_at(timebase::TickPosition position) const noexcept;

    /// The sequence's groove, or null when Groove was not declared or the
    /// sequence is not in this snapshot. A sequence that plays straight still
    /// returns a groove — one that states no feel — because "plays straight" is
    /// an answer, not an absence.
    const GrooveTemplate* groove() const noexcept;

  private:
    const Sequence* sequence_ = nullptr;
    CompileContextSubscriptions subscriptions_;
};

} // namespace pulp::timeline
