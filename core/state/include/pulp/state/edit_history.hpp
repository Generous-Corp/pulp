#pragma once

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
    void perform(std::unique_ptr<EditAction> action) {
        action->redo();
        // Coalesce: if same description as last action, replace it
        if (coalesce_ && !undo_stack_.empty() &&
            !action->description().empty() &&
            undo_stack_.back()->description() == action->description()) {
            undo_stack_.back() = std::move(action);
            return;
        }
        // Clear any redo actions
        redo_stack_.clear();
        undo_stack_.push_back(std::move(action));
        // Enforce depth limit
        while (undo_stack_.size() > max_depth_)
            undo_stack_.erase(undo_stack_.begin());
    }

    /// Enable/disable coalescing (merge rapid changes with same description).
    void set_coalesce(bool enabled) { coalesce_ = enabled; }

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

    void set_max_depth(size_t depth) { max_depth_ = depth; }
    size_t max_depth() const { return max_depth_; }

private:
    std::vector<std::unique_ptr<EditAction>> undo_stack_;
    std::vector<std::unique_ptr<EditAction>> redo_stack_;
    size_t max_depth_;
    bool coalesce_ = true;  ///< Merge rapid changes with same description
};

} // namespace pulp::state
