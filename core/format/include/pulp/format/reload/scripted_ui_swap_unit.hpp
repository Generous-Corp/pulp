#pragma once

/// @file scripted_ui_swap_unit.hpp
/// UX SwapUnit adapter over ScriptedUiSession (live-swap plan item 1.8b/2.5b).
///
/// Adapts a scripted-UI reload to the SwapUnit contract so a content pack's UI
/// swap composes with a DSP swap (DspReloadSwapUnit) under apply_live_swap. Lives
/// in the format layer because it bridges the SwapUnit contract (format) to
/// ScriptedUiSession (view), and format depends on view (not the reverse). Kept
/// in its OWN header so DSP-only users of reload_swap_units.hpp don't pull in view.
///
/// Ordering: the UX unit goes FIRST in a UX+DSP transaction (cheap to re-apply),
/// so if the DSP stage (last) fails, this unit's rollback re-applies the previous
/// script. reload_from() is itself last-good (a bad script keeps the current UI),
/// so apply is atomic; rollback re-points at the pre-transaction bundle.

#include <pulp/format/reload/live_swap_transaction.hpp>
#include <pulp/view/scripted_ui.hpp>

#include <filesystem>
#include <string>
#include <utility>

namespace pulp::format::reload {

class ScriptedUiSwapUnit : public SwapUnit {
public:
    ScriptedUiSwapUnit(view::ScriptedUiSession& session,
                       std::filesystem::path new_script,
                       std::string stage_name = "ux")
        : session_(session),
          new_script_(std::move(new_script)),
          name_(std::move(stage_name)) {}

    std::string name() const override { return name_; }

    SwapStage to_stage() override {
        prev_script_ = session_.script_path();   // pre-transaction UI, for rollback
        return SwapStage{
            name_,
            [this] { return session_.reload_from(new_script_); },
            [this] {
                // Re-apply the previous bundle if we actually changed it. (No-op
                // when the swap targets the same path.) reload_from is last-good,
                // so a failed rollback leaves a valid UI rather than a broken one.
                if (prev_script_ != new_script_)
                    (void)session_.reload_from(prev_script_);
            }};
    }

private:
    view::ScriptedUiSession& session_;
    std::filesystem::path new_script_;
    std::filesystem::path prev_script_;
    std::string name_;
};

}  // namespace pulp::format::reload
