#pragma once

/// @file undo_manager.hpp
/// Generic undo/redo system with named actions and transaction grouping.

#include <chrono>
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace pulp::state {

/// A single undoable action with undo and redo functions.
///
/// @code
/// auto action = UndoAction::create("Set gain to -6dB",
///     [=] { store.set_value(kGain, old_value); },  // undo
///     [=] { store.set_value(kGain, new_value); }   // redo
/// );
/// undo_manager.perform(std::move(action));
/// @endcode
struct UndoAction {
    std::string name;
    std::function<void()> undo_fn;
    std::function<void()> redo_fn;

    static std::unique_ptr<UndoAction> create(
        std::string action_name,
        std::function<void()> undo,
        std::function<void()> redo)
    {
        auto a = std::make_unique<UndoAction>();
        a->name = std::move(action_name);
        a->undo_fn = std::move(undo);
        a->redo_fn = std::move(redo);
        return a;
    }
};

/// A group of actions that undo/redo together as one step.
struct UndoTransaction {
    std::string name;
    std::vector<std::unique_ptr<UndoAction>> actions;
};

/// Manages undo/redo history with configurable depth.
///
/// Supports single actions, transaction grouping (begin/end),
/// and named action descriptions for menu display.
///
/// @code
/// UndoManager undo;
/// undo.set_max_history(100);
///
/// // Simple action
/// undo.perform(UndoAction::create("Change color",
///     [=] { set_color(old_color); },
///     [=] { set_color(new_color); }));
///
/// // Transaction (multiple actions as one undo step)
/// undo.begin_transaction("Move and resize");
/// undo.perform(UndoAction::create("Move", undo_move, redo_move));
/// undo.perform(UndoAction::create("Resize", undo_resize, redo_resize));
/// undo.end_transaction();
///
/// undo.undo();  // undoes entire "Move and resize"
/// undo.redo();  // redoes entire "Move and resize"
/// @endcode
class UndoManager {
public:
    /// Perform an action and add it to the undo history.
    /// The action's redo function is called immediately.
    ///
    /// When `set_coalesce_window` is non-zero and this call arrives
    /// within that window of the previous outside-transaction `perform()`,
    /// the new action is appended to the previous transaction rather than
    /// creating a new top-level entry. This produces single-step undo for
    /// rapid sequences (e.g., a slider drag).
    void perform(std::unique_ptr<UndoAction> action) {
        if (action->redo_fn) action->redo_fn();

        if (in_transaction_) {
            current_transaction_->actions.push_back(std::move(action));
            return;
        }

        const auto now = Clock::now();
        if (coalesce_window_.count() > 0 && !undo_stack_.empty() &&
            (now - last_perform_time_) <= coalesce_window_) {
            // A new edit invalidates redo history even when it merges into
            // the prior transaction — otherwise undo+new-edit could leave
            // stale redo entries that no longer make sense.
            redo_stack_.clear();
            undo_stack_.back().actions.push_back(std::move(action));
            last_perform_time_ = now;
            if (on_state_changed) on_state_changed();
            return;
        }

        UndoTransaction tx;
        tx.name = action->name;
        tx.actions.push_back(std::move(action));
        push_undo(std::move(tx));
        last_perform_time_ = now;
    }

    /// Perform an action without immediately executing it.
    /// Use when you've already applied the change.
    void add_without_executing(std::unique_ptr<UndoAction> action) {
        if (in_transaction_) {
            current_transaction_->actions.push_back(std::move(action));
        } else {
            UndoTransaction tx;
            tx.name = action->name;
            tx.actions.push_back(std::move(action));
            push_undo(std::move(tx));
        }
    }

    /// Begin a transaction — all subsequent perform() calls are grouped.
    void begin_transaction(const std::string& name) {
        current_transaction_ = std::make_unique<UndoTransaction>();
        current_transaction_->name = name;
        in_transaction_ = true;
    }

    /// End the current transaction and push it to the undo stack.
    void end_transaction() {
        if (in_transaction_ && current_transaction_ && !current_transaction_->actions.empty()) {
            push_undo(std::move(*current_transaction_));
        }
        current_transaction_.reset();
        in_transaction_ = false;
    }

    /// Cancel the current transaction (discard without pushing).
    void cancel_transaction() {
        // Undo all actions in the transaction
        if (in_transaction_ && current_transaction_) {
            for (auto it = current_transaction_->actions.rbegin();
                 it != current_transaction_->actions.rend(); ++it) {
                if ((*it)->undo_fn) (*it)->undo_fn();
            }
        }
        current_transaction_.reset();
        in_transaction_ = false;
    }

    /// Undo the most recent action/transaction.
    bool undo() {
        if (undo_stack_.empty()) return false;

        auto tx = std::move(undo_stack_.back());
        undo_stack_.pop_back();

        // Undo actions in reverse order
        for (auto it = tx.actions.rbegin(); it != tx.actions.rend(); ++it) {
            if ((*it)->undo_fn) (*it)->undo_fn();
        }

        redo_stack_.push_back(std::move(tx));
        // Reset the coalesce timestamp so the next edit starts a fresh
        // window — a `perform()` after `undo()` must never merge into
        // the older entry that's still on the undo stack.
        last_perform_time_ = Clock::time_point{};
        if (on_state_changed) on_state_changed();
        return true;
    }

    /// Redo the most recently undone action/transaction.
    bool redo() {
        if (redo_stack_.empty()) return false;

        auto tx = std::move(redo_stack_.back());
        redo_stack_.pop_back();

        // Redo actions in forward order
        for (auto& action : tx.actions) {
            if (action->redo_fn) action->redo_fn();
        }

        undo_stack_.push_back(std::move(tx));
        last_perform_time_ = Clock::time_point{};
        if (on_state_changed) on_state_changed();
        return true;
    }

    /// Check if undo is available.
    bool can_undo() const { return !undo_stack_.empty(); }

    /// Check if redo is available.
    bool can_redo() const { return !redo_stack_.empty(); }

    /// Get the name of the next undo action (for menu display).
    std::string undo_name() const {
        return undo_stack_.empty() ? "" : undo_stack_.back().name;
    }

    /// Get the name of the next redo action.
    std::string redo_name() const {
        return redo_stack_.empty() ? "" : redo_stack_.back().name;
    }

    /// Number of undo steps available.
    int undo_count() const { return static_cast<int>(undo_stack_.size()); }

    /// Number of redo steps available.
    int redo_count() const { return static_cast<int>(redo_stack_.size()); }

    /// Clear all undo/redo history.
    void clear() {
        undo_stack_.clear();
        redo_stack_.clear();
        if (on_state_changed) on_state_changed();
    }

    /// Set maximum undo history depth. Oldest entries are discarded.
    void set_max_history(int max) {
        max_history_ = max < 0 ? 0 : max;
        trim_to_max_history();
    }
    int max_history() const { return max_history_; }

    /// Called whenever the undo/redo state changes.
    std::function<void()> on_state_changed;

    /// Time-window coalescing. When @p window > 0, two consecutive
    /// `perform()` calls outside any explicit transaction that arrive
    /// within this duration are merged into one undo step. 0 disables
    /// (each `perform()` becomes its own undo step). 0 by default to
    /// preserve existing behavior.
    void set_coalesce_window(std::chrono::milliseconds window) {
        coalesce_window_ = window;
    }
    std::chrono::milliseconds coalesce_window() const { return coalesce_window_; }

private:
    using Clock = std::chrono::steady_clock;

    std::vector<UndoTransaction> undo_stack_;
    std::vector<UndoTransaction> redo_stack_;
    int max_history_ = 100;
    bool in_transaction_ = false;
    std::unique_ptr<UndoTransaction> current_transaction_;
    std::chrono::milliseconds coalesce_window_{0};
    Clock::time_point last_perform_time_{};

    void push_undo(UndoTransaction tx) {
        redo_stack_.clear(); // new action invalidates redo
        undo_stack_.push_back(std::move(tx));
        trim_to_max_history();
        if (on_state_changed) on_state_changed();
    }

    void trim_to_max_history() {
        while (static_cast<int>(undo_stack_.size()) > max_history_) {
            undo_stack_.erase(undo_stack_.begin());
        }
    }
};

} // namespace pulp::state
