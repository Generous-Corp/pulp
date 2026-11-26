#pragma once

/// @file scripted_ui_swap_unit.hpp
/// A live-swap stage that moves a Vellum scripted editor to another script.
///
/// Adapts `vellum::view::ScriptedUiSession::reload_from()` to Pulp's
/// `format::reload::SwapUnit` contract, so a content pack's UI swap composes
/// with a DSP swap (`DspReloadSwapUnit`) under `apply_live_swap()`.
///
/// Ordering: put this unit first in a UI plus DSP transaction. It is cheap to
/// re-apply, so if the DSP stage (last) fails, its rollback re-points the
/// session at the script it ran before the transaction. `reload_from()` keeps
/// the last good UI when a script fails to evaluate, so a failed apply or a
/// failed rollback leaves a working editor rather than a broken one.
///
/// Call it on the editor's UI thread, the thread that owns the session.

#include <pulp/format/reload/live_swap_transaction.hpp>

#include <vellum/view/scripted_ui.hpp>

#include <filesystem>
#include <string>
#include <utility>

namespace pulp::vellum {

class ScriptedUiSwapUnit : public format::reload::SwapUnit {
  public:
    ScriptedUiSwapUnit(::vellum::view::ScriptedUiSession& session, std::filesystem::path new_script,
                       std::string stage_name = "ux")
        : session_(session), new_script_(std::move(new_script)), name_(std::move(stage_name)) {}

    std::string name() const override {
        return name_;
    }

    format::reload::SwapStage to_stage() override {
        prev_script_ = session_.script_path(); // the UI before the transaction, for rollback
        return format::reload::SwapStage{name_,
                                         [this] { return session_.reload_from(new_script_); },
                                         [this] {
                                             // Nothing to undo when the swap targets the
                                             // script already running.
                                             if (prev_script_ != new_script_)
                                                 (void)session_.reload_from(prev_script_);
                                         }};
    }

  private:
    ::vellum::view::ScriptedUiSession& session_;
    std::filesystem::path new_script_;
    std::filesystem::path prev_script_;
    std::string name_;
};

} // namespace pulp::vellum
