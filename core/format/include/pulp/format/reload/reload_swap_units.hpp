#pragma once

/// @file reload_swap_units.hpp
/// Real SwapUnit adapters over the reload sessions (live-swap plan item 1.8b/2.5b).
///
/// The `SwapUnit` contract (live_swap_transaction.hpp) lets heterogeneous
/// subsystems join one atomic live-swap transaction. This header adapts the DSP
/// reload path (`ReloadSession`) to it, so a content pack's DSP swap composes
/// with a UI swap under `apply_live_swap`. The UX side (a `ScriptedUiSession`
/// adapter) lives in the view layer — this file is format-only.

#include <pulp/format/reload/live_swap_transaction.hpp>
#include <pulp/format/reload/reload_transaction.hpp>

#include <string>
#include <utility>

namespace pulp::format::reload {

/// Adapts a `ReloadSession` reload-from-library to a `SwapUnit`. The DSP stage is
/// ordered LAST in a UX+DSP transaction (the hardest to un-swap), so its
/// `rollback` is never invoked — and it needs none: `ReloadSession::reload` is
/// atomic (a rejected candidate leaves the live processor untouched). The unit
/// records the last outcome so the caller can read the reject reason.
class DspReloadSwapUnit : public SwapUnit {
public:
    DspReloadSwapUnit(ReloadSession& session, std::string library_path,
                      std::string stage_name = "dsp")
        : session_(session),
          library_path_(std::move(library_path)),
          name_(std::move(stage_name)) {}

    std::string name() const override { return name_; }

    SwapStage to_stage() override {
        return SwapStage{
            name_,
            [this] {
                last_ = session_.reload(library_path_);
                return last_.ok();
            },
            // No-op: this stage is terminal in the transaction ordering (DSP last),
            // and the reload is atomic on failure, so there is nothing to undo.
            [] {}};
    }

    /// The outcome of the most recent apply() (valid after the transaction runs).
    const ReloadOutcome& last_outcome() const { return last_; }

private:
    ReloadSession& session_;
    std::string library_path_;
    std::string name_;
    ReloadOutcome last_{};
};

}  // namespace pulp::format::reload
