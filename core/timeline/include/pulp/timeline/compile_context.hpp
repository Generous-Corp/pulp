#pragma once

#include <pulp/timeline/model.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace pulp::timeline {

/** @addtogroup timeline_compile
 * @{
 */

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
/// The reserved kinds below carry a constraint the first two do not. ChordScale
/// and Groove are read from a sequence the reader already sits under, so the
/// compile order is a tree walk. Dynamics, CrossTrackRhythm, and EnsembleSkeleton
/// are peer-edge kinds: a reader can subscribe to context produced by a sibling
/// track rather than an ancestor. That makes the compile order a DAG, and the
/// DAG must stay acyclic — two tracks that each read the other's context have no
/// valid order and would either deadlock a compile or silently resolve against
/// a stale peer. Whoever gives one of these kinds a producer owns proving the
/// acyclicity, because the bitset here cannot express it: subscription records
/// *what* a reader reads, never *when* it may be compiled.
enum class CompileContextKind : std::uint8_t {
    ChordScale,
    Groove,
    /// Reserved. Slot only: no producer, no consumer, no semantics (P0-15).
    Dynamics,
    /// Reserved, peer-edge. See the acyclicity note above.
    CrossTrackRhythm,
    /// Reserved, peer-edge. See the acyclicity note above.
    EnsembleSkeleton,
};

/// Number of CompileContextKind values represented by the subscription bitset.
inline constexpr std::size_t kCompileContextKindCount = 5;

/// The declared set of context kinds one content renderer reads.
///
/// Default-constructed means "reads nothing beyond its own clip", which is what
/// every built-in content kind declares today — so the contract cannot change
/// the invalidation of anything that existed before it.
class CompileContextSubscriptions {
  public:
    /// Constructs an empty subscription set.
    constexpr CompileContextSubscriptions() noexcept = default;

    /// Returns an empty subscription set.
    static constexpr CompileContextSubscriptions none() noexcept {
        return {};
    }

    /// Adds `kind` to this set and returns this object for chaining.
    constexpr CompileContextSubscriptions& subscribe(CompileContextKind kind) noexcept {
        bits_ = static_cast<Bits>(bits_ | mask(kind));
        return *this;
    }

    /// Returns whether `kind` is present in this set.
    constexpr bool reads(CompileContextKind kind) const noexcept {
        return (bits_ & mask(kind)) != 0;
    }

    /// Returns whether this set contains any context kind.
    constexpr bool any() const noexcept {
        return bits_ != 0;
    }

    /// Compares the complete subscribed-kind set.
    constexpr bool operator==(const CompileContextSubscriptions&) const noexcept = default;

  private:
    /// One bit per kind. Widened to 16 bits when the vocabulary grew past its
    /// original two, rather than at the eighth kind: the widening is invisible
    /// to every caller (the set is in-memory only and never serialized), so
    /// doing it early costs a byte per subscription and removes the chance that
    /// whoever adds a ninth kind discovers the ceiling by tripping it.
    using Bits = std::uint16_t;

    static constexpr Bits mask(CompileContextKind kind) noexcept {
        return static_cast<Bits>(Bits{1} << static_cast<unsigned>(kind));
    }

    Bits bits_ = 0;

    static_assert(kCompileContextKindCount <= std::numeric_limits<Bits>::digits,
                  "CompileContextSubscriptions stores one bit per kind. Widen Bits before adding "
                  "a kind past the storage width.");
};

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
    /// Creates a borrowed view of `sequence_id` narrowed to `subscriptions`.
    ///
    /// `project` must outlive this view. A missing sequence produces a view whose
    /// context accessors all return `nullptr`.
    CompileContextView(const Project& project, ItemId sequence_id,
                       CompileContextSubscriptions subscriptions) noexcept;

    /// Returns the context kinds this view permits callers to read.
    CompileContextSubscriptions subscriptions() const noexcept {
        return subscriptions_;
    }

    /// The sequence's chord/scale lane, or null when ChordScale was not
    /// declared or the sequence is not in this snapshot.
    const ChordScaleLane* chord_scale_lane() const noexcept;

    /// The harmony in force at `position`, or null when ChordScale was not
    /// declared, the lane is empty, or `position` precedes its first event.
    const ChordScaleEvent* chord_scale_at(timebase::TickPosition position) const noexcept;

    /// The sequence's intensity lane, or null when Dynamics was not declared or
    /// the sequence is not in this snapshot.
    const DynamicsLane* dynamics_lane() const noexcept;

    /// The interpolated intensity at `position`, or empty when Dynamics was not
    /// declared, the lane is empty, or `position` precedes its first event.
    ///
    /// Empty is not zero: a reader that has not been told an intensity must
    /// decide for itself, and handing back 0.0 would look like an authored
    /// silence the document never states.
    std::optional<float> dynamics_at(timebase::TickPosition position) const noexcept;

    /// The sequence's groove, or null when Groove was not declared or the
    /// sequence is not in this snapshot. A sequence that plays straight still
    /// returns a groove — one that states no feel — because "plays straight" is
    /// an answer, not an absence.
    const GrooveTemplate* groove() const noexcept;

  private:
    const Sequence* sequence_ = nullptr;
    CompileContextSubscriptions subscriptions_;
};

/// @}

} // namespace pulp::timeline
