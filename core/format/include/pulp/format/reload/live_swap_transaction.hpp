#pragma once

/// @file live_swap_transaction.hpp
/// One reload session = one atomic unit across UX + DSP (live-swap plan item 1.8).
///
/// A content pack can carry BOTH a new scripted UI and new DSP. Applied
/// independently, a failure in one half leaves a mismatched plugin — a new UI
/// bound to old DSP, or vice versa. This coordinator applies the halves as a
/// single transaction: apply stages in order; if ANY stage fails (returns false
/// or throws), roll back the stages that already succeeded, in reverse order, so
/// the plugin returns wholesale to its last-good state. Either everything swaps
/// or nothing does.
///
/// **Stage ordering is the contract.** A stage's `apply` must be atomic — on
/// failure it leaves NO partial state (the DSP reload keeps the old processor;
/// the scripted-UI reload self-restores its own tree). Order stages
/// **easiest-to-roll-back first**, so the hardest-to-undo stage is LAST and is
/// therefore never rolled back (nothing runs after it). For UX+DSP that means
/// UX first (rebuild is cheap to re-apply from the previous code + state) and the
/// DSP slot swap last (un-swapping a live processor is expensive; putting it last
/// means a DSP failure only rolls back the already-applied UX, and a DSP success
/// ends the transaction with nothing left to undo).
///
/// Control-thread only — this orchestrates the existing atomic reload paths
/// (ReloadSession / ScriptedUiSession); it does no audio-thread work itself.

#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace pulp::format::reload {

/// One half of a live swap. `apply` performs the swap and returns true on
/// success (and MUST leave no partial state on false/throw). `rollback` undoes a
/// previously-successful `apply` — called only for stages that applied OK when a
/// LATER stage fails. `name` identifies the stage in the result.
struct SwapStage {
    std::string name;
    std::function<bool()> apply;
    std::function<void()> rollback;   // may be null if this stage is always last
};

struct LiveSwapResult {
    bool ok = false;
    std::string failed_stage;   ///< which stage rejected/threw (empty when ok)
    std::string detail;         ///< exception text or "stage rejected"
    bool rollback_clean = true; ///< false if a rollback() itself threw (best-effort continued)
};

/// Apply @p stages as one transaction. On the first stage that returns false or
/// throws, the already-applied stages are rolled back in reverse and the failing
/// stage's name/detail is reported. Returns ok=true only if EVERY stage applied.
inline LiveSwapResult apply_live_swap(const std::vector<SwapStage>& stages) {
    std::vector<const SwapStage*> applied;
    applied.reserve(stages.size());

    auto unwind = [&](LiveSwapResult result) {
        // Roll back the applied stages in reverse; a throwing rollback must not
        // abort the unwind of the others (best-effort; flag it).
        for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
            if (!(*it)->rollback) continue;
            try {
                (*it)->rollback();
            } catch (...) {
                result.rollback_clean = false;
            }
        }
        return result;
    };

    for (const auto& stage : stages) {
        bool ok = false;
        try {
            ok = stage.apply ? stage.apply() : true;
        } catch (const std::exception& e) {
            return unwind({false, stage.name, std::string("threw: ") + e.what(), true});
        } catch (...) {
            return unwind({false, stage.name, "threw (non-std)", true});
        }
        if (!ok)
            return unwind({false, stage.name, "stage rejected", true});
        applied.push_back(&stage);
    }
    return {true, "", "", true};
}

/// The contract a swappable subsystem implements to join a live-swap transaction
/// (live-swap plan item 2.5). A `SwapUnit` is anything that can be swapped as one
/// half of a unified reload — DSP (a `ReloadSession`), scripted UI (a
/// `ScriptedUiSession`), or a future GPU-resource unit. It produces a `SwapStage`
/// that captures its current state for rollback at the moment `to_stage()` is
/// called; the transaction then applies the stages in order and unwinds on
/// failure (see `apply_live_swap`).
///
/// Implementations MUST honour the same atomicity + ordering contract as
/// `SwapStage`: `apply` leaves no partial state on failure, and units are handed
/// to the transaction easiest-to-roll-back first so the hardest-to-undo unit is
/// last. `to_stage()` is expected to snapshot the pre-swap state now (e.g. the
/// current UI code + widget values, or "keep the current processor") so the
/// returned `rollback` can restore it if a LATER unit fails.
class SwapUnit {
public:
    virtual ~SwapUnit() = default;

    /// Human-readable identity, surfaced in `LiveSwapResult::failed_stage`.
    virtual std::string name() const = 0;

    /// Build the stage for this unit's pending swap, capturing the current state
    /// for rollback. Called once per transaction, before any stage is applied.
    virtual SwapStage to_stage() = 0;
};

/// Run an ordered set of `SwapUnit`s as one transaction: collect each unit's
/// stage (snapshotting current state), then delegate to `apply_live_swap`. Order
/// units easiest-rollback-first (UX before the DSP slot swap). Null entries are
/// skipped.
inline LiveSwapResult apply_live_swap(const std::vector<SwapUnit*>& units) {
    std::vector<SwapStage> stages;
    stages.reserve(units.size());
    for (auto* unit : units) {
        if (unit) stages.push_back(unit->to_stage());
    }
    return apply_live_swap(stages);
}

}  // namespace pulp::format::reload
