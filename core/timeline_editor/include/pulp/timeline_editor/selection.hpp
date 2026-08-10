#pragma once

/// @file selection.hpp
/// Non-durable selection state for timeline editors.

#include <pulp/timeline/model.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <vector>

namespace pulp::timeline_editor {

/** @addtogroup timeline_editing
 * @{
 */

/// Identity-only editor state that never enters the document or its undo history.
class Selection {
  public:
    /// Returns selected identities in ascending order without duplicates.
    std::span<const timeline::ItemId> items() const noexcept {
        return items_;
    }
    /// Returns the identity from which range extension begins, when one exists.
    std::optional<timeline::ItemId> anchor() const noexcept {
        return anchor_;
    }
    /// Returns whether `item` is selected.
    bool contains(timeline::ItemId item) const noexcept {
        return std::binary_search(items_.begin(), items_.end(), item);
    }
    /// Returns whether no identities are selected.
    bool empty() const noexcept {
        return items_.empty();
    }

    /// Clears the selected identities and range anchor.
    void clear() noexcept {
        items_.clear();
        anchor_.reset();
    }

    /// Replaces the selection, retaining the first valid input as the range anchor.
    void replace(std::span<const timeline::ItemId> items) {
        anchor_.reset();
        items_.clear();
        items_.reserve(items.size());
        for (const auto item : items) {
            if (!item.valid())
                continue;
            if (!anchor_)
                anchor_ = item;
            items_.push_back(item);
        }
        canonicalize();
    }

    /// Adds one identity while preserving an existing range anchor.
    void add(timeline::ItemId item) {
        if (!item.valid() || contains(item))
            return;
        items_.insert(std::lower_bound(items_.begin(), items_.end(), item), item);
        if (!anchor_)
            anchor_ = item;
    }

    /// Adds an absent identity or removes a selected one.
    void toggle(timeline::ItemId item) {
        if (!item.valid())
            return;
        const auto found = std::lower_bound(items_.begin(), items_.end(), item);
        if (found == items_.end() || *found != item) {
            items_.insert(found, item);
            if (!anchor_)
                anchor_ = item;
            return;
        }
        items_.erase(found);
        if (anchor_ == item)
            anchor_ =
                items_.empty() ? std::nullopt : std::optional<timeline::ItemId>{items_.front()};
    }

    /// Replaces the selection with the inclusive ordered range from the anchor to `item`.
    ///
    /// Returns false when `item`, or an existing anchor, is absent from `ordered_items`.
    /// With no anchor, a present `item` becomes the sole selection and establishes one.
    bool extend_from_anchor(timeline::ItemId item,
                            std::span<const timeline::ItemId> ordered_items) {
        if (!item.valid())
            return false;
        const auto target = std::find(ordered_items.begin(), ordered_items.end(), item);
        if (target == ordered_items.end())
            return false;
        if (!anchor_) {
            replace(std::span<const timeline::ItemId>(&*target, 1));
            return true;
        }
        const auto start = std::find(ordered_items.begin(), ordered_items.end(), *anchor_);
        if (start == ordered_items.end())
            return false;

        const auto first = std::min(start, target);
        const auto last = std::max(start, target);
        items_.assign(first, last + 1);
        canonicalize();
        return true;
    }

    /// Removes identities missing from `project` or retained only as inactive tombstones.
    void prune(const timeline::Project& project) {
        const auto dead = [&](timeline::ItemId item) {
            const auto location = project.locate(item);
            return !location || !location->active;
        };
        items_.erase(std::remove_if(items_.begin(), items_.end(), dead), items_.end());
        if (anchor_ && dead(*anchor_))
            anchor_.reset();
    }

  private:
    void canonicalize() {
        items_.erase(std::remove_if(items_.begin(), items_.end(),
                                    [](timeline::ItemId item) { return !item.valid(); }),
                     items_.end());
        std::sort(items_.begin(), items_.end());
        items_.erase(std::unique(items_.begin(), items_.end()), items_.end());
    }

    std::vector<timeline::ItemId> items_;
    std::optional<timeline::ItemId> anchor_;
};

/// @}

} // namespace pulp::timeline_editor
