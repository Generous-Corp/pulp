#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulp::state {

/// Base class for undoable actions.
struct EditAction {
    virtual ~EditAction() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual std::string description() const { return ""; }
};

/// Convenience action using lambdas.
class LambdaEdit : public EditAction {
public:
    LambdaEdit(std::function<void()> do_fn, std::function<void()> undo_fn,
               std::string desc = "")
        : do_(std::move(do_fn)), undo_(std::move(undo_fn)), desc_(std::move(desc)) {}

    void undo() override { if (undo_) undo_(); }
    void redo() override { if (do_) do_(); }
    std::string description() const override { return desc_; }

private:
    std::function<void()> do_;
    std::function<void()> undo_;
    std::string desc_;
};

/// Global undo/redo history for the application.
/// Supports coalescing (rapid small changes merge into one undo step)
/// and configurable depth limits.
class EditHistory {
public:
    explicit EditHistory(size_t max_depth = 100) : max_depth_(max_depth) {}

    /// Execute an action and add it to the history.
    /// If coalescing is enabled and the last action has the same description,
    /// replaces it (merging rapid small changes into one undo step).
    /// If `set_coalesce_window` is set to a non-zero duration, two
    /// consecutive actions also coalesce when they arrive within that
    /// window even if their descriptions differ.
    void perform(std::unique_ptr<EditAction> action) {
        action->redo();
        // A new edit invalidates redo history even when it coalesces into
        // the current undo step.
        redo_stack_.clear();
        const auto now = Clock::now();
        const bool same_description =
            coalesce_ && !undo_stack_.empty() &&
            !action->description().empty() &&
            undo_stack_.back()->description() == action->description();
        const bool within_window =
            coalesce_window_.count() > 0 && !undo_stack_.empty() &&
            (now - last_perform_time_) <= coalesce_window_;
        if (same_description || (coalesce_ && within_window)) {
            undo_stack_.back() = std::move(action);
            last_perform_time_ = now;
            return;
        }
        undo_stack_.push_back(std::move(action));
        last_perform_time_ = now;
        // Enforce depth limit
        while (undo_stack_.size() > max_depth_)
            undo_stack_.erase(undo_stack_.begin());
    }

    /// Enable/disable description-based coalescing (merge rapid changes
    /// with same description). Defaults to true.
    void set_coalesce(bool enabled) { coalesce_ = enabled; }

    /// Time-window coalescing. When @p window > 0, two consecutive
    /// `perform()` calls that arrive within this duration are merged into
    /// one undo step even if their descriptions differ. Use 0 to disable
    /// (description-only coalescing). 0 by default to preserve existing
    /// behavior.
    void set_coalesce_window(std::chrono::milliseconds window) {
        coalesce_window_ = window;
    }
    std::chrono::milliseconds coalesce_window() const { return coalesce_window_; }

    /// Convenience: perform a lambda-based action.
    void perform(std::function<void()> do_fn, std::function<void()> undo_fn,
                 const std::string& desc = "") {
        perform(std::make_unique<LambdaEdit>(std::move(do_fn), std::move(undo_fn), desc));
    }

    /// Undo the most recent action.
    bool undo() {
        if (undo_stack_.empty()) return false;
        auto action = std::move(undo_stack_.back());
        undo_stack_.pop_back();
        action->undo();
        redo_stack_.push_back(std::move(action));
        return true;
    }

    /// Redo the most recently undone action.
    bool redo() {
        if (redo_stack_.empty()) return false;
        auto action = std::move(redo_stack_.back());
        redo_stack_.pop_back();
        action->redo();
        undo_stack_.push_back(std::move(action));
        return true;
    }

    bool can_undo() const { return !undo_stack_.empty(); }
    bool can_redo() const { return !redo_stack_.empty(); }
    size_t undo_count() const { return undo_stack_.size(); }
    size_t redo_count() const { return redo_stack_.size(); }

    /// Clear all history.
    void clear() {
        undo_stack_.clear();
        redo_stack_.clear();
    }

    /// Get the description of the next undo action.
    std::string undo_description() const {
        return undo_stack_.empty() ? "" : undo_stack_.back()->description();
    }

    void set_max_depth(size_t depth) {
        max_depth_ = depth;
        while (undo_stack_.size() > max_depth_)
            undo_stack_.erase(undo_stack_.begin());
    }
    size_t max_depth() const { return max_depth_; }

private:
    using Clock = std::chrono::steady_clock;

    std::vector<std::unique_ptr<EditAction>> undo_stack_;
    std::vector<std::unique_ptr<EditAction>> redo_stack_;
    size_t max_depth_;
    bool coalesce_ = true;  ///< Merge rapid changes with same description
    std::chrono::milliseconds coalesce_window_{0};
    Clock::time_point last_perform_time_{};
};

} // namespace pulp::state
